// Copyright 2026 media-platform contributors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp4/timed_metadata_emsg.h>

namespace shaka {
namespace media {
namespace mp4 {

EventMessage MakeEventMessage(const TimedMetadataEvent& event) {
  EventMessage emsg;
  emsg.timescale = kTimedMetadataTimeScale;
  emsg.presentation_time = static_cast<uint64_t>(event.pts_90k);
  emsg.event_duration =
      (event.duration_90k < 0 || event.duration_90k >= 0xFFFFFFFFll)
          ? EventMessage::kUnknownDuration
          : static_cast<uint32_t>(event.duration_90k);
  emsg.id = TimedMetadataEmsgId(event.id);
  emsg.scheme_id_uri = event.kind == TimedMetadataKind::kId3
                           ? kEmsgSchemeId3
                           : kEmsgSchemeScte35;
  emsg.message_data = event.payload;
  return emsg;
}

size_t WriteEventMessages(const std::vector<TimedMetadataEvent>& events,
                          BufferWriter* writer) {
  const size_t before = writer->Size();
  for (const auto& event : events) {
    EventMessage emsg = MakeEventMessage(event);
    emsg.Write(writer);
  }
  return writer->Size() - before;
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka
