// Copyright 2021 Tatsuyuki Ishi
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

#ifndef LATENCYFLEX_LATENCYFLEX_LAYER_H
#define LATENCYFLEX_LATENCYFLEX_LAYER_H

#include <atomic>
#include <chrono>
#include <cstdint>

#include <vulkan/vk_layer.h>

// These are private APIs. There is no backwards compatibility guarantee.

extern "C" VK_LAYER_EXPORT void lfx_WaitAndBeginFrame();
extern "C" VK_LAYER_EXPORT void lfx_SetTargetFrameTime(uint64_t target_frame_time);

inline uint64_t current_time_ns() {
  struct timespec tv;
  // CLOCK_BOOTTIME used for compatibility with Perfetto timestamps
  clock_gettime(CLOCK_BOOTTIME, &tv);
  return tv.tv_nsec + tv.tv_sec * UINT64_C(1000000000);
}

#endif // LATENCYFLEX_LATENCYFLEX_LAYER_H
