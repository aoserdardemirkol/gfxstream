// Copyright (C) 2022 The Android Open Source Project
// Copyright (C) 2022 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "gfxstream/host/GfxApiLogger.h"

namespace gfxstream {
namespace host {
namespace vk {

struct VkDecoderContext {
    const char* processName = nullptr;
    host::GfxApiLogger* gfxApiLogger = nullptr;
    std::atomic_bool* shouldExit = nullptr;
    // VIMA fork (0013): the guest process this stream belongs to. Only used for
    // diagnostics -- a seqno stall is per-process, and processName alone is not
    // enough to tell two contexts of the same app apart.
    uint64_t puid = 0;
};

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
