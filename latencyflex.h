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

#ifndef LATENCYFLEX_H
#define LATENCYFLEX_H

#ifdef LATENCYFLEX_HAVE_PERFETTO
#include <perfetto.h>
PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("latencyflex")
        .SetDescription("LatencyFleX latency and throughput metrics"));
#else
#define TRACE_COUNTER(...)
#define TRACE_EVENT_BEGIN(...)
#define TRACE_EVENT_END(...)
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace lfx {
namespace internal {
// An exponentially weighted moving average estimator.
class EwmaEstimator {
public:
  // `alpha`: Smoothing factor. Larger values means less smoothing, resulting in
  //          a bumpy but quick response.
  // `full_weight`: Set to true to disable weight correction for initial
  //                samples. The estimator will start with a value of 0 weighted
  //                at 100% instead.
  EwmaEstimator(double alpha, bool full_weight = false)
      : alpha_(alpha), current_weight_(full_weight ? 1.0 : 0.0) {}

  // Update the estimate with `value`. `value` must not be negative. If a
  // negative exponent is used, then `value` must not be too small or the
  // internal accumulator will overflow.
  void update(double value) {
    current_ = (1 - alpha_) * current_ + alpha_ * value;
    current_weight_ = (1 - alpha_) * current_weight_ + alpha_;
  }

  double get() const {
    if (current_weight_ == 0) {
      return 0;
    }
    return current_ / current_weight_;
  }

private:
  double alpha_;
  double current_ = 0;
  double current_weight_;
};
} // namespace internal

// Tracks and computes frame time, latency and the desired sleep time before
// next tick. All time is in nanoseconds. The clock domain doesn't matter as
// long as it's a single consistent clock.
//
// Access must be externally synchronized.
class LatencyFleX {
public:
  LatencyFleX()
      : latency_(0.3), inv_throughtput_(0.3), proj_correction_(0.5, true) {
    std::fill(std::begin(frame_begin_ids_), std::end(frame_begin_ids_),
              UINT64_MAX);
  }

  // Get the desired wake-up time. Sleep until this time, then call
  // BeginFrame(). target_frame_time can be specified to limit the FPS, or left
  // at 0 for unlimited framerate.
  //
  // If a wait target cannot be determined due to lack of data, then `0` is
  // returned.
  uint64_t GetWaitTarget(uint64_t frame_id) {
    if (prev_frame_end_id_ != UINT64_MAX) {
      size_t phase = frame_id % cycle_.size();
      double gain = cycle_[phase];
      double invtpt = inv_throughtput_.get();
      if (frame_end_projection_base_ == UINT64_MAX) {
        frame_end_projection_base_ = prev_frame_end_ts_;
      } else {
        int64_t correction =
            (int64_t)prev_frame_end_ts_ -
            (int64_t)(frame_end_projection_base_ +
                      frame_end_projected_ts_[prev_frame_end_id_ %
                                              kMaxInflightFrames]);
        proj_correction_.update(correction - prev_correction_);
        // Once we have accounted for the delay, don't accumulate it for future
        // frames.
        prev_correction_ = correction;
      }

      TRACE_COUNTER("latencyflex", "Delay Compensation",
                    proj_correction_.get());

      uint64_t target = (int64_t)frame_end_projection_base_ +
                        (int64_t)frame_end_projected_ts_[prev_frame_begin_id_ %
                                                         kMaxInflightFrames] +
                        (int64_t)fmax(proj_correction_.get(), 0.0) +
                        (int64_t)std::round((((int64_t)frame_id -
                                              (int64_t)prev_frame_begin_id_) +
                                             1 / gain - 1) *
                                                invtpt -
                                            latency_.get());
      // Contrary to the target time, the projection time uses min(1, gain) for
      // its calculation. This is because in the steady state, a positive gain
      // would increase the queue but not increase the throughput, while a
      // negative gain would decrease the throughput. Since one affects the
      // frame time while the other doesn't, we need to do an asymmetrical
      // handling here.
      // The delay compensation is also not added: we are trying to compensate
      // for the case where the queue is increasing, so there should not be a
      // change in throughput if queuing was successfully reduced.
      uint64_t new_projection =
          (int64_t)frame_end_projected_ts_[prev_frame_begin_id_ %
                                           kMaxInflightFrames] +
          (int64_t)fmax(proj_correction_.get(), 0.0) +
          (int64_t)std::round(
              (((int64_t)frame_id - (int64_t)prev_frame_begin_id_) +
               1 / std::fmin(gain, 1) - 1) *
              invtpt);
      frame_end_projected_ts_[frame_id % kMaxInflightFrames] = new_projection;
      TRACE_EVENT_BEGIN("latencyflex", "projection",
                        perfetto::Track(track_base_ +
                                        frame_id % kMaxInflightFrames +
                                        kMaxInflightFrames),
                        target);
      TRACE_EVENT_END("latencyflex",
                      perfetto::Track(track_base_ +
                                      frame_id % kMaxInflightFrames +
                                      kMaxInflightFrames),
                      frame_end_projection_base_ + new_projection);
      return target;
    } else {
      return 0;
    }
  }

  // Begin the frame. Called on the main/simulation thread.
  //
  // It's recommended that the timestamp is calculated as follows:
  // - If a sleep is not performed (because the wait target has already been
  //   passed), then pass the current time.
  // - If a sleep is performed (wait target was not in the past), then pass the
  //   wait target as-is. This allows compensating for any latency incurred by
  //   the OS for waking up the process.
  void BeginFrame(uint64_t frame_id, uint64_t timestamp) {
    TRACE_EVENT_BEGIN(
        "latencyflex", "frame",
        perfetto::Track(track_base_ + frame_id % kMaxInflightFrames),
        timestamp);
    frame_begin_ids_[frame_id % kMaxInflightFrames] = frame_id;
    frame_begin_ts_[frame_id % kMaxInflightFrames] = timestamp;
    prev_frame_begin_id_ = frame_id;
  }

  // End the frame. Called from a rendering-related thread.
  //
  // The timestamp should be obtained in one of the following ways:
  // - Run a thread dedicated to wait for command buffer completion fences.
  //   Capture the timestamp on CPU when the fence is signaled.
  // - Capture a GPU timestamp when frame ends, then convert it into a clock
  //   domain on CPU (known as "timestamp calibration").
  //
  // If `latency` and `frame_time` are not null, then the latency and the frame
  // time are returned respectively, or UINT64_MAX is returned if measurement is
  // unavailable.
  void EndFrame(uint64_t frame_id, uint64_t timestamp, uint64_t *latency,
                uint64_t *frame_time) {
    size_t phase = frame_id % cycle_.size();
    int64_t latency_val = -1;
    int64_t frame_time_val = -1;
    if (frame_begin_ids_[frame_id % kMaxInflightFrames] == frame_id) {
      auto frame_start = frame_begin_ts_[frame_id % kMaxInflightFrames];
      frame_begin_ids_[frame_id % kMaxInflightFrames] = UINT64_MAX;
      latency_val = (int64_t)timestamp - (int64_t)frame_start;
      latency_val =
          std::clamp(latency_val, INT64_C(1000000), INT64_C(50000000));
      if (phase == 1 && latency_val > 0) {
        latency_.update(latency_val);
      }
      TRACE_COUNTER("latencyflex", "Latency", latency_val);
      TRACE_COUNTER("latencyflex", "Latency (Estimate)", latency_.get());
      if (prev_frame_end_id_ != UINT64_MAX) {
        if (frame_id > prev_frame_end_id_) {
          auto frames_elapsed = frame_id - prev_frame_end_id_;
          frame_time_val = ((int64_t)timestamp - (int64_t)prev_frame_end_ts_) /
                           (int64_t)frames_elapsed;
          frame_time_val =
              std::clamp(frame_time_val, INT64_C(1000000), INT64_C(50000000));
          if (phase == 0 && frame_time_val > 0) {
            inv_throughtput_.update(frame_time_val);
          }
          TRACE_COUNTER("latencyflex", "Frame Time", frame_time_val);
          TRACE_COUNTER("latencyflex", "Frame Time (Estimate)",
                        inv_throughtput_.get());
        }
      }
      prev_frame_end_id_ = frame_id;
      prev_frame_end_ts_ = timestamp;
    }
    if (latency)
      *latency = latency_val;
    if (frame_time)
      *frame_time = frame_time_val;
    TRACE_EVENT_END(
        "latencyflex",
        perfetto::Track(track_base_ + frame_id % kMaxInflightFrames),
        timestamp);
  }

  void Reset() {
    auto new_instance = LatencyFleX();
#ifdef LATENCYFLEX_HAVE_PERFETTO
    new_instance.track_base_ = track_base_ + 2 * kMaxInflightFrames;
#endif
    new_instance.target_frame_time = target_frame_time;
    *this = new_instance;
  }

  // Currently unimplemented
  uint64_t target_frame_time = 0;

private:
  static const std::size_t kMaxInflightFrames = 16;

  uint64_t frame_begin_ts_[kMaxInflightFrames] = {};
  uint64_t frame_begin_ids_[kMaxInflightFrames];
  uint64_t frame_end_projected_ts_[kMaxInflightFrames] = {};
  uint64_t frame_end_projection_base_ = UINT64_MAX;
  uint64_t prev_frame_begin_id_ = UINT64_MAX;
  std::vector<double> cycle_ = {1.08, 0.96};
  int64_t prev_correction_ = 0;
  uint64_t prev_frame_end_id_ = UINT64_MAX;
  uint64_t prev_frame_end_ts_;
  internal::EwmaEstimator latency_;
  internal::EwmaEstimator inv_throughtput_;
  internal::EwmaEstimator proj_correction_;

#ifdef LATENCYFLEX_HAVE_PERFETTO
  uint64_t track_base_ = 0;
#endif
};
} // namespace lfx

#endif // LATENCYFLEX_H
