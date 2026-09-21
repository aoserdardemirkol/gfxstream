// VIMA fork hook (R2.3): let the host embedder back a ColorBuffer's VkImage with
// its own MTLTexture (an IOSurface it will later composite with Metal), instead of
// gfxstream allocating fresh device memory. Proven on MoltenVK 1.4.2 via
// VkImportMetalTextureInfoEXT chained into VkImageCreateInfo.pNext -- see
// VIMA tools/image-builder/r-s/R-S.md sec V3.
//
// This header carries no Vulkan/Metal types so it is safe to include from C.
#pragma once

#include <stdint.h>

#ifndef GFXSTREAM_VIMA_EXPORT
#define GFXSTREAM_VIMA_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Return an `id<MTLTexture>` (cast to void*) to back the ColorBuffer's VkImage,
// or NULL to fall back to gfxstream's normal allocation. The texture and the
// IOSurface behind it are owned by the embedder for the ColorBuffer's lifetime.
// vkFormat is a VkFormat value; width/height are the ColorBuffer dimensions.
typedef void* (*GfxstreamVimaMetalTextureProviderFn)(uint32_t colorBufferHandle,
                                                     uint32_t width, uint32_t height,
                                                     int vkFormat);

GFXSTREAM_VIMA_EXPORT void gfxstream_vima_set_metal_texture_provider(
    GfxstreamVimaMetalTextureProviderFn fn);
GFXSTREAM_VIMA_EXPORT GfxstreamVimaMetalTextureProviderFn
gfxstream_vima_get_metal_texture_provider(void);

// ---------------------------------------------------------------------------
// R3.2 (Option B) -- the OPPOSITE direction, and the one that actually works.
//
// The provider above pushes VIMA's texture INTO gfxstream's ColorBuffer image.
// That splits the ColorBuffer's storage: the image becomes VIMA's texture while
// the memory stays gfxstream's, and the guest renders into the memory. We then
// present the image and see nothing. (Measured: with the provider disabled via
// VIMA_CB_IMPORT=0, the guest's screencap immediately shows live, changing
// content -- so the pixels were always landing in the memory-backed image.)
//
// This sink inverts it: gfxstream keeps sole ownership of the ColorBuffer, binds
// its image to its own exported memory exactly as upstream does on every
// platform, and then hands VIMA the resulting `id<MTLTexture>` to composite.
// One storage, no copy, and the object VIMA presents IS the object the guest
// rendered into.
//
// Called once per ColorBuffer, right after its image is bound. `mtlTexture` is
// owned by gfxstream and lives as long as the ColorBuffer; the embedder must
// retain it if it keeps a reference.
typedef void (*GfxstreamVimaColorBufferTextureFn)(uint32_t colorBufferHandle, void* mtlTexture,
                                                  uint32_t width, uint32_t height, int vkFormat);

GFXSTREAM_VIMA_EXPORT void gfxstream_vima_set_colorbuffer_texture_sink(
    GfxstreamVimaColorBufferTextureFn fn);
GFXSTREAM_VIMA_EXPORT GfxstreamVimaColorBufferTextureFn
gfxstream_vima_get_colorbuffer_texture_sink(void);


/** Native Vulkan presentation. Attach is called on the UI thread before guest start.
 * Updates enqueue work; post completion runs off the UI thread after GPU consumption.
 * The embedder must keep the view alive for the renderer lifetime.
 */
GFXSTREAM_VIMA_EXPORT int gfxstream_vima_attach_surface(void* view, int width, int height);
GFXSTREAM_VIMA_EXPORT void gfxstream_vima_update_surface(int width, int height, int rotation, int visible);
/** |completed| receives |context| and 1 when the frame reached the display worker, 0 when the
 * post failed. A non-zero return means the callback will never run and |context| is still the
 * caller's to release. */
GFXSTREAM_VIMA_EXPORT int gfxstream_vima_post(uint32_t resource, void (*completed)(void*, int),
                                              void* context);

#ifdef __cplusplus
}  // extern "C"
#endif
