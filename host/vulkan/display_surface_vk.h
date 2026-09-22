// Copyright (C) 2022 The Android Open Source Project
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

#include <cstdint>
#include <memory>

#include "gfxstream/host/display_surface.h"
#include "render-utils/render_api_platform_types.h"
#include "goldfish_vk_dispatch.h"

namespace gfxstream {
namespace host {
namespace vk {

class DisplaySurfaceVk : public DisplaySurfaceImpl {
  public:
   static std::unique_ptr<DisplaySurfaceVk> create(const VulkanDispatch& vk, VkInstance vkInstance,
                                                   FBNativeWindowType window);

   ~DisplaySurfaceVk();

   VkSurfaceKHR getSurface() const { return mSurface; }

   // Resizes the drawables the window hands out, on platforms where a drawable is a distinct
   // thing from a swapchain image. Call it from the thread that creates the swapchain, right
   // before creating one, so the two can never disagree about the shape. A no-op elsewhere.
   void setDrawableSize(uint32_t width, uint32_t height) const;

  private:
   DisplaySurfaceVk(const VulkanDispatch& vk, VkInstance vkInstance, VkSurfaceKHR vkSurface,
                    FBNativeWindowType window);

   const VulkanDispatch& mVk;
   VkInstance mInstance = VK_NULL_HANDLE;
   VkSurfaceKHR mSurface = VK_NULL_HANDLE;
   FBNativeWindowType mWindow = {};
};

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
