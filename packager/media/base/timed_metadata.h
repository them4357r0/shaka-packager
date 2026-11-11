// Copyright 2026 media-platform contributors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_BASE_TIMED_METADATA_H_
#define PACKAGER_MEDIA_BASE_TIMED_METADATA_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace shaka {
namespace media {

// Out-of-band timed metadata (ID3, SCTE-35) supplied through
// --timed_metadata_input, in the line-oriented MPTM1 record format:
//   MPTM1 anchor <pts90k> <unix_ms>
//   MPTM1 id3    <pts90k> <dur|-> <id> <base64>
//   MPTM1 scte35 <pts90k> <dur|-> <OUT|IN|CMD|NONE> <id> <base64>
// All times are in the 90 kHz MPEG timebase of the media being packaged.

constexpr size_t kTimedMetadataMaxLineBytes = 8 * 1024;
constexpr int32_t kTimedMetadataTimeScale = 90000;

enum class TimedMetadataKind { kId3, kScte35 };
enum class Scte35Class { kNone, kOut, kIn, kCmd };

struct TimedMetadataEvent {
  // Arrival order in the store; assigned by TimedMetadataStore::AddEvent.
  uint64_t seq = 0;
  TimedMetadataKind kind = TimedMetadataKind::kId3;
  int64_t pts_90k = 0;
  // Negative means unknown/not given.
  int64_t duration_90k = -1;
  Scte35Class scte35_class = Scte35Class::kNone;
  std::string id;
  std::vector<uint8_t> payload;
  // emsg id assigned by the store so that no two retained events of one kind
  // share an id (DASH treats equal scheme/value/id as redundant copies). Unset
  // outside a store; TimedMetadataEmsgId(id) is then used.
  std::optional<uint32_t> emsg_id;
};

struct TimedMetadataAnchor {
  int64_t pts_90k = 0;
  int64_t unix_ms = 0;
};

struct TimedMetadataRecord {
  enum class Type { kAnchor, kEvent };
  Type type = Type::kEvent;
  TimedMetadataAnchor anchor;
  TimedMetadataEvent event;
};

enum class TimedMetadataParseResult { kOk, kMalformed, kUnknownRecord };

/// Parses one MPTM1 line (without line terminator).
TimedMetadataParseResult ParseTimedMetadataLine(std::string_view line,
                                                TimedMetadataRecord* record,
                                                std::string* error);

/// Event id used in emsg boxes: the decimal value if |id| is a decimal number
/// that fits in 32 bits, otherwise a 32 bit FNV-1a hash of |id|.
uint32_t TimedMetadataEmsgId(const std::string& id);

/// Bounded, thread-safe store shared by all output streams. Events are
/// retained (not consumed) so every stream can select its own events.
class TimedMetadataStore {
 public:
  static constexpr size_t kDefaultMaxEvents = 4096;
  explicit TimedMetadataStore(size_t max_events = kDefaultMaxEvents);

  /// Sets the anchor. Only the first anchor is kept; returns false if one
  /// was already set.
  bool SetAnchor(const TimedMetadataAnchor& anchor);
  std::optional<TimedMetadataAnchor> anchor() const;
  /// Waits until an anchor is set or |timeout_ms| elapsed.
  bool WaitForAnchor(int64_t timeout_ms) const;

  /// Adds an event. Events with an already known (kind, id, scte35 class) are
  /// ignored and false is returned: a splice_insert OUT and its IN (and a CMD)
  /// legitimately share one id, an exact re-send does not add anything. Events
  /// of one kind that would share an emsg id get distinct ids (the first
  /// keeps the plain derivation; later ones are salted with the class). The oldest event is evicted when the store is full.
  bool AddEvent(TimedMetadataEvent event);

  /// Signals that the input reached EOF (or failed); no more events follow.
  void MarkInputEnded();
  bool input_ended() const;

  size_t size() const;
  uint64_t next_seq() const;
  uint64_t evicted_count() const;

  /// Selects, in (pts, seq) order, events whose presentation time (90 kHz)
  /// scaled to |timescale| lies in [start, end), plus, when |late_from_seq|
  /// is set, events with a presentation time before |start| whose seq is
  /// >= *late_from_seq (they arrived after the previous selection). Sets
  /// |*next_seq| to the next sequence number at selection time.
  std::vector<TimedMetadataEvent> Select(int64_t start,
                                         int64_t end,
                                         int32_t timescale,
                                         std::optional<uint64_t> late_from_seq,
                                         uint64_t* next_seq) const;

 private:
  const size_t max_events_;
  mutable std::mutex mutex_;
  mutable std::condition_variable anchor_cv_;
  std::optional<TimedMetadataAnchor> anchor_;
  std::vector<TimedMetadataEvent> events_;
  uint64_t next_seq_ = 1;
  uint64_t evicted_ = 0;
  bool input_ended_ = false;
};

/// Per-consumer (one per output stream / playlist) view of the store. Each
/// event is returned for a time range once; events that arrive after the
/// range they belong to are returned with the next range and logged.
class TimedMetadataCursor {
 public:
  explicit TimedMetadataCursor(std::shared_ptr<TimedMetadataStore> store);

  /// |start| and |duration| are in |timescale| ticks per second.
  std::vector<TimedMetadataEvent> Take(int64_t start,
                                       int64_t duration,
                                       int32_t timescale);

 private:
  std::shared_ptr<TimedMetadataStore> store_;
  std::optional<uint64_t> high_water_;
};

/// Process-wide store set by Packager when --timed_metadata_input is used.
/// Null when the feature is off (behaviour is then unchanged).
std::shared_ptr<TimedMetadataStore> GetGlobalTimedMetadataStore();
void SetGlobalTimedMetadataStore(std::shared_ptr<TimedMetadataStore> store);

/// Reads MPTM1 records from a FIFO or regular file on its own thread and fills
/// a store. EOF ends the metadata only; it never affects packaging.
class TimedMetadataReader {
 public:
  TimedMetadataReader();
  ~TimedMetadataReader();

  void Start(const std::string& path, std::shared_ptr<TimedMetadataStore> store);
  void Stop();

  /// Splits |data| into lines and feeds records to |store|. Exposed for tests;
  /// |pending| carries a partial trailing line (and the overlong-line
  /// discard state) between calls.
  struct LineState {
    std::string pending;
    bool discarding = false;
  };
  static void ConsumeBytes(const char* data,
                           size_t size,
                           LineState* state,
                           TimedMetadataStore* store);
  static void ConsumeLine(std::string_view line, TimedMetadataStore* store);

 private:
  void Run(std::string path);

  std::shared_ptr<TimedMetadataStore> store_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_BASE_TIMED_METADATA_H_
