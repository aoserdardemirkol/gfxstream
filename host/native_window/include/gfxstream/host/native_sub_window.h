/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef NATIVE_SUB_WINDOW_H
#define NATIVE_SUB_WINDOW_H

#include <cstdint>

#include "render-utils/render_api_platform_types.h"

#if GFXSTREAM_ENABLE_HOST_GLES
#include <EGL/egl.h>
#else
typedef unsigned int GLenum;
typedef int32_t EGLint;
// Must stay ABI-compatible with the real EGLNativeWindowType from
// <EGL/eglplatform.h>, which is pointer-sized on 64-bit platforms
// (e.g. `void*` on Apple); a 32-bit integer here truncates pointers.
typedef void* EGLNativeWindowType;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*SubWindowRepaintCallback)(void*);

// Create a new sub-window that will be used to display the content of the
// emulated GPU on top of the regular UI window.
// |p_window| is the platform-specific handle to the main UI window.
// |x|, |y| is the position sub-window relative to the top-left corner of the
// main window.
// |width| and |height| are the dimensions of the sub-window, as well as of
// the emulated framebuffer.
// |repaint_callback| may be invoked every time the window has to be repainted
// (such as receiving a WM_PAINT event on Windows). If the provided argument is
//  NULL, nothing will be invoked.
// |repaint_callback_param| an additional parameter that will be passed to the
// repaint callback when/if it's invoked.
// On success, return a new platform-specific window handle, cast as an
// EGLNativeWindowType. Or 0/NULL in case of failure.
EGLNativeWindowType createSubWindow(FBNativeWindowType p_window, int x, int y, int width,
                                    int height, float dpr,
                                    SubWindowRepaintCallback repaint_callback,
                                    void* repaint_callback_param, int hideWindow);

// Destroy a sub-window previously created through createSubWindow() above.
void destroySubWindow(EGLNativeWindowType win);

// Moves a sub-window previously created through createSubWindow() above.
// |p_parent_window| is the platform-specific handle to the main UI window.
// |p_sub_window| is the platform-specific handle to the EGL subwindow.
// |x|,|y|,|width|,|height| are the new location and dimensions of the
// subwindow.
int moveSubWindow(FBNativeWindowType p_parent_window, EGLNativeWindowType p_sub_window, int x,
                  int y, int width, int height, float dpr);

void* getNativeDisplay();

#if defined(VK_USE_PLATFORM_METAL_EXT)
// Retrieve metal layer from the view, to create a swapchain surface
extern void* getMetalLayerFromView(void* view);

// Resize the drawables the view's metal layer hands out.
//
// The swapchain images are copied into a drawable at present time, so the drawable size and
// the swapchain extent have to agree. Only the thread that creates the swapchain may call
// this, and it must call it before creating one; an embedder that sets the size itself races
// the posting thread and eventually presents into a drawable of the wrong shape.
extern void setMetalLayerDrawableSize(void* view, unsigned width, unsigned height);
#endif

#ifdef __cplusplus
}
#endif

#endif
