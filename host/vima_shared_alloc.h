// VIMA fork — shareable host allocation for the ASG ring buffer.
//
// gfxstream allocates the Address-Space-Graphics ring with posix_memalign
// (AlignedBuf). On QEMU the VMM maps any host pointer into guest space
// in-process, so heap is fine. Apple's Virtualization.framework hands the
// region to a separate helper process via xpc_shmem_create(), which requires a
// Mach memory entry over the range — heap memory triggers _xpc_api_misuse and a
// SIGTRAP. This returns MAP_SHARED shm-backed memory instead, so
// VZVirtioSharedMemoryRegion.mapMemory() accepts gfxstream's own pointer with no
// USE_EXTERNAL_BLOB (which would break the Metal external-memory / IOSurface
// ColorBuffer path). Non-Darwin falls through to AlignedBuf unchanged.
#pragma once
#include <cstddef>

namespace gfxstream {

// align must be <= page size (the ASG ring passes getpagesize()). NULL on failure.
void* vima_shared_alloc(size_t align, size_t size);
void  vima_shared_free(void* addr, size_t size);

}  // namespace gfxstream
