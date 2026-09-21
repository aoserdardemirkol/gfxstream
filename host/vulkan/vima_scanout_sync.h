#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace gfxstream::host::vk {

// One instance per ColorBuffer allocation, shared by all native-buffer aliases.
// Unfenced KMS flushes do not prevent the guest's ASG decoder from reacquiring
// a buffer. Arbitrate that acquisition against display reads at the renderer.
class VimaScanoutSync {
public:
    void acquireGuest() {
        std::unique_lock<std::mutex> lock(mMutex);
        mChanged.wait(lock, [this] { return !mReading; });
        ++mGeneration;
        mReady = false;
        mReleasing = false;
    }

    uint64_t releaseGuest() {
        std::lock_guard<std::mutex> lock(mMutex);
        mReleasing = true;
        return mGeneration;
    }

    void producerCompleted(uint64_t generation) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (generation == mGeneration) {
            mReady = true;
            mReleasing = false;
            mChanged.notify_all();
        }
    }

    void producerFailed(uint64_t generation) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (generation == mGeneration) {
            mReleasing = false;
            mChanged.notify_all();
        }
    }

    bool tryAcquireDisplay() {
        std::unique_lock<std::mutex> lock(mMutex);
        if ((!mReady && !mReleasing) || mReading) return false;
        mReading = true;
        // Reserve before waiting: a guest fence callback may arrive before the
        // renderer publishes completion. Do not drop that otherwise valid frame.
        mChanged.wait(lock, [this] { return mReady || !mReleasing; });
        if (!mReady) {
            mReading = false;
            mChanged.notify_all();
            return false;
        }
        return true;
    }

    void releaseDisplay() {
        std::lock_guard<std::mutex> lock(mMutex);
        mReading = false;
        mChanged.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    uint64_t mGeneration = 0;
    bool mReady = false;
    bool mReleasing = false;
    bool mReading = false;
};

} // namespace gfxstream::host::vk
