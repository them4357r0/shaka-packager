// Copyright 2026 media-platform contributors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/base/timed_metadata.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include <absl/log/log.h>
#include <absl/strings/escaping.h>
#include <absl/strings/numbers.h>
#include <absl/strings/str_split.h>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace shaka {
namespace media {

namespace {

constexpr int64_t kMaxPts90k = int64_t{1} << 40;
constexpr size_t kMaxIdLength = 128;

bool ParseInt64(std::string_view s, int64_t* out) {
  return absl::SimpleAtoi(s, out);
}

bool ValidId(std::string_view id) {
  if (id.empty() || id.size() > kMaxIdLength)
    return false;
  for (char c : id) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u <= 0x20 || u >= 0x7f || c == '"' || c == '\\')
      return false;
  }
  return true;
}

bool ParseDuration(std::string_view s, int64_t* out) {
  if (s == "-") {
    *out = -1;
    return true;
  }
  int64_t v;
  if (!ParseInt64(s, &v) || v < 0 || v >= kMaxPts90k)
    return false;
  *out = v;
  return true;
}

bool ParsePayload(std::string_view s, std::vector<uint8_t>* out) {
  std::string raw;
  if (s.empty() || !absl::Base64Unescape(s, &raw) || raw.empty())
    return false;
  out->assign(raw.begin(), raw.end());
  return true;
}

std::mutex g_global_mutex;
std::shared_ptr<TimedMetadataStore> g_global_store;

}  // namespace

TimedMetadataParseResult ParseTimedMetadataLine(std::string_view line,
                                                TimedMetadataRecord* record,
                                                std::string* error) {
  auto fail = [&](const char* msg) {
    if (error)
      *error = msg;
    return TimedMetadataParseResult::kMalformed;
  };
  if (line.size() > kTimedMetadataMaxLineBytes)
    return fail("line too long");
  std::vector<std::string_view> t = absl::StrSplit(line, ' ');
  if (t.size() < 2 || t[0] != "MPTM1")
    return fail("missing MPTM1 prefix");

  TimedMetadataRecord r;
  if (t[1] == "anchor") {
    if (t.size() != 4)
      return fail("anchor: wrong field count");
    r.type = TimedMetadataRecord::Type::kAnchor;
    if (!ParseInt64(t[2], &r.anchor.pts_90k) || r.anchor.pts_90k < 0 ||
        r.anchor.pts_90k >= kMaxPts90k)
      return fail("anchor: bad pts");
    if (!ParseInt64(t[3], &r.anchor.unix_ms) || r.anchor.unix_ms <= 0)
      return fail("anchor: bad unix_ms");
    *record = std::move(r);
    return TimedMetadataParseResult::kOk;
  }

  size_t payload_index;
  if (t[1] == "id3") {
    if (t.size() != 6)
      return fail("id3: wrong field count");
    r.event.kind = TimedMetadataKind::kId3;
    r.event.scte35_class = Scte35Class::kNone;
    if (!ValidId(t[4]))
      return fail("id3: bad id");
    r.event.id = std::string(t[4]);
    payload_index = 5;
  } else if (t[1] == "scte35") {
    if (t.size() != 7)
      return fail("scte35: wrong field count");
    r.event.kind = TimedMetadataKind::kScte35;
    if (t[4] == "OUT")
      r.event.scte35_class = Scte35Class::kOut;
    else if (t[4] == "IN")
      r.event.scte35_class = Scte35Class::kIn;
    else if (t[4] == "CMD")
      r.event.scte35_class = Scte35Class::kCmd;
    else if (t[4] == "NONE")
      r.event.scte35_class = Scte35Class::kNone;
    else
      return fail("scte35: bad class");
    if (!ValidId(t[5]))
      return fail("scte35: bad id");
    r.event.id = std::string(t[5]);
    payload_index = 6;
  } else {
    if (error)
      *error = "unknown record type";
    return TimedMetadataParseResult::kUnknownRecord;
  }
  r.type = TimedMetadataRecord::Type::kEvent;
  if (!ParseInt64(t[2], &r.event.pts_90k) || r.event.pts_90k < 0 ||
      r.event.pts_90k >= kMaxPts90k)
    return fail("bad pts");
  if (!ParseDuration(t[3], &r.event.duration_90k))
    return fail("bad duration");
  if (!ParsePayload(t[payload_index], &r.event.payload))
    return fail("bad base64 payload");
  *record = std::move(r);
  return TimedMetadataParseResult::kOk;
}

