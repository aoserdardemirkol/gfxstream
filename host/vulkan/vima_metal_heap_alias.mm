// VIMA fork hook -- see vima_metal_heap_alias.h.
#include "vima_metal_heap_alias.h"

#import <Metal/Metal.h>

namespace gfxstream {
namespace host {
namespace vk {

namespace {

MTLPixelFormat nativePixelFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
        case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
        case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return MTLPixelFormatRGB10A2Unorm;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
        default: return MTLPixelFormatInvalid;
    }
}

// kk_image_layout.c: vk_image_usage_flags_to_mtl_texture_usage (non-atomic formats).
MTLTextureUsage textureUsage(VkImageUsageFlags usage, VkImageCreateFlags flags) {
    MTLTextureUsage out = MTLTextureUsageUnknown;
    if (usage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
        out |= MTLTextureUsageShaderWrite;
    }
    if (usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
        out |= MTLTextureUsageShaderRead;
    }
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        out |= MTLTextureUsageRenderTarget;
    }
    if (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
        out |= MTLTextureUsagePixelFormatView;
    }
    return out;
}

// kk_image_layout.c: kk_image_layout_can_optimize.
bool canOptimize(const VkImageCreateInfo& ci) {
    if (ci.tiling != VK_IMAGE_TILING_OPTIMAL) return false;
    if (ci.usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT) return false;
    if (ci.usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) return false;
    if (ci.flags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) return false;
    return true;
}

}  // namespace

void* vimaNewHeapAliasTexture(void* mtlHeap, const VkImageCreateInfo& ci, uint64_t memoryOffset) {
    if (!mtlHeap || ci.tiling != VK_IMAGE_TILING_OPTIMAL || ci.imageType != VK_IMAGE_TYPE_2D ||
        ci.arrayLayers != 1 || ci.samples != VK_SAMPLE_COUNT_1_BIT) {
        return nullptr;
    }
    // kk_image_layout.c: input attachments are always 2D array textures.
    const bool asArray = ci.usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    MTLPixelFormat pixelFormat = nativePixelFormat(ci.format);
    if (pixelFormat == MTLPixelFormatInvalid) return nullptr;

    @autoreleasepool {
        id<MTLHeap> heap = (__bridge id<MTLHeap>)mtlHeap;
        if (heap.type != MTLHeapTypePlacement) return nullptr;

        MTLTextureDescriptor* d = [[MTLTextureDescriptor new] autorelease];
        d.textureType = asArray ? MTLTextureType2DArray : MTLTextureType2D;
        d.pixelFormat = pixelFormat;
        d.width = ci.extent.width;
        d.height = ci.extent.height;
        d.depth = 1;
        d.mipmapLevelCount = ci.mipLevels;
        d.sampleCount = 1;
        d.arrayLength = 1;
        d.allowGPUOptimizedContents = canOptimize(ci);
        d.usage = textureUsage(ci.usage, ci.flags);
        d.resourceOptions = heap.resourceOptions;

        // kk_image_plane_bind aligns the bind offset to the plane's heap alignment.
        MTLSizeAndAlign sa = [heap.device heapTextureSizeAndAlignWithDescriptor:d];
        uint64_t offset = sa.align ? (memoryOffset + sa.align - 1) / sa.align * sa.align
                                   : memoryOffset;
        if (offset + sa.size > heap.size) return nullptr;

        id<MTLTexture> tex = [heap newTextureWithDescriptor:d offset:offset];
        if (!tex || !asArray) return (__bridge void*)tex;
        // VIMA presents 2D textures; a view keeps the array texture alive.
        id<MTLTexture> view = [tex newTextureViewWithPixelFormat:pixelFormat
                                                     textureType:MTLTextureType2D
                                                          levels:NSMakeRange(0, ci.mipLevels)
                                                          slices:NSMakeRange(0, 1)];
        [tex release];
        return (__bridge void*)view;
    }
}

void vimaReleaseHeapAliasTexture(void* mtlTexture) {
    if (mtlTexture) [(__bridge id<MTLTexture>)mtlTexture release];
}

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
