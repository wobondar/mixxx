#pragma once

#include <random>
#include <vector>

#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class NoiseSweepGroupState : public EffectState {
  public:
    NoiseSweepGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters),
              gen(std::random_device()()),
              dist(-1.0f, 1.0f) {
        const double fs = engineParameters.sampleRate();
        echoLen = static_cast<int>(fs * 4.0) + 2; // 4 s ceiling
        for (int c = 0; c < 2; c++) {
            echoBuf[c].assign(echoLen, 0.0f);
        }
        clear();
    }
    ~NoiseSweepGroupState() override = default;

    void clear() {
        for (int c = 0; c < 2; c++) {
            std::fill(echoBuf[c].begin(), echoBuf[c].end(), 0.0f);
            bpX1[c] = bpX2[c] = bpY1[c] = bpY2[c] = 0.0f;
            holdValue[c] = 0.0f;
        }
        echoWrite = 0;
        modPhase = 0.0;
        holdCounter = 0.0;
        samplesSinceEnable = 0;
        oscGain = 0.0f;
        gateNormal = 1.0f;
        gateFrozen = 0.0f;
        gateEchoIn = 1.0f;
        feedback = 0.5f;
    }

    std::minstd_rand gen;
    std::uniform_real_distribution<float> dist;
    int echoLen;
    std::vector<float> echoBuf[2];
    int echoWrite;
    // band-pass biquad state per channel
    float bpX1[2], bpX2[2], bpY1[2], bpY2[2];
    // sample-and-hold decimator state (crush)
    float holdValue[2];
    double holdCounter;
    double modPhase; // triangle LFO phase, wraps at 2
    long samplesSinceEnable;
    float oscGain;   // ramped
    // freeze gates (ramped) and the echo feedback (switched)
    float gateNormal;
    float gateFrozen;
    float gateEchoIn;
    float feedback;
};

class NoiseSweepEffect : public EffectProcessorImpl<NoiseSweepGroupState> {
  public:
    NoiseSweepEffect() = default;
    ~NoiseSweepEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            NoiseSweepGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pFreqParameter;
    EngineEffectParameterPointer m_pLevelParameter;
    EngineEffectParameterPointer m_pModParameter;
    EngineEffectParameterPointer m_pCrushParameter;
    EngineEffectParameterPointer m_pFreezeParameter;

    DISALLOW_COPY_AND_ASSIGN(NoiseSweepEffect);
};
