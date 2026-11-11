// Copyright 2026 media-platform contributors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/app/timed_metadata_flags.h>

#include <string>

#include <absl/flags/flag.h>

ABSL_FLAG(std::string,
          timed_metadata_input,
          "",
          "Path of a FIFO or file with out-of-band timed metadata, one "
          "record per line (at most 8 KiB): 'MPTM1 anchor <pts90k> "
          "<unix_ms>', 'MPTM1 id3 <pts90k> <dur|-> <id> <base64>', 'MPTM1 "
          "scte35 <pts90k> <dur|-> <OUT|IN|CMD|NONE> <id> <base64>'. Times "
          "use the 90 kHz media timeline. Events are written as 'emsg' boxes "
          "(fMP4 outputs) and EXT-X-DATERANGE tags (HLS); the anchor sets the "
          "EXT-X-PROGRAM-DATE-TIME reference. End of input ends the metadata "
          "only, not packaging. Disabled when empty.");
