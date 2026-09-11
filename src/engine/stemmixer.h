#pragma once

#include <array>
#include <atomic>

#include "audio/types.h"
#include "engine/engine.h"
#include "stems/stemtrack.h"
#include "util/types.h"

/// Applies separated stems to the stereo signal read from the caching
/// reader, before the scaler:
///
///     out = mix + sum_k (g_k - 1) * stem_k
///
/// With every gain at unity the file plays untouched. Frames whose region
/// the separator has not finished are left alone, so a fader touched ahead
/// of the frontier has no effect there rather than producing garbage.
/// Gains ramp linearly over each read to stay click-free.
///
/// Engine thread only, except setTrack, which the owner calls from the main
/// thread while keeping the previous track alive until the next swap.
class StemMixer {
  public:
    static constexpr int kMaxStems = mixxx::kMaxSupportedStems;

    StemMixer();

    void setTrack(mixxx::StemTrack* pTrack) {
        m_pTrack.store(pTrack, std::memory_order_release);
    }
    bool hasTrack() const {
        return m_pTrack.load(std::memory_order_relaxed) != nullptr;
    }
    /// Effective gain of a stem, 0 when muted. Engine thread.
    void setGain(int stem, float gain) {
        m_targetGain[stem] = gain;
    }
    /// Tells the separator where the deck is, so it works ahead of it.
    void setPlayPosition(SINT frame);
    /// Fraction of the track's regions the separator has finished.
    float readyFraction() const;
    /// True when the region under the playhead and the given number of
    /// regions from it onward are finished, clamped to the end of the
    /// track so the outro can report ready.
    bool isPlayheadReady(int regions) const;
    /// True while any gain is off unity on a track with stems.
    bool isActive() const;

    void apply(CSAMPLE* pBuffer,
            SINT startSample,
            SINT numSamples,
            bool reverse,
            mixxx::audio::ChannelCount channelCount);

  private:
    std::atomic<mixxx::StemTrack*> m_pTrack;
    std::array<float, kMaxStems> m_targetGain;
    std::array<float, kMaxStems> m_currentGain;
};
