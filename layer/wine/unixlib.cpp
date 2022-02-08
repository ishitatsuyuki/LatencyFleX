// Copyright 2022 Tatsuyuki Ishi
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "latencyflex_layer.h"
#define NTSTATUS long

extern "C" {

// Internal definitions copied out of the wine source tree.
// These APIs are likely unstable: copy these at your own risk. They will require changes when
// upstream modifies the mechanism.
typedef NTSTATUS (*unixlib_entry_t)(void *args);

static NTSTATUS winelfx_WaitAndBeginFrame(void *) {
  lfx_WaitAndBeginFrame();
  return 0;
}

static NTSTATUS winelfx_SetTargetFrameTime(void *target_frame_time) {
  lfx_SetTargetFrameTime(*(int64_t *)target_frame_time);
  return 0;
}

// extern declaration is required, or g++ would happily mangle the symbol name
extern const unixlib_entry_t __wine_unix_call_funcs[];
// Keep this in sync with builtin.cpp.
const unixlib_entry_t __wine_unix_call_funcs[] = {
    winelfx_WaitAndBeginFrame,
    winelfx_SetTargetFrameTime,
};

} // extern "C"