// VIMA fork — see vima_shared_alloc.h.
#include "vima_shared_alloc.h"

#include "gfxstream/AlignedBuf.h"

#if defined(__APPLE__)
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gfxstream {

#if defined(__APPLE__)

namespace {
size_t roundUpToPage(size_t n) {
    const size_t p = static_cast<size_t>(::getpagesize());
    return (n + p - 1) & ~(p - 1);
}
}  // namespace

void* vima_shared_alloc(size_t align, size_t size) {
    const size_t page = static_cast<size_t>(::getpagesize());
    // mmap always returns page-aligned; the ASG ring only ever asks for <= page.
    assert(align <= page && "vima_shared_alloc: alignment larger than a page");
    if (align > page || size == 0) return nullptr;

    const size_t len = roundUpToPage(size);

    // macOS PSHMNAMLEN is 31 including the leading slash; keep it short & unique.
    static std::atomic<unsigned> counter{0};
    char name[32];
    std::snprintf(name, sizeof(name), "/vima-rb-%d-%u", ::getpid(),
                  counter.fetch_add(1, std::memory_order_relaxed));

    int fd = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return nullptr;
    ::shm_unlink(name);  // fd keeps the object alive; nothing else can open it

    if (::ftruncate(fd, static_cast<off_t>(len)) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* addr = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);  // mapping keeps the object alive after this
    if (addr == MAP_FAILED) return nullptr;
    std::memset(addr, 0, len);
    return addr;
}

void vima_shared_free(void* addr, size_t size) {
    if (addr == nullptr) return;
    ::munmap(addr, roundUpToPage(size));
}

#else  // !__APPLE__ — upstream behaviour unchanged

void* vima_shared_alloc(size_t align, size_t size) {
    return gfxstream::aligned_buf_alloc(align, size);
}
void vima_shared_free(void* addr, size_t /*size*/) {
    gfxstream::aligned_buf_free(addr);
}

#endif

}  // namespace gfxstream
