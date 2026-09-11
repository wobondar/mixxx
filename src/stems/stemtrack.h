#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

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
///
/// Next to the audio the track keeps a visual copy for the waveform: one
/// byte per stem per channel per visual frame, on the grid the waveform
/// analyzer uses, filled by the worker as each region finishes. It is
/// published through the same region flag as the audio, so a reader that
/// sees a region done sees its bytes too. Undone regions read as zero. The
/// bytes are atomic because a visual frame on a region boundary is written
/// again when the neighbouring region finishes, after readers may have it.
///
/// Stems are stored densely but addressed by the deck through engine stem
/// slots, so a stem keeps its slot across modes with fewer stems; the
/// constructor takes the slot of each stored stem.
class StemTrack {
  public:
    static constexpr SINT kRegionFrames = 512 * 1024;
    /// Frames the leading zero frame of the STFT shifts the split grid by.
    static constexpr SINT kRegionOffset = 4096;
    /// The waveform analyzer's visual sample rate.
    static constexpr int kVisualSampleRate = 441;
    static constexpr int kNoStem = -1;

    StemTrack(SINT numFrames, std::vector<int> stemSlots, int sampleRate, bool rmsEnvelope)
            : m_numFrames(numFrames),
              m_slots(std::move(stemSlots)),
              m_numStems(static_cast<int>(m_slots.size())),
              m_numRegions(regionOf(numFrames - 1) + 1),
              m_visualRatio(sampleRate > kVisualSampleRate
                              ? static_cast<double>(sampleRate) / kVisualSampleRate
                              : 1.0),
              m_numVisualFrames(static_cast<SINT>(numFrames / m_visualRatio) + 1),
              m_rmsEnvelope(rmsEnvelope),
              m_data(new int16_t[static_cast<size_t>(numFrames) * m_numStems * 2]),
              m_visual(new std::atomic<uint8_t>[static_cast<size_t>(m_numVisualFrames) *
                      m_numStems * 2]()),
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
    static SINT regionFirstFrame(SINT region) {
        return std::max<SINT>(0, region * kRegionFrames - kRegionOffset);
    }

    SINT numFrames() const {
        return m_numFrames;
    }
    int numStems() const {
        return m_numStems;
    }
    int slotOf(int stem) const {
        return m_slots[stem];
    }
    /// Stored stem index of an engine slot, kNoStem for an empty slot.
    int stemOfSlot(int slot) const {
        for (int stem = 0; stem < m_numStems; ++stem) {
            if (m_slots[stem] == slot) {
                return stem;
            }
        }
        return kNoStem;
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
            renderVisual(region);
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

    double visualRatio() const {
        return m_visualRatio;
    }
    SINT numVisualFrames() const {
        return m_numVisualFrames;
    }
    /// Bytes of one visual frame: stem0 L, stem0 R, stem1 L, ...
    const std::atomic<uint8_t>* visualData(SINT visualFrame) const {
        return m_visual.get() + static_cast<size_t>(visualFrame) * m_numStems * 2;
    }
    SINT visualFrameOf(SINT frame) const {
        return static_cast<SINT>(frame / m_visualRatio);
    }

  private:
    // A visual frame on a region boundary gets the max of both sides, each
    // written when its region finishes.
    void renderVisual(SINT region) {
        const SINT first = regionFirstFrame(region);
        const SINT end = std::min(m_numFrames, regionFirstFrame(region + 1));
        const int lanes = m_numStems * 2;
        SINT frame = first;
        while (frame < end) {
            const SINT visualFrame = visualFrameOf(frame);
            const SINT stop = std::min(end,
                    static_cast<SINT>(std::ceil((visualFrame + 1) * m_visualRatio)));
            std::atomic<uint8_t>* out = m_visual.get() + static_cast<size_t>(visualFrame) * lanes;
            for (int lane = 0; lane < lanes; ++lane) {
                const int16_t* in = m_data.get() + static_cast<size_t>(frame) * lanes + lane;
                float amplitude = 0.0f;
                if (m_rmsEnvelope) {
                    double sum = 0.0;
                    for (SINT f = frame; f < stop; ++f, in += lanes) {
                        const double v = *in;
                        sum += v * v;
                    }
                    // Scaled so a full-scale sine lands on the byte its peak would
                    constexpr float kSineRmsToPeak = 1.41421356f;
                    amplitude = static_cast<float>(std::sqrt(sum / (stop - frame))) *
                            kSineRmsToPeak;
                } else {
                    int peak = 0;
                    for (SINT f = frame; f < stop; ++f, in += lanes) {
                        peak = std::max(peak, std::abs(static_cast<int>(*in)));
                    }
                    amplitude = static_cast<float>(peak);
                }
                const int byte = std::min(255,
                        static_cast<int>(amplitude * 255.0f / 32768.0f + 0.5f));
                // The worker is the only writer, so load and store need no CAS
                const int previous = out[lane].load(std::memory_order_relaxed);
                out[lane].store(static_cast<uint8_t>(std::max(previous, byte)),
                        std::memory_order_relaxed);
            }
            frame = stop;
        }
    }

    const SINT m_numFrames;
    const std::vector<int> m_slots;
    const int m_numStems;
    const SINT m_numRegions;
    const double m_visualRatio;
    const SINT m_numVisualFrames;
    const bool m_rmsEnvelope;
    // Left uninitialized on purpose: frames of undone regions are never read
    std::unique_ptr<int16_t[]> m_data;
    std::unique_ptr<std::atomic<uint8_t>[]> m_visual;
    std::unique_ptr<std::atomic<uint8_t>[]> m_regionDone;
    std::atomic<SINT> m_wantedFrame;
};

using StemTrackPointer = std::shared_ptr<StemTrack>;

} // namespace mixxx
