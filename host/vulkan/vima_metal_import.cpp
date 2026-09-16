// VIMA fork hook (R2.3) -- see vima_metal_import.h.
#include "vima_metal_import.h"

namespace {
GfxstreamVimaMetalTextureProviderFn sProvider = nullptr;

// R3.2/R3.3 (Option B) -- whether the VIMA texture provider is installed at all
// is decided by the embedder, not here. The VM/Android path does NOT install it:
// pushing VIMA's texture into infoPtr->image while infoPtr->memory stays
// allocated and exported SPLITS the ColorBuffer's storage, the guest renders into
// the memory, and we present the image -- which is why the home screen was black
// for five attempts. The R2.3.x ColorBuffer selftests DO install it, because
// proving "this ColorBuffer's VkImage is a VIMA IOSurface" is exactly their point.
}  // namespace

extern "C" void gfxstream_vima_set_metal_texture_provider(
    GfxstreamVimaMetalTextureProviderFn fn) {
    sProvider = fn;
}

extern "C" GfxstreamVimaMetalTextureProviderFn gfxstream_vima_get_metal_texture_provider(void) {
    return sProvider;
}

namespace {
GfxstreamVimaColorBufferTextureFn sTextureSink = nullptr;
}

extern "C" void gfxstream_vima_set_colorbuffer_texture_sink(GfxstreamVimaColorBufferTextureFn fn) {
    sTextureSink = fn;
}

extern "C" GfxstreamVimaColorBufferTextureFn gfxstream_vima_get_colorbuffer_texture_sink(void) {
    return sTextureSink;
}
