#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "util/types.h"

namespace mixxx {

/// Separated stems for one loaded track, produced by the estimator worker and
/// read by the engine thread. Storage is interleaved per frame:
/// stem0 L, stem0 R, stem1 L, stem1 R, ... as int16.
///
/// The separator finalizes audio in regions that follow the model's split
/// grid. A region is marked done only once every STFT frame contributing to
/// its samples has been overlap-added, so the engine never sees a partial
/// region. Reads check the flag of each frame's region and treat frames of an
/// unfinished region as "no stems yet".
class StemTrack {
  public:
    static constexpr SINT kRegionFrames = 512 * 1024;
    /// Frames the leading zero frame of the STFT shifts the split grid by.
    static constexpr SINT kRegionOffset = 4096;

    StemTrack(SINT numFrames, int numStems)
            : m_numFrames(numFrames),
              m_numStems(numStems),
              m_numRegions(regionOf(numFrames - 1) + 1),
              m_data(new int16_t[static_cast<size_t>(numFrames) * numStems * 2]),
              m_regionDone(new std::atomic<uint8_t>[m_numRegions]),
              m_wantedFrame(0) {
        for (SINT i = 0; i < m_numRegions; ++i) {
            m_regionDone[i].store(0, std::memory_order_relaxed);
        }
    }

    /// Playhead hint written by the engine thread; the worker separates the
    /// region holding it first.
    void setWantedFrame(SINT frame) {
        m_wantedFrame.store(frame, std::memory_order_relaxed);
    }
    SINT wantedFrame() const {
        return m_wantedFrame.load(std::memory_order_relaxed);
    }

    static SINT regionOf(SINT frame) {
        return (frame + kRegionOffset) / kRegionFrames;
    }

    SINT numFrames() const {
        return m_numFrames;
    }
    int numStems() const {
        return m_numStems;
    }
    SINT numRegions() const {
        return m_numRegions;
    }

    bool isRegionDone(SINT region) const {
        return region >= 0 && region < m_numRegions &&
                m_regionDone[region].load(std::memory_order_acquire) != 0;
    }
    bool isFrameDone(SINT frame) const {
        return frame >= 0 && frame < m_numFrames && isRegionDone(regionOf(frame));
    }
    void markRegionDone(SINT region) {
        if (region >= 0 && region < m_numRegions) {
            m_regionDone[region].store(1, std::memory_order_release);
        }
    }
    SINT doneRegionCount() const {
        SINT count = 0;
        for (SINT i = 0; i < m_numRegions; ++i) {
            count += m_regionDone[i].load(std::memory_order_relaxed);
        }
        return count;
    }

    /// First region at or after the given one that is not done yet.
    SINT firstUndoneRegionFrom(SINT region) const {
        while (region < m_numRegions && isRegionDone(region)) {
            ++region;
        }
        return region;
    }

    const int16_t* frameData(SINT frame) const {
        return m_data.get() + static_cast<size_t>(frame) * m_numStems * 2;
    }
    int16_t* frameData(SINT frame) {
        return m_data.get() + static_cast<size_t>(frame) * m_numStems * 2;
    }

  private:
    const SINT m_numFrames;
    const int m_numStems;
    const SINT m_numRegions;
    // Left uninitialized on purpose: frames of undone regions are never read
    std::unique_ptr<int16_t[]> m_data;
    std::unique_ptr<std::atomic<uint8_t>[]> m_regionDone;
    std::atomic<SINT> m_wantedFrame;
};

using StemTrackPointer = std::shared_ptr<StemTrack>;

} // namespace mixxx
