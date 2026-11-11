// Copyright 2026 media-platform contributors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp4/timed_metadata_emsg.h>

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <packager/media/formats/mp4/box_reader.h>

namespace shaka {
namespace media {
namespace mp4 {
namespace {

TimedMetadataEvent Ev(TimedMetadataKind kind,
                      int64_t pts,
                      int64_t dur,
                      const std::string& id,
                      std::vector<uint8_t> payload) {
  TimedMetadataEvent e;
  e.kind = kind;
  e.pts_90k = pts;
  e.duration_90k = dur;
  e.id = id;
  e.payload = std::move(payload);
  return e;
}

TEST(EmsgTest, ExactBytesId3) {
  BufferWriter w;
  const size_t n = WriteEventMessages(
      {Ev(TimedMetadataKind::kId3, 0x0102030405, 90000, "7", {0xAA, 0xBB})},
      &w);
  const std::string scheme = kEmsgSchemeId3;
  std::vector<uint8_t> expected = {
      0, 0, 0, 0,  // size, patched below
      'e', 'm', 's', 'g',
      1, 0, 0, 0,              // version 1, flags 0
      0, 1, 0x5f, 0x90,        // timescale 90000
      0, 0, 0, 1, 2, 3, 4, 5,  // presentation_time
      0, 1, 0x5f, 0x90,        // event_duration 90000
      0, 0, 0, 7,              // id
  };
  expected.insert(expected.end(), scheme.begin(), scheme.end());
  expected.push_back(0);  // scheme terminator
  expected.push_back(0);  // empty value terminator
  expected.push_back(0xAA);
  expected.push_back(0xBB);
  expected[3] = static_cast<uint8_t>(expected.size());
  ASSERT_EQ(expected.size(), n);
  EXPECT_EQ(expected, std::vector<uint8_t>(w.Buffer(), w.Buffer() + w.Size()));
}

TEST(EmsgTest, Scte35SchemeUnknownDurationAndReadback) {
  BufferWriter w;
  WriteEventMessages({Ev(TimedMetadataKind::kScte35, 252000, -1, "evt-9",
                         {0xfc, 0x30, 0x11})},
                     &w);
  bool err = false;
  std::unique_ptr<BoxReader> reader(
      BoxReader::ReadBox(w.Buffer(), w.Size(), &err));
  ASSERT_TRUE(reader);
  ASSERT_FALSE(err);
  EXPECT_EQ(FOURCC_emsg, reader->type());
  EventMessage emsg;
  ASSERT_TRUE(emsg.Parse(reader.get()));
  EXPECT_EQ(1, emsg.version);
  EXPECT_EQ(90000u, emsg.timescale);
  EXPECT_EQ(252000u, emsg.presentation_time);
  EXPECT_EQ(0xFFFFFFFFu, emsg.event_duration);
  EXPECT_EQ(std::string(kEmsgSchemeScte35), emsg.scheme_id_uri);
  EXPECT_EQ("", emsg.value);
  EXPECT_EQ(TimedMetadataEmsgId("evt-9"), emsg.id);
  EXPECT_EQ((std::vector<uint8_t>{0xfc, 0x30, 0x11}), emsg.message_data);
}

TEST(EmsgTest, MultipleEventsAppendInOrder) {
  BufferWriter w;
  const size_t n = WriteEventMessages(
      {Ev(TimedMetadataKind::kId3, 1, 5, "a", {1}),
       Ev(TimedMetadataKind::kScte35, 2, 5, "b", {2})},
      &w);
  EXPECT_EQ(w.Size(), n);
  EXPECT_EQ(0u, WriteEventMessages({}, &w));
}

}  // namespace
}  // namespace mp4
}  // namespace media
}  // namespace shaka
