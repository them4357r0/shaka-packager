// Copyright 2026 media-platform contributors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_FORMATS_MP4_TIMED_METADATA_EMSG_H_
#define PACKAGER_MEDIA_FORMATS_MP4_TIMED_METADATA_EMSG_H_

#include <string>
#include <vector>

#include <packager/media/base/buffer_writer.h>
#include <packager/media/base/timed_metadata.h>
#include <packager/media/formats/mp4/box_definitions.h>

namespace shaka {
namespace media {
namespace mp4 {

constexpr char kEmsgSchemeId3[] = "https://aomedia.org/emsg/ID3";
constexpr char kEmsgSchemeScte35[] = "urn:scte:scte35:2013:bin";

/// Builds the emsg v1 box for one event: timescale 90000, absolute
/// presentation_time, event_duration (0xFFFFFFFF when unknown), message_data
/// is the raw ID3 tag or SCTE-35 splice_info_section.
EventMessage MakeEventMessage(const TimedMetadataEvent& event);

/// Appends one emsg box per event to |writer|; returns the bytes appended.
size_t WriteEventMessages(const std::vector<TimedMetadataEvent>& events,
                          BufferWriter* writer);

}  // namespace mp4
}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_FORMATS_MP4_TIMED_METADATA_EMSG_H_
