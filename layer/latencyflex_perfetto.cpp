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

#ifdef LATENCYFLEX_HAVE_PERFETTO
#include "latencyflex.h"

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace {
void perfetto_init() {
  perfetto::TracingInitArgs args;
  args.backends |= perfetto::kSystemBackend;
  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();
}

class OnLoad {
public:
  OnLoad() { perfetto_init(); }
};

[[maybe_unused]] OnLoad on_load;
} // namespace
#endif