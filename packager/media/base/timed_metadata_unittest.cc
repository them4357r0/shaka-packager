// Copyright 2026 media-platform contributors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/base/timed_metadata.h>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <absl/strings/escaping.h>
#include <gtest/gtest.h>

namespace shaka {
namespace media {
namespace {

std::string B64(const std::string& raw) {
  return absl::Base64Escape(raw);
}

TEST(TimedMetadataParse, Anchor) {
  TimedMetadataRecord r;
  std::string err;
  ASSERT_EQ(TimedMetadataParseResult::kOk,
            ParseTimedMetadataLine("MPTM1 anchor 900000 1700000000123", &r,
                                   &err));
  EXPECT_EQ(TimedMetadataRecord::Type::kAnchor, r.type);
  EXPECT_EQ(900000, r.anchor.pts_90k);
  EXPECT_EQ(1700000000123, r.anchor.unix_ms);
}

TEST(TimedMetadataParse, Id3) {
  TimedMetadataRecord r;
  std::string err;
  const std::string raw = std::string("ID3\x04payload");
  const std::string line = "MPTM1 id3 270000 90000 id-1 " + B64(raw);
  ASSERT_EQ(TimedMetadataParseResult::kOk,
            ParseTimedMetadataLine(line, &r, &err))
      << err;
  EXPECT_EQ(TimedMetadataRecord::Type::kEvent, r.type);
  EXPECT_EQ(TimedMetadataKind::kId3, r.event.kind);
  EXPECT_EQ(270000, r.event.pts_90k);
  EXPECT_EQ(90000, r.event.duration_90k);
  EXPECT_EQ("id-1", r.event.id);
  EXPECT_EQ(raw, std::string(r.event.payload.begin(), r.event.payload.end()));
}

TEST(TimedMetadataParse, Scte35AllClassesAndNoDuration) {
  const struct {
    const char* name;
    Scte35Class cls;
  } kCases[] = {{"OUT", Scte35Class::kOut},
                {"IN", Scte35Class::kIn},
                {"CMD", Scte35Class::kCmd},
                {"NONE", Scte35Class::kNone}};
  for (const auto& c : kCases) {
    TimedMetadataRecord r;
    std::string err;
    const std::string line = "MPTM1 scte35 0 - " + std::string(c.name) +
                             " 42 " + B64(std::string("\xfc\x30"));
    ASSERT_EQ(TimedMetadataParseResult::kOk,
              ParseTimedMetadataLine(line, &r, &err))
        << err;
    EXPECT_EQ(TimedMetadataKind::kScte35, r.event.kind);
    EXPECT_EQ(c.cls, r.event.scte35_class);
    EXPECT_EQ(-1, r.event.duration_90k);
    EXPECT_EQ("42", r.event.id);
  }
}

TEST(TimedMetadataParse, Malformed) {
  const char* kBad[] = {
      "",
      "garbage",
      "MPTM1",
      "MPTM1 anchor 1",
      "MPTM1 anchor x 5",
      "MPTM1 anchor -1 5",
      "MPTM1 anchor 1 0",
      "MPTM1 id3 1 - id",                // no payload
      "MPTM1 id3 1 - id !!!notbase64",   // bad base64
      "MPTM1 id3 1 - id QQ==  extra",    // extra field
      "MPTM1 id3 -4 - id QQ==",          // negative pts
      "MPTM1 id3 1 x id QQ==",           // bad duration
      "MPTM1 scte35 1 - BOGUS id QQ==",  // bad class
      "MPTM1 scte35 1 - OUT id",         // missing payload
      "MPTM1 scte35 1 - OUT i\"d QQ==",  // quote in id
  };
  for (const char* line : kBad) {
    TimedMetadataRecord r;
    std::string err;
    EXPECT_EQ(TimedMetadataParseResult::kMalformed,
              ParseTimedMetadataLine(line, &r, &err))
        << line;
  }
  TimedMetadataRecord r;
  EXPECT_EQ(TimedMetadataParseResult::kUnknownRecord,
            ParseTimedMetadataLine("MPTM1 future 1 2 3", &r, nullptr));
}

TEST(TimedMetadataParse, EmsgId) {
  EXPECT_EQ(4242u, TimedMetadataEmsgId("4242"));
  EXPECT_EQ(TimedMetadataEmsgId("abc"), TimedMetadataEmsgId("abc"));
  EXPECT_NE(TimedMetadataEmsgId("abc"), TimedMetadataEmsgId("abd"));
  // Not canonical decimal: hashed.
  EXPECT_NE(7u, TimedMetadataEmsgId("007"));
}

TimedMetadataEvent Ev(const std::string& id, int64_t pts) {
  TimedMetadataEvent e;
  e.kind = TimedMetadataKind::kId3;
  e.id = id;
  e.pts_90k = pts;
  e.payload = {1, 2, 3};
  return e;
}

TEST(TimedMetadataStoreTest, DedupeAndBound) {
  TimedMetadataStore store(3);
  EXPECT_TRUE(store.AddEvent(Ev("a", 1)));
  EXPECT_FALSE(store.AddEvent(Ev("a", 2)));  // same (kind, id)
  EXPECT_TRUE(store.AddEvent(Ev("b", 2)));
  EXPECT_TRUE(store.AddEvent(Ev("c", 3)));
  EXPECT_TRUE(store.AddEvent(Ev("d", 4)));  // evicts "a"
  EXPECT_EQ(3u, store.size());
  EXPECT_EQ(1u, store.evicted_count());
  uint64_t next = 0;
  auto sel = store.Select(0, 100, 90000, std::nullopt, &next);
  ASSERT_EQ(3u, sel.size());
  EXPECT_EQ("b", sel[0].id);
}

TimedMetadataEvent Sc(const std::string& id, Scte35Class cls, int64_t pts) {
  TimedMetadataEvent e;
  e.kind = TimedMetadataKind::kScte35;
  e.id = id;
  e.scte35_class = cls;
  e.pts_90k = pts;
  e.payload = {0xfc};
  return e;
}

// splice_insert OUT and IN share splice_event_id, hence the product event id.
TEST(TimedMetadataStoreTest, OutAndInWithSameIdAreBothKept) {
  TimedMetadataStore store;
  EXPECT_TRUE(store.AddEvent(Sc("1000", Scte35Class::kOut, 100)));
  EXPECT_TRUE(store.AddEvent(Sc("1000", Scte35Class::kIn, 900)));
  EXPECT_EQ(2u, store.size());
  uint64_t next = 0;
  auto sel = store.Select(0, 1000, 90000, std::nullopt, &next);
  ASSERT_EQ(2u, sel.size());
  EXPECT_EQ(Scte35Class::kOut, sel[0].scte35_class);
  EXPECT_EQ(Scte35Class::kIn, sel[1].scte35_class);
  // Distinct emsg ids; the first keeps the plain derivation.
  EXPECT_EQ(1000u, *sel[0].emsg_id);
  EXPECT_NE(*sel[0].emsg_id, *sel[1].emsg_id);
}

TEST(TimedMetadataStoreTest, ExactResendStillDropped) {
  TimedMetadataStore store;
  EXPECT_TRUE(store.AddEvent(Sc("1000", Scte35Class::kOut, 100)));
  EXPECT_FALSE(store.AddEvent(Sc("1000", Scte35Class::kOut, 100)));
  // A re-send is dropped even with a different pts or payload.
  EXPECT_FALSE(store.AddEvent(Sc("1000", Scte35Class::kOut, 500)));
  EXPECT_TRUE(store.AddEvent(Sc("1000", Scte35Class::kIn, 900)));
  EXPECT_FALSE(store.AddEvent(Sc("1000", Scte35Class::kIn, 900)));
  EXPECT_EQ(2u, store.size());
}

TEST(TimedMetadataStoreTest, CmdWithSameIdAsOutKeptSeparately) {
  TimedMetadataStore store;
  EXPECT_TRUE(store.AddEvent(Sc("5", Scte35Class::kOut, 1)));
  EXPECT_TRUE(store.AddEvent(Sc("5", Scte35Class::kCmd, 2)));
  EXPECT_TRUE(store.AddEvent(Sc("5", Scte35Class::kIn, 3)));
  EXPECT_FALSE(store.AddEvent(Sc("5", Scte35Class::kCmd, 2)));
  EXPECT_EQ(3u, store.size());
  uint64_t next = 0;
  auto sel = store.Select(0, 100, 90000, std::nullopt, &next);
  ASSERT_EQ(3u, sel.size());
  EXPECT_NE(*sel[0].emsg_id, *sel[1].emsg_id);
  EXPECT_NE(*sel[0].emsg_id, *sel[2].emsg_id);
  EXPECT_NE(*sel[1].emsg_id, *sel[2].emsg_id);
}

TEST(TimedMetadataStoreTest, Id3AndScte35SameIdIndependent) {
  TimedMetadataStore store;
  EXPECT_TRUE(store.AddEvent(Ev("9", 1)));
  EXPECT_TRUE(store.AddEvent(Sc("9", Scte35Class::kNone, 1)));
}

TEST(TimedMetadataReaderTest, OutInSameIdThroughFeed) {
  TimedMetadataStore store;
  TimedMetadataReader::LineState st;
  const std::string feed =
      "MPTM1 scte35 100 2700000 OUT 1000 /DAW\n"
      "MPTM1 scte35 900 - IN 1000 /DAW\n"
      "MPTM1 scte35 900 - IN 1000 /DAW\n"
      "MPTM1 scte35 950 - BOGUS 1000 /DAW\n";
  TimedMetadataReader::ConsumeBytes(feed.data(), feed.size(), &st, &store);
  EXPECT_EQ(2u, store.size());  // OUT, IN; re-send and bad class rejected
}

TEST(TimedMetadataStoreTest, SelectRangeAndTimescale) {
  TimedMetadataStore store;
  store.AddEvent(Ev("a", 90000));   // 1.0 s
  store.AddEvent(Ev("b", 179999));  // just before 2.0 s
  store.AddEvent(Ev("c", 180000));  // 2.0 s
  uint64_t next = 0;
  // 48 kHz timescale: [48000, 96000) is [1 s, 2 s).
  auto sel = store.Select(48000, 96000, 48000, std::nullopt, &next);
  ASSERT_EQ(2u, sel.size());
  EXPECT_EQ("a", sel[0].id);
  EXPECT_EQ("b", sel[1].id);
  sel = store.Select(96000, 144000, 48000, std::nullopt, &next);
  ASSERT_EQ(1u, sel.size());
  EXPECT_EQ("c", sel[0].id);
}

TEST(TimedMetadataCursorTest, EachEventOnceLateGoesToNextSegment) {
  auto store = std::make_shared<TimedMetadataStore>();
  TimedMetadataCursor cursor(store);
  store->AddEvent(Ev("e1", 100000));  // in segment 1 [90000, 180000)
  store->AddEvent(Ev("e3", 200000));  // in segment 2
  auto s1 = cursor.Take(90000, 90000, 90000);
  ASSERT_EQ(1u, s1.size());
  EXPECT_EQ("e1", s1[0].id);
  // e2 belongs to segment 1 but arrives after segment 1 was taken: late.
  store->AddEvent(Ev("e2", 150000));
  auto s2 = cursor.Take(180000, 90000, 90000);
  ASSERT_EQ(2u, s2.size());
  EXPECT_EQ("e2", s2[0].id);  // late, sorted first
  EXPECT_EQ("e3", s2[1].id);
  EXPECT_TRUE(cursor.Take(270000, 90000, 90000).empty());
}

TEST(TimedMetadataCursorTest, FirstSegmentDoesNotClaimPast) {
  auto store = std::make_shared<TimedMetadataStore>();
  store->AddEvent(Ev("before", 10));
  TimedMetadataCursor cursor(store);
  EXPECT_TRUE(cursor.Take(90000, 90000, 90000).empty());
}

TEST(TimedMetadataCursorTest, IndependentCursors) {
  auto store = std::make_shared<TimedMetadataStore>();
  TimedMetadataCursor a(store), b(store);
  store->AddEvent(Ev("x", 95000));
  EXPECT_EQ(1u, a.Take(90000, 90000, 90000).size());
  EXPECT_EQ(1u, b.Take(90000, 90000, 90000).size());
}

TEST(TimedMetadataCursorTest, NullStoreYieldsNothing) {
  TimedMetadataCursor c(nullptr);
  EXPECT_TRUE(c.Take(0, 90000, 90000).empty());
}

TEST(TimedMetadataAnchorTest, FirstWinsAndWait) {
  TimedMetadataStore store;
  EXPECT_FALSE(store.WaitForAnchor(10));
  EXPECT_TRUE(store.SetAnchor({90000, 1000}));
  EXPECT_FALSE(store.SetAnchor({1, 2}));
  EXPECT_TRUE(store.WaitForAnchor(10));
  EXPECT_EQ(1000, store.anchor()->unix_ms);
}

TEST(TimedMetadataReaderTest, SplitLinesOverlongAndMalformed) {
  TimedMetadataStore store;
  TimedMetadataReader::LineState st;
  const std::string good1 = "MPTM1 anchor 0 1700000000000\n";
  const std::string bad = "MPTM1 id3 1 - id !!!\n";
  const std::string huge(kTimedMetadataMaxLineBytes + 10, 'A');
  const std::string good2 = "MPTM1 id3 90000 - ok " + B64("hi") + "\n";
  const std::string all = good1 + bad + "MPTM1 id3 1 - " + huge + "\n" + good2;
  // Feed in awkward 7-byte chunks.
  for (size_t i = 0; i < all.size(); i += 7) {
    const std::string chunk = all.substr(i, 7);
    TimedMetadataReader::ConsumeBytes(chunk.data(), chunk.size(), &st, &store);
  }
  EXPECT_TRUE(store.anchor().has_value());
  EXPECT_EQ(1u, store.size());
  uint64_t n;
  auto sel = store.Select(0, 1000000, 90000, std::nullopt, &n);
  ASSERT_EQ(1u, sel.size());
  EXPECT_EQ("ok", sel[0].id);
}

TEST(TimedMetadataReaderTest, AcceptsMaxSizeLine) {
  TimedMetadataStore store;
  TimedMetadataReader::LineState st;
  // Exactly 8 KiB line (without terminator); base64 of 'A' x N, N % 4 == 0.
  const std::string prefix = "MPTM1 id3 5 - big ";
  size_t n = kTimedMetadataMaxLineBytes - prefix.size();
  n -= n % 4;
  const std::string line = prefix + std::string(n, 'A') + "\n";
  TimedMetadataReader::ConsumeBytes(line.data(), line.size(), &st, &store);
  EXPECT_EQ(1u, store.size());
}

TEST(TimedMetadataReaderTest, FileEofEndsMetadataOnly) {
  char path[] = "/tmp/tmreaderXXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  const std::string content = "MPTM1 anchor 90000 5000\nMPTM1 id3 1 - a " +
                              B64("x") + "\nMPTM1 id3 2 - b " + B64("y");
  ASSERT_EQ(static_cast<ssize_t>(content.size()),
            write(fd, content.data(), content.size()));
  close(fd);
  auto store = std::make_shared<TimedMetadataStore>();
  TimedMetadataReader reader;
  reader.Start(path, store);
  for (int i = 0; i < 100 && !store->input_ended(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  reader.Stop();
  unlink(path);
  EXPECT_TRUE(store->input_ended());
  EXPECT_TRUE(store->anchor().has_value());
  EXPECT_EQ(2u, store->size());  // last record without newline still read
}

TEST(TimedMetadataReaderTest, FifoLiveThenEof) {
  char dir[] = "/tmp/tmfifoXXXXXX";
  ASSERT_NE(nullptr, mkdtemp(dir));
  const std::string path = std::string(dir) + "/f";
  ASSERT_EQ(0, mkfifo(path.c_str(), 0600));
  auto store = std::make_shared<TimedMetadataStore>();
  TimedMetadataReader reader;
  reader.Start(path, store);  // opens before any writer exists
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_FALSE(store->input_ended());  // no writer yet is not EOF
  {
    std::ofstream out(path);
    out << "MPTM1 anchor 0 7000\n";
    out << "MPTM1 id3 1 - a " << B64("x") << "\n";
    out.flush();
    for (int i = 0; i < 100 && store->size() < 1; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(1u, store->size());
    EXPECT_FALSE(store->input_ended());
    out << "garbage line\nMPTM1 id3 2 - b " << B64("y") << "\n";
  }  // writer closes: EOF
  for (int i = 0; i < 100 && !store->input_ended(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_TRUE(store->input_ended());
  EXPECT_EQ(2u, store->size());
  reader.Stop();
  unlink(path.c_str());
  rmdir(dir);
}

TEST(TimedMetadataReaderTest, StopWithoutWriterDoesNotHang) {
  char dir[] = "/tmp/tmfifoXXXXXX";
  ASSERT_NE(nullptr, mkdtemp(dir));
  const std::string path = std::string(dir) + "/f";
  ASSERT_EQ(0, mkfifo(path.c_str(), 0600));
  auto store = std::make_shared<TimedMetadataStore>();
  TimedMetadataReader reader;
  reader.Start(path, store);
  reader.Stop();
  unlink(path.c_str());
  rmdir(dir);
}

TEST(TimedMetadataReaderTest, MissingPathContinuesWithoutMetadata) {
  auto store = std::make_shared<TimedMetadataStore>();
  TimedMetadataReader reader;
  reader.Start("/nonexistent/dir/feed", store);
  for (int i = 0; i < 100 && !store->input_ended(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_TRUE(store->input_ended());
  reader.Stop();
}

}  // namespace
}  // namespace media
}  // namespace shaka