uint32_t TimedMetadataEmsgId(const std::string& id) {
  uint32_t v;
  if (absl::SimpleAtoi(id, &v) && std::to_string(v) == id)
    return v;
  uint32_t h = 2166136261u;
  for (unsigned char c : id) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

// ---------------------------------------------------------------------------

TimedMetadataStore::TimedMetadataStore(size_t max_events)
    : max_events_(std::max<size_t>(1, max_events)) {}

bool TimedMetadataStore::SetAnchor(const TimedMetadataAnchor& anchor) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (anchor_)
      return false;
    anchor_ = anchor;
  }
  anchor_cv_.notify_all();
  return true;
}

std::optional<TimedMetadataAnchor> TimedMetadataStore::anchor() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return anchor_;
}

bool TimedMetadataStore::WaitForAnchor(int64_t timeout_ms) const {
  std::unique_lock<std::mutex> lock(mutex_);
  return anchor_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                             [this] { return anchor_.has_value() || input_ended_; }) &&
         anchor_.has_value();
}

bool TimedMetadataStore::AddEvent(TimedMetadataEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& e : events_) {
    if (e.kind == event.kind && e.id == event.id &&
        e.scte35_class == event.scte35_class)
      return false;
  }
  {
    auto taken = [&](uint32_t v) {
      for (const auto& e : events_) {
        if (e.kind == event.kind &&
            e.emsg_id.value_or(TimedMetadataEmsgId(e.id)) == v)
          return true;
      }
      return false;
    };
    uint32_t candidate = TimedMetadataEmsgId(event.id);
    if (taken(candidate)) {
      static const char* const kNames[] = {"NONE", "OUT", "IN", "CMD"};
      candidate = TimedMetadataEmsgId(
          event.id + "/" + kNames[static_cast<int>(event.scte35_class)]);
      while (taken(candidate))
        ++candidate;
    }
    event.emsg_id = candidate;
  }
  if (events_.size() >= max_events_) {
    events_.erase(events_.begin());
    ++evicted_;
  }
  event.seq = next_seq_++;
  events_.push_back(std::move(event));
  return true;
}

void TimedMetadataStore::MarkInputEnded() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    input_ended_ = true;
  }
  anchor_cv_.notify_all();
}

bool TimedMetadataStore::input_ended() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return input_ended_;
}

size_t TimedMetadataStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_.size();
}

uint64_t TimedMetadataStore::next_seq() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_seq_;
}

uint64_t TimedMetadataStore::evicted_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return evicted_;
}

std::vector<TimedMetadataEvent> TimedMetadataStore::Select(
    int64_t start,
    int64_t end,
    int32_t timescale,
    std::optional<uint64_t> late_from_seq,
    uint64_t* next_seq) const {
  std::vector<TimedMetadataEvent> out;
  // Compare p / 90000 against t / timescale without division:
  // p * timescale  vs  t * 90000.
  const int64_t lo = start * kTimedMetadataTimeScale;
  const int64_t hi = end * kTimedMetadataTimeScale;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& e : events_) {
    const int64_t scaled = e.pts_90k * timescale;
    const bool in_range = scaled >= lo && scaled < hi;
    const bool late =
        scaled < lo && late_from_seq.has_value() && e.seq >= *late_from_seq;
    if (in_range || late)
      out.push_back(e);
  }
  if (next_seq)
    *next_seq = next_seq_;
  std::stable_sort(out.begin(), out.end(),
                   [](const TimedMetadataEvent& a, const TimedMetadataEvent& b) {
                     if (a.pts_90k != b.pts_90k)
                       return a.pts_90k < b.pts_90k;
                     return a.seq < b.seq;
                   });
  return out;
}

// ---------------------------------------------------------------------------

TimedMetadataCursor::TimedMetadataCursor(
    std::shared_ptr<TimedMetadataStore> store)
    : store_(std::move(store)) {}

std::vector<TimedMetadataEvent> TimedMetadataCursor::Take(int64_t start,
                                                          int64_t duration,
                                                          int32_t timescale) {
  if (!store_ || timescale <= 0)
    return {};
  uint64_t next_seq = 0;
  // The very first range does not claim events from before the stream start.
  auto events = store_->Select(start, start + std::max<int64_t>(duration, 0),
                               timescale, high_water_, &next_seq);
  high_water_ = next_seq;
  for (const auto& e : events) {
    if (e.pts_90k * timescale < start * kTimedMetadataTimeScale) {
      LOG(WARNING) << "Timed metadata event " << e.id << " at pts "
                   << e.pts_90k << " arrived late; attached to the segment at "
                   << start << "/" << timescale;
    }
  }
  return events;
}

