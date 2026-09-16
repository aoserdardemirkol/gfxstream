// Copyright (C) 2019 The Android Open Source Project
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

#include "ring_stream.h"

#include <assert.h>
#include <memory.h>

#include "gfxstream/host/dma_device.h"
#include "gfxstream/common/logging.h"
#include "gfxstream/host/stream_utils.h"
#include "gfxstream/system/System.h"
#include "render-utils/dma_device.h"
#include "render-utils/stream.h"

namespace gfxstream {
namespace {

struct asg_context CreateContext(const AsgConsumerCreateInfo& info) {
    struct asg_context context = asg_context_create(info.ring_storage, info.buffer, info.buffer_size);

    context.ring_config->buffer_size = info.buffer_size;
    context.ring_config->flush_interval = info.buffer_flush_interval;
    context.ring_config->host_consumed_pos = 0;
    context.ring_config->guest_write_pos = 0;
    context.ring_config->transfer_mode = 1;
    context.ring_config->transfer_size = 0;
    context.ring_config->in_error = 0;

    return context;
}

void SaveRingConfig(Stream* stream, const struct asg_ring_config& config) {
    stream->putBe32(config.buffer_size);
    stream->putBe32(config.flush_interval);
    stream->putBe32(config.host_consumed_pos);
    stream->putBe32(config.guest_write_pos);
    stream->putBe32(config.transfer_mode);
    stream->putBe32(config.transfer_size);
    stream->putBe32(config.in_error);
}

void LoadRingConfig(Stream* stream, struct asg_ring_config* config) {
    config->buffer_size = stream->getBe32();
    config->flush_interval = stream->getBe32();
    config->host_consumed_pos = stream->getBe32();
    config->guest_write_pos = stream->getBe32();
    config->transfer_mode = stream->getBe32();
    config->transfer_size = stream->getBe32();
    config->in_error = stream->getBe32();
}

void SaveAsgContext(Stream* stream, const struct asg_context& context) {
    stream->write(context.to_host, sizeof(struct ring_buffer));
    stream->write(context.to_host_large_xfer.ring, sizeof(struct ring_buffer));
    stream->write(context.from_host_large_xfer.ring, sizeof(struct ring_buffer));
    stream->putBe32(context.buffer_size);
    stream->write(context.buffer, context.buffer_size);
}

void LoadAsgContext(Stream* stream, struct asg_context* context) {
    stream->read(context->to_host, sizeof(struct ring_buffer));
    stream->read(context->to_host_large_xfer.ring, sizeof(struct ring_buffer));
    stream->read(context->from_host_large_xfer.ring, sizeof(struct ring_buffer));
    context->buffer_size = stream->getBe32();
    stream->read(context->buffer, context->buffer_size);
}

}  // namespace

RingStream::RingStream(const AsgConsumerCreateInfo& info, size_t bufsize) :
    IOStream(bufsize),
    mContext(CreateContext(info)),
    mSavedRingConfig(*mContext.ring_config),
    mCallbacks(info.callbacks),
    mBufSize(info.buffer_size) {}

RingStream::~RingStream() = default;

void RingStream::reloadRingConfig() {
    *mContext.ring_config = mSavedRingConfig;
}

void* RingStream::allocBuffer(size_t minSize) {
    if (mWriteBuffer.size() < minSize) {
        mWriteBuffer.resize_noinit(minSize);
    }
    return mWriteBuffer.data();
}

int RingStream::commitBuffer(size_t size) {
    size_t sent = 0;
    auto data = mWriteBuffer.data();

    size_t iters = 0;
    size_t backedOffIters = 0;
    const size_t kBackoffIters = 10000000ULL;
    while (sent < size) {
        ++iters;
        auto avail = ring_buffer_available_write(
            mContext.from_host_large_xfer.ring,
            &mContext.from_host_large_xfer.view);

        // Check if the guest process crashed.
        if (!avail) {
            if (__atomic_load_n(mContext.host_state, __ATOMIC_SEQ_CST) ==
                ASG_HOST_STATE_EXIT) {
                return sent;
            } else {
                ring_buffer_yield();
                if (iters > kBackoffIters) {
                    gfxstream::base::sleepUs(10);
                    ++backedOffIters;
                }
            }
            continue;
        }

        auto remaining = size - sent;
        auto todo = remaining < avail ? remaining : avail;

        ring_buffer_view_write(
            mContext.from_host_large_xfer.ring,
            &mContext.from_host_large_xfer.view,
            data + sent, todo, 1);

        sent += todo;
    }

    if (backedOffIters > 0) {
        GFXSTREAM_WARNING(
            "Backed off %zu times to avoid overloading the guest system. This "
            "may indicate resource constraints or performance issues.",
            backedOffIters);
    }
    return sent;
}

const unsigned char* RingStream::readRaw(void* buf, size_t* inout_len) {
    size_t wanted = *inout_len;
    size_t count = 0U;
    auto dst = static_cast<char*>(buf);

    uint32_t ringAvailable = 0;
    uint32_t ringLargeXferAvailable = 0;

    const uint32_t maxSpins = 30;
    uint32_t spins = 0;
    bool inLargeXfer = true;

    // VIMA fork (0009): EVERY host_state access is seq-cst, not just the
    // NEED_NOTIFY publish that patch 0005 fixed.
    //
    // host_state lives in memory shared with the guest across Apple's
    // Virtualization framework (patch 0002), on weakly-ordered ARM64. 0005
    // correctly made the NEED_NOTIFY publish a seq-cst store with a fence and a
    // ring re-check, but left the CAN_CONSUME, RENDERING and EXIT transitions as
    // plain stores, and the EXIT check as a plain load. Those are the same
    // hazard on the other transitions: the compiler may sink or reorder them,
    // and the guest may observe them late or not at all.
    //
    // The consequence is a hard hang, and it is not theoretical -- it is what
    // Where Winds Meet does once it reaches gameplay (R5.88):
    //
    //   guest  vkQueueWaitIdle -> speculativeRead -> usleep   (awaiting a reply)
    //   host   RingStream::readRaw -> onUnavailableRead       (awaiting data)
    //
    // Both sides waiting on the other, no errors on either. A guest that reads a
    // stale CAN_CONSUME believes this consumer is actively polling and skips its
    // ASG_NOTIFY_AVAILABLE ping; the consumer then parks with the guest's data
    // already in the ring. Patch 0006's one-second backstop is supposed to
    // recover exactly that by re-reading the ring, and it does not here, which
    // points at visibility rather than at the wakeup.
    //
    // Making every transition seq-cst costs nothing measurable -- these are a
    // handful of stores per read, against ring traffic of megabytes -- and
    // removes the whole class.
    __atomic_store_n(mContext.host_state, ASG_HOST_STATE_CAN_CONSUME, __ATOMIC_SEQ_CST);

    while (count < wanted) {

        if (mReadBufferLeft) {
            size_t avail = std::min<size_t>(wanted - count, mReadBufferLeft);
            memcpy(dst + count,
                    mReadBuffer.data() + (mReadBuffer.size() - mReadBufferLeft),
                    avail);
            count += avail;
            mReadBufferLeft -= avail;
            continue;
        }

        mReadBuffer.clear();

        // no read buffer left...
        if (count > 0) {  // There is some data to return.
            break;
        }

        __atomic_store_n(mContext.host_state, ASG_HOST_STATE_CAN_CONSUME, __ATOMIC_SEQ_CST);

        if (mShouldExit) {
            return nullptr;
        }

        ringAvailable =
            ring_buffer_available_read(mContext.to_host, 0);
        ringLargeXferAvailable =
            ring_buffer_available_read(
                mContext.to_host_large_xfer.ring,
                &mContext.to_host_large_xfer.view);

        auto current = dst + count;
        auto ptrEnd = dst + wanted;

        if (ringAvailable) {
            inLargeXfer = false;
            uint32_t transferMode =
                mContext.ring_config->transfer_mode;
            switch (transferMode) {
                case 1:
                    type1Read(ringAvailable, dst, &count, &current, ptrEnd);
                    break;
                case 2:
                    type2Read(ringAvailable, &count, &current, ptrEnd);
                    break;
                case 3:
                    // emugl::emugl_crash_reporter(
                    //     "Guest should never set to "
                    //     "transfer mode 3 with ringAvailable != 0\n");
                default:
                    // emugl::emugl_crash_reporter(
                    //     "Unknown transfer mode %u\n",
                    //     transferMode);
                    break;
            }
        } else if (ringLargeXferAvailable) {
            type3Read(ringLargeXferAvailable,
                      &count, &current, ptrEnd);
            inLargeXfer = true;
            if (0 == __atomic_load_n(&mContext.ring_config->transfer_size, __ATOMIC_ACQUIRE)) {
                inLargeXfer = false;
            }
        } else {
            if (inLargeXfer && 0 != __atomic_load_n(&mContext.ring_config->transfer_size, __ATOMIC_ACQUIRE)) {
                continue;
            }

            if (inLargeXfer && 0 == __atomic_load_n(&mContext.ring_config->transfer_size, __ATOMIC_ACQUIRE)) {
                inLargeXfer = false;
            }

            if (++spins < maxSpins) {
                ring_buffer_yield();
                continue;
            } else {
                spins = 0;
            }

            if (mShouldExit) {
                return nullptr;
            }

            if (mShouldExitForSnapshot && mInSnapshotOperation) {
                return nullptr;
            }

            ++mUnavailableReadCount;
            if (mUnavailableReadCount >= kMaxUnavailableReads) {
                // VIMA fork (0005): publish NEED_NOTIFY with a seq-cst store and
                // a full fence, then re-check the ring before parking. Upstream's
                // plain store is only safe on x86-TSO; on the Apple VZ shm ring
                // (patch 0002, weakly ordered ARM64) the guest can read
                // host_state stale, skip its ASG_NOTIFY_AVAILABLE ping, and this
                // consumer would then block forever in onUnavailableRead() with
                // data sitting in the ring — wedging the context and pinning any
                // sibling RenderThread that spins on the process seqno. The
                // seq-cst store + re-check establishes a total order: either the
                // guest observes NEED_NOTIFY and pings, or this re-check observes
                // the data the guest already wrote. Idle contexts still block at
                // zero CPU.
                __atomic_store_n(mContext.host_state, ASG_HOST_STATE_NEED_NOTIFY,
                                 __ATOMIC_SEQ_CST);
                __atomic_thread_fence(__ATOMIC_SEQ_CST);
                if (ring_buffer_available_read(mContext.to_host, 0) ||
                    ring_buffer_available_read(mContext.to_host_large_xfer.ring,
                                               &mContext.to_host_large_xfer.view)) {
                    __atomic_store_n(mContext.host_state, ASG_HOST_STATE_CAN_CONSUME,
                                     __ATOMIC_SEQ_CST);
                    mUnavailableReadCount = 0;
                    continue;
                }

                // VIMA fork (DIAG R5.92): if this consumer stays parked while the
                // guest is waiting on us, say what the ring actually contains.
                //
                // The remaining Where Winds Meet freeze has the guest polling in
                // vkQueueWaitIdle -> speculativeRead for a reply while every host
                // consumer sits here. Patch 0006's one-second backstop is running
                // (the stack shows beforeTimedRead/timedWait), the ring's own
                // read/write positions are seq-cst, and R5.90/0010 rule out the
                // seqno and unknown-opcode deadlocks -- so reading more code is
                // not going to settle whether the guest's command is sitting
                // unread or never arrived. Print it.
                //
                // Once per consumer after ~3 s parked, not per iteration.
                {
                    const uint64_t now = gfxstream::base::getUnixTimeUs();
                    if (!mVimaParkStartUs) mVimaParkStartUs = now;
                    if (!mVimaParkLogged && now - mVimaParkStartUs > 3000000) {
                        mVimaParkLogged = true;
                        // R5.93: dump the WHOLE transport state, not just the
                        // ring. The guest hangs in ensureType1Finished(), which
                        // waits on the AUXILIARY BUFFER protocol in
                        // asg_ring_config -- not on the to_host ring at all. That
                        // is why the earlier probe showed avail=0 and looked
                        // innocent: it was reading the wrong side of the
                        // transport.
                        //
                        // CORRECTION (R5.94): the guest_write_pos vs
                        // host_consumed_pos "discriminator" this comment used to
                        // describe is WRONG for this hang, and the data proved
                        // it. Those two fields belong to the auxiliary-buffer
                        // reuse sub-protocol (advanceWrite/get_available_for_write),
                        // NOT to draining the descriptor ring.
                        // ensureType1Finished never reads either one -- confirmed
                        // from the guest disassembly, which touches only the
                        // to_host ring and ring_config.in_error. The measured
                        // values (guest_write_pos=0 while host_consumed_pos=509595)
                        // are a third outcome the discriminator did not allow for,
                        // which is what exposed the mistake. Kept in the dump
                        // because they are cheap and occasionally informative, but
                        // do not reason about THIS hang from them.
                        //
                        // in_error IS load-bearing: it is the one field the
                        // guest's spin checks every iteration regardless of
                        // read_pos visibility, which is what makes the R5.94
                        // watchdog below possible.
                        const asg_ring_config* cfg = mContext.ring_config;
                        GFXSTREAM_ERROR(
                            "VIMA-R5.93 parked>3s host_state=%u | to_host w=%u r=%u avail=%d | "
                            "to_host_lg w=%u r=%u | from_host_lg w=%u r=%u | "
                            "cfg{guest_write_pos=%u host_consumed_pos=%u mode=%u size=%u "
                            "in_error=%u bufsz=%u flush=%u} | xmits=%llu recv=%llu",
                            __atomic_load_n(mContext.host_state, __ATOMIC_SEQ_CST),
                            __atomic_load_n(&mContext.to_host->write_pos, __ATOMIC_SEQ_CST),
                            __atomic_load_n(&mContext.to_host->read_pos, __ATOMIC_SEQ_CST),
                            (int)ring_buffer_available_read(mContext.to_host, 0),
                            __atomic_load_n(&mContext.to_host_large_xfer.ring->write_pos,
                                            __ATOMIC_SEQ_CST),
                            __atomic_load_n(&mContext.to_host_large_xfer.ring->read_pos,
                                            __ATOMIC_SEQ_CST),
                            __atomic_load_n(&mContext.from_host_large_xfer.ring->write_pos,
                                            __ATOMIC_SEQ_CST),
                            __atomic_load_n(&mContext.from_host_large_xfer.ring->read_pos,
                                            __ATOMIC_SEQ_CST),
                            __atomic_load_n(&cfg->guest_write_pos, __ATOMIC_SEQ_CST),
                            __atomic_load_n(&cfg->host_consumed_pos, __ATOMIC_SEQ_CST),
                            __atomic_load_n(&cfg->transfer_mode, __ATOMIC_SEQ_CST),
                            __atomic_load_n(&cfg->transfer_size, __ATOMIC_SEQ_CST),
                            __atomic_load_n(&cfg->in_error, __ATOMIC_SEQ_CST),
                            cfg->buffer_size, cfg->flush_interval,
                            (unsigned long long)mXmits, (unsigned long long)mTotalRecv);
                    }
                    // NOTE: an earlier version logged "DATA PENDING - lost wakeup
                    // confirmed" here whenever the ring was non-empty at this
                    // point. That was wrong and it fired 317 times in a session
                    // that was mostly healthy. Reaching here means the re-check
                    // above already found the ring empty, so finding data now
                    // just means it arrived in the interval -- a race the
                    // NEED_NOTIFY publish and the one-second backstop both
                    // recover from. Only data still pending AFTER a long park is
                    // evidence of anything, and the parked>3s line above reports
                    // exactly that. Measured on the real freeze: every consumer
                    // parked >3s had avail=0, i.e. NO lost wakeup.
                }
                // VIMA fork (0011 / R5.94): bound the damage of a read_pos the
                // guest never observes.
                //
                // The guest spins in ensureType1Finished() until
                // ring_buffer_available_read(to_host) == 0. It computes that from
                // a PLAIN, non-atomic load of to_host->read_pos -- the field WE
                // own and advance with a SEQ_CST RMW. Mixing a seq-cst writer with
                // a plain reader is a data race, and this transport has already
                // produced exactly this failure once before, on host_state, fixed
                // by patch 0009. read_pos was never audited then.
                //
                // Unlike ensureConsumerFinishing() (which host_state drives, and
                // which has a ping/notify escape), this loop has NO escape: if our
                // advance never becomes visible, nothing brings the guest back.
                // Measured at the freeze: we see to_host w == r (avail=0) while
                // the guest keeps spinning, i.e. the two sides disagree about the
                // same ring.
                //
                // We cannot fix the guest's load -- it is a prebuilt APEX binary.
                // Two host-only mitigations, in escalating order:
                //
                //   1. Re-publish read_pos with a fresh SEQ_CST store + fence,
                //      once per second while parked. Numerically a no-op; the
                //      point is new coherence traffic. If the original advance
                //      simply never crossed, this may carry it. THIS IS ALSO THE
                //      EXPERIMENT: if re-publishing alone unwedges the guest, the
                //      cause is visibility (H1). If only step 2 ever helps, it is
                //      not, and the mapping itself is the next suspect (H2).
                //
                //   2. After 10 s, set in_error -- the ONE field this guest loop
                //      checks every iteration regardless of read_pos visibility.
                //      That releases the guest with an error rather than leaving
                //      it hung forever, and lets the existing 0005/0006 backstop
                //      retire this consumer. A lost context beats a lost session.
                //
                // Both re-verify the ring is STILL drained on a fresh read, so a
                // straggler arriving in the interim is never mistaken for a hang.
                if (mVimaParkStartUs) {
                    const uint64_t parkedUs =
                        gfxstream::base::getUnixTimeUs() - mVimaParkStartUs;
                    if (parkedUs > 1000000 && !ring_buffer_available_read(mContext.to_host, 0)) {
                        uint32_t rp;
                        __atomic_load(&mContext.to_host->read_pos, &rp, __ATOMIC_SEQ_CST);
                        __atomic_store_n(&mContext.to_host->read_pos, rp, __ATOMIC_SEQ_CST);
                        __atomic_thread_fence(__ATOMIC_SEQ_CST);
                    }
                    // DO NOT force in_error here. This was tried and it is WRONG.
                    //
                    // The idea was: parked >10s with the ring provably drained
                    // means the guest is stuck on a read_pos it never observed, so
                    // release it via the one field its loop checks. The flaw is
                    // that "parked with an empty ring" is ALSO the completely
                    // normal idle state of every context that simply has nothing
                    // to do, and from the host those two are indistinguishable --
                    // no ring state, counter or flag separates "guest spinning in
                    // ensureType1Finished waiting for us" from "guest asleep".
                    //
                    // Measured, on a freshly booted system with NO game running:
                    // 44 contexts had in_error forced on them within a minute.
                    // That is a fault injected into perfectly healthy contexts,
                    // strictly worse than the hang it was meant to bound.
                    //
                    // The re-publish above stays: it is numerically a no-op, it
                    // cannot harm an idle context, and it is the actual experiment
                    // -- if the guest recovers within a second of parking, the
                    // cause is read_pos visibility.
                    //
                    // A real escape needs a signal that distinguishes waiting from
                    // idle, which the host does not currently have. The honest
                    // options are: have the guest publish "I am waiting" (needs a
                    // guest rebuild), or find such a signal. Do not reintroduce a
                    // timeout-only version of this.
                    (void)mVimaForcedError;
                }

                bool sleeping = false;
                do {
                    const AsgOnUnavailableReadStatus status = mCallbacks.onUnavailableRead();
                    switch (status) {
                        case AsgOnUnavailableReadStatus::kContinue: {
                            __atomic_store_n(mContext.host_state,
                                             ASG_HOST_STATE_CAN_CONSUME,
                                             __ATOMIC_SEQ_CST);
                            break;
                        }
                        case AsgOnUnavailableReadStatus::kExit: {
                            __atomic_store_n(mContext.host_state, ASG_HOST_STATE_EXIT,
                                             __ATOMIC_SEQ_CST);
                            mShouldExit = true;
                            break;
                        }
                        case AsgOnUnavailableReadStatus::kSleep: {
                            sleeping = true;
                            break;
                        }
                        case AsgOnUnavailableReadStatus::kPauseForSnapshot: {
                            mShouldExitForSnapshot = true;
                            break;
                        }
                        case AsgOnUnavailableReadStatus::kResumeAfterSnapshot: {
                            mShouldExitForSnapshot = false;
                            break;
                        }
                    }
                } while (sleeping);
            }
            continue;
        }
    }

    *inout_len = count;
    ++mXmits;
    mTotalRecv += count;

    // Reset the park clock: this consumer is demonstrably alive. Without this the
    // R5.94 watchdog would measure from the FIRST ever park and eventually fire
    // in_error on a perfectly healthy context.
    mVimaParkStartUs = 0;
    mVimaParkLogged = false;
    mVimaForcedError = false;

    __atomic_store_n(mContext.host_state, ASG_HOST_STATE_RENDERING, __ATOMIC_SEQ_CST);
    return (const unsigned char*)buf;
}

void RingStream::type1Read(
    uint32_t available,
    char* begin,
    size_t* count, char** current, const char* ptrEnd) {

    uint32_t xferTotal = available / sizeof(struct asg_type1_xfer);

    if (mType1Xfers.size() < xferTotal) {
        mType1Xfers.resize(xferTotal * 2);
    }

    auto xfersPtr = mType1Xfers.data();

    ring_buffer_copy_contents(
        mContext.to_host, 0, xferTotal * sizeof(struct asg_type1_xfer), (uint8_t*)xfersPtr);

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunreachable-code-loop-increment"
#endif // __clang__
    for (uint32_t i = 0; i < xferTotal; ++i) {
        const asg_type1_xfer& xfer = xfersPtr[i];

        const uint32_t buffer_size = mContext.buffer_size;

        // Guest controls offset/size via shared memory; validate against the
        // host-allocated auxiliary buffer before dereferencing.
        if (xfer.offset >= buffer_size || xfer.size > buffer_size - xfer.offset) {
            GFXSTREAM_ERROR("Invalid type1 xfer: offset %u, size %u, buffer_size %u\n",
                            xfer.offset, xfer.size, buffer_size);
            __atomic_store_n(&mContext.ring_config->in_error, 1, __ATOMIC_RELEASE);
            return;
        }

        if (*current + xfer.size > ptrEnd) {
            // Save in a temp buffer or we'll get stuck
            if (begin == *current && i == 0) {
                const char* src = mContext.buffer + xfer.offset;
                mReadBuffer.resize_noinit(xfer.size);
                memcpy(mReadBuffer.data(), src, xfer.size);
                mReadBufferLeft = xfer.size;
                ring_buffer_advance_read(
                        mContext.to_host, sizeof(struct asg_type1_xfer), 1);
                __atomic_fetch_add(&mContext.ring_config->host_consumed_pos, xfersPtr[i].size, __ATOMIC_RELEASE);
            }
            return;
        }
        const char* src = mContext.buffer + xfer.offset;
        memcpy(*current, src, xfer.size);
        ring_buffer_advance_read(
                mContext.to_host, sizeof(struct asg_type1_xfer), 1);
        __atomic_fetch_add(&mContext.ring_config->host_consumed_pos, xfer.size, __ATOMIC_RELEASE);
        *current += xfer.size;
        *count += xfer.size;

        // TODO: Figure out why running multiple xfers here can result in data
        // corruption and remove clang diagnostic block.
        return;
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif // __clang__
}

void RingStream::type2Read(
    uint32_t available,
    size_t* count, char** current,const char* ptrEnd) {

    GFXSTREAM_FATAL("nyi. abort");

    uint32_t xferTotal = available / sizeof(struct asg_type2_xfer);

    if (mType2Xfers.size() < xferTotal) {
        mType2Xfers.resize(xferTotal * 2);
    }

    auto xfersPtr = mType2Xfers.data();

    ring_buffer_copy_contents(
        mContext.to_host, 0, available, (uint8_t*)xfersPtr);

    for (uint32_t i = 0; i < xferTotal; ++i) {

        if (*current + xfersPtr[i].size > ptrEnd) return;

        const char* src =
            mCallbacks.getPtr(xfersPtr[i].physAddr);

        memcpy(*current, src, xfersPtr[i].size);

        ring_buffer_advance_read(
            mContext.to_host, sizeof(struct asg_type1_xfer), 1);

        *current += xfersPtr[i].size;
        *count += xfersPtr[i].size;
    }
}

void RingStream::type3Read(
    uint32_t available,
    size_t* count, char** current, const char* ptrEnd) {

    uint32_t xferTotal = __atomic_load_n(&mContext.ring_config->transfer_size, __ATOMIC_ACQUIRE);
    uint32_t maxCanRead = ptrEnd - *current;
    uint32_t ringAvail = available;
    uint32_t actuallyRead = std::min(ringAvail, std::min(xferTotal, maxCanRead));

    // Decrement transfer_size before letting the guest proceed in ring_buffer funcs or we will race
    // to the next time the guest sets transfer_size
    __atomic_fetch_sub(&mContext.ring_config->transfer_size, actuallyRead, __ATOMIC_RELEASE);

    ring_buffer_read_fully_with_abort(
            mContext.to_host_large_xfer.ring,
            &mContext.to_host_large_xfer.view,
            *current, actuallyRead,
            1, &mContext.ring_config->in_error);

    *current += actuallyRead;
    *count += actuallyRead;
}

void* RingStream::getDmaForReading(uint64_t guest_paddr) {
    return gfxstream::host::g_gfxstream_dma_get_host_addr(guest_paddr);
}

void RingStream::unlockDma(uint64_t guest_paddr) {
    gfxstream::host::g_gfxstream_dma_unlock(guest_paddr);
}

int RingStream::writeFully(const void* buf, size_t len) {
    void* dstBuf = alloc(len);
    memcpy(dstBuf, buf, len);
    flush();
    return 0;
}

const unsigned char *RingStream::readFully( void *buf, size_t len) {
    GFXSTREAM_FATAL("not intended for use with RingStream");
    return nullptr;
}

void RingStream::onSave(gfxstream::Stream* stream) {
    stream->putBe32(mReadBufferLeft);
    stream->write(mReadBuffer.data() + mReadBuffer.size() - mReadBufferLeft,
                  mReadBufferLeft);

    gfxstream::host::saveBuffer(stream, mWriteBuffer);

    stream->putBe32(mUnavailableReadCount);

    mSavedRingConfig = *mContext.ring_config;

    SaveRingConfig(stream, mSavedRingConfig);

    SaveAsgContext(stream, mContext);
}

unsigned char* RingStream::onLoad(gfxstream::Stream* stream) {
    gfxstream::host::loadBuffer(stream, &mReadBuffer);
    mReadBufferLeft = mReadBuffer.size();

    gfxstream::host::loadBuffer(stream, &mWriteBuffer);

    mUnavailableReadCount = stream->getBe32();

    LoadRingConfig(stream, &mSavedRingConfig);

    LoadAsgContext(stream, &mContext);

    return reinterpret_cast<unsigned char*>(mWriteBuffer.data());
}

}  // namespace gfxstream
