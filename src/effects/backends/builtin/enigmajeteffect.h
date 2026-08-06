#pragma once

#include <vector>

#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class EnigmaJetGroupState : public EffectState {
  public:
    EnigmaJetGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        const double fs = engineParameters.sampleRate();
        minDelay = static_cast<int>(fs / 10000.0);
        range = static_cast<int>(fs / 500.0);
        bufLen = minDelay + range + 2;
        for (int k = 0; k < 4; k++) {
            line[k].assign(bufLen, 0.0f);
        }
        clear();
    }
    ~EnigmaJetGroupState() override = default;

    void clear() {
        for (int k = 0; k < 4; k++) {
            std::fill(line[k].begin(), line[k].end(), 0.0f);
        }
        writeIdx = 0;
        phase = 0.0;
        modPhase = 0.0;
        dryGain = 1.0f;
        wetGain = 0.0f;
        for (int c = 0; c < 2; c++) {
            w1[c] = 0.0f;
            w2[c] = 0.0f;
        }
    }

    int minDelay;
    int range;
    int bufLen;
    std::vector<float> line[4];
    int writeIdx;
    double phase;    // sweep phase, [0,1)
    double modPhase; // modulation LFO phase, wraps at 2
    float dryGain;
    float wetGain;
    // band-split biquad state, per channel
    float w1[2];
    float w2[2];
};

class EnigmaJetEffect : public EffectProcessorImpl<EnigmaJetGroupState> {
  public:
    EnigmaJetEffect() = default;
    ~EnigmaJetEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            EnigmaJetGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pBeatsParameter;
    EngineEffectParameterPointer m_pDepthParameter;
    EngineEffectParameterPointer m_pFeedbackParameter;
    EngineEffectParameterPointer m_pModParameter;
    EngineEffectParameterPointer m_pFreezeParameter;

    DISALLOW_COPY_AND_ASSIGN(EnigmaJetEffect);
};
