/*  Copyright (C) 2014-2017 FastoGT. All right reserved.

    This file is part of FastoTV.

    FastoTV is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    FastoTV is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with FastoTV. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <stdint.h>  // for int64_t

extern "C" {
#include <libavutil/frame.h>
}

#include <common/macros.h>

#include "client/core/types.h"

namespace fasto {
namespace fastotv {
namespace client {
namespace core {

struct AudioFrame {
  AudioFrame();
  ~AudioFrame();

  AVFrame* frame;
  serial_id_t serial;
  clock_t pts;      /* presentation timestamp for the frame */
  clock_t duration; /* estimated duration of the frame */
  int64_t pos;      /* byte position of the frame in the input file */

  void ClearFrame();

 private:
  DISALLOW_COPY_AND_ASSIGN(AudioFrame);
};

}  // namespace core
}
}
}