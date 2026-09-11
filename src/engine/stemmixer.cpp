#include "engine/stemmixer.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kInt16Scale = 1.0f / 32767.0f;
constexpr float kUnityTolerance = 1e-4f;

} // namespace

StemMixer::StemMixer()
        : m_pTrack(nullptr) {
    m_targetGain.fill(1.0f);
    m_currentGain.fill(1.0f);
}

void StemMixer::setPlayPosition(SINT frame) {
    mixxx::StemTrack* pTrack = m_pTrack.load(std::memory_order_acquire);
    if (pTrack) {
        pTrack->setWantedFrame(frame);
    }
}

float StemMixer::readyFraction() const {
    mixxx::StemTrack* pTrack = m_pTrack.load(std::memory_order_acquire);
    if (!pTrack || pTrack->numRegions() == 0) {
        return 0.0f;
    }
    return static_cast<float>(pTrack->doneRegionCount()) / pTrack->numRegions();
}

bool StemMixer::isPlayheadReady(int regions) const {
    mixxx::StemTrack* pTrack = m_pTrack.load(std::memory_order_acquire);
    if (!pTrack || pTrack->numRegions() == 0) {
        return false;
    }
    const SINT first = mixxx::StemTrack::regionOf(pTrack->wantedFrame());
    const SINT last = std::min(first + regions - 1, pTrack->numRegions() - 1);
    for (SINT region = first; region <= last; ++region) {
        if (!pTrack->isRegionDone(region)) {
            return false;
        }
    }
    return true;
}

bool StemMixer::isActive() const {
    mixxx::StemTrack* pTrack = m_pTrack.load(std::memory_order_acquire);
    if (!pTrack) {
        return false;
    }
    const int numStems = pTrack->numStems();
    for (int k = 0; k < numStems; ++k) {
        if (std::fabs(m_targetGain[pTrack->slotOf(k)] - 1.0f) > kUnityTolerance) {
            return true;
        }
    }
    return false;
}

void StemMixer::apply(CSAMPLE* pBuffer,
        SINT startSample,
        SINT numSamples,
        bool reverse,
        mixxx::audio::ChannelCount channelCount) {
    mixxx::StemTrack* pTrack = m_pTrack.load(std::memory_order_acquire);
    if (!pTrack || channelCount != mixxx::audio::ChannelCount::stereo() || numSamples <= 0) {
        m_currentGain = m_targetGain;
        return;
    }
    const int numStems = pTrack->numStems();
    // Gains arrive per engine slot; the stems are stored densely.
    std::array<float, kMaxStems> target;
    std::array<float, kMaxStems> gain;
    bool unity = true;
    for (int k = 0; k < numStems; ++k) {
        const int slot = pTrack->slotOf(k);
        target[k] = m_targetGain[slot];
        gain[k] = m_currentGain[slot];
        if (std::fabs(target[k] - 1.0f) > kUnityTolerance ||
                std::fabs(gain[k] - 1.0f) > kUnityTolerance) {
            unity = false;
        }
    }
    if (unity) {
        return;
    }
    const SINT numFrames = numSamples / channelCount;
    // The reader fills a reverse read with the frames before startSample,
    // last frame first.
    const SINT firstFrame = startSample / channelCount;
    std::array<float, kMaxStems> step;
    for (int k = 0; k < numStems; ++k) {
        step[k] = (target[k] - gain[k]) / static_cast<float>(numFrames);
    }
    for (SINT i = 0; i < numFrames; ++i) {
        for (int k = 0; k < numStems; ++k) {
            gain[k] += step[k];
        }
        const SINT frame = reverse ? firstFrame - 1 - i : firstFrame + i;
        if (!pTrack->isFrameDone(frame)) {
            continue;
        }
        const int16_t* stems = pTrack->frameData(frame);
        float left = 0.0f;
        float right = 0.0f;
        for (int k = 0; k < numStems; ++k) {
            const float weight = (gain[k] - 1.0f) * kInt16Scale;
            left += weight * stems[k * 2];
            right += weight * stems[k * 2 + 1];
        }
        pBuffer[i * 2] += left;
        pBuffer[i * 2 + 1] += right;
    }
    m_currentGain = m_targetGain;
}
