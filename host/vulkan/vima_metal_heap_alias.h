// VIMA fork hook -- a ColorBuffer MTLTexture for drivers without VK_EXT_metal_objects.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace gfxstream {
namespace host {
namespace vk {

// KosmicKrisp has no vkExportMetalObjectsEXT, but it binds every OPTIMAL-tiled image as
// [MTLHeap newTextureWithDescriptor:offset:] on the placement heap it exports through
// VK_EXT_external_memory_metal. A second texture built from the same descriptor at the same
// heap offset aliases the image's storage, so VIMA can present it without a copy.
//
// The descriptor mirrors KosmicKrisp's kk_image_layout rules (mesa 26.2). Only formats whose
// Metal pixel format is native and non-atomic there are handled; anything else returns
// nullptr. Returns a +1 retained id<MTLTexture>.
void* vimaNewHeapAliasTexture(void* mtlHeap, const VkImageCreateInfo& imageCi,
                              uint64_t memoryOffset);

// Releases a texture returned by vimaNewHeapAliasTexture.
void vimaReleaseHeapAliasTexture(void* mtlTexture);

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