std::shared_ptr<TimedMetadataStore> GetGlobalTimedMetadataStore() {
  std::lock_guard<std::mutex> lock(g_global_mutex);
  return g_global_store;
}

void SetGlobalTimedMetadataStore(std::shared_ptr<TimedMetadataStore> store) {
  std::lock_guard<std::mutex> lock(g_global_mutex);
  g_global_store = std::move(store);
}

// ---------------------------------------------------------------------------

TimedMetadataReader::TimedMetadataReader() = default;

TimedMetadataReader::~TimedMetadataReader() {
  Stop();
}

void TimedMetadataReader::Start(const std::string& path,
                                std::shared_ptr<TimedMetadataStore> store) {
  store_ = std::move(store);
  stop_ = false;
  thread_ = std::thread([this, path] { Run(path); });
}

void TimedMetadataReader::Stop() {
  stop_ = true;
  if (thread_.joinable())
    thread_.join();
}

void TimedMetadataReader::ConsumeLine(std::string_view line,
                                      TimedMetadataStore* store) {
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1);
  if (line.empty())
    return;
  TimedMetadataRecord record;
  std::string error;
  switch (ParseTimedMetadataLine(line, &record, &error)) {
    case TimedMetadataParseResult::kOk:
      if (record.type == TimedMetadataRecord::Type::kAnchor) {
        if (!store->SetAnchor(record.anchor))
          LOG(WARNING) << "Timed metadata: ignoring repeated anchor record";
      } else if (!store->AddEvent(std::move(record.event))) {
        LOG(WARNING) << "Timed metadata: duplicate event id ignored";
      }
      break;
    case TimedMetadataParseResult::kMalformed:
      LOG(WARNING) << "Timed metadata: skipping malformed record (" << error
                   << ")";
      break;
    case TimedMetadataParseResult::kUnknownRecord:
      LOG(WARNING) << "Timed metadata: skipping unknown record type";
      break;
  }
}

void TimedMetadataReader::ConsumeBytes(const char* data,
                                       size_t size,
                                       LineState* state,
                                       TimedMetadataStore* store) {
  size_t pos = 0;
  while (pos < size) {
    const char* nl =
        static_cast<const char*>(memchr(data + pos, '\n', size - pos));
    const size_t chunk = nl ? static_cast<size_t>(nl - (data + pos)) : size - pos;
    if (!state->discarding) {
      // The limit applies to the line without its terminator.
      if (state->pending.size() + chunk > kTimedMetadataMaxLineBytes) {
        LOG(WARNING) << "Timed metadata: record exceeds "
                     << kTimedMetadataMaxLineBytes << " bytes, skipping";
        state->pending.clear();
        state->discarding = true;
      } else {
        state->pending.append(data + pos, chunk);
      }
    }
    pos += chunk;
    if (nl) {
      if (!state->discarding)
        ConsumeLine(state->pending, store);
      state->pending.clear();
      state->discarding = false;
      ++pos;  // skip '\n'
    }
  }
}

void TimedMetadataReader::Run(std::string path) {
#if defined(_WIN32)
  LOG(ERROR) << "--timed_metadata_input is not supported on this platform";
  store_->MarkInputEnded();
#else
  int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    LOG(ERROR) << "Timed metadata: cannot open " << path << ": "
               << strerror(errno) << "; continuing without timed metadata";
    store_->MarkInputEnded();
    return;
  }
  LineState state;
  char buf[16 * 1024];
  while (!stop_) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int rc = poll(&pfd, 1, 100);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      LOG(ERROR) << "Timed metadata: poll failed: " << strerror(errno);
      break;
    }
    if (rc == 0)
      continue;
    if ((pfd.revents & POLLIN) == 0 && (pfd.revents & (POLLHUP | POLLERR))) {
      // FIFO whose writers are all gone: end of metadata.
      break;
    }
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      ConsumeBytes(buf, static_cast<size_t>(n), &state, store_.get());
      continue;
    }
    if (n == 0) {
      // Regular file EOF (a FIFO without data would not have been readable).
      break;
    }
    if (errno == EAGAIN || errno == EINTR)
      continue;
    LOG(ERROR) << "Timed metadata: read failed: " << strerror(errno);
    break;
  }
  close(fd);
  if (!stop_) {
    // A final record without a trailing newline is still a record.
    if (!state.discarding && !state.pending.empty())
      ConsumeLine(state.pending, store_.get());
    LOG(INFO) << "Timed metadata input ended; no more events will arrive";
  }
  store_->MarkInputEnded();
#endif
}

}  // namespace media
}  // namespace shaka
