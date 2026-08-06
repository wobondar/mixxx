#pragma once

#include <vector>

#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class MobiusSawGroupState : public EffectState {
  public:
    MobiusSawGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        const double fs = engineParameters.sampleRate();
        lenA = static_cast<int>(fs * 0.080544218);
        lenB = static_cast<int>(fs * 0.012335601);
        bufA.assign(lenA, 0.0f);
        bufB.assign(lenB, 0.0f);
        clear();
    }
    ~MobiusSawGroupState() override = default;

    void clear() {
        std::fill(bufA.begin(), bufA.end(), 0.0f);
        std::fill(bufB.begin(), bufB.end(), 0.0f);
        wa = 0;
        wb = 0;
        masterPhase = 0.0;
        modPhase = 0.0;
        toneGain = 0.0f;
        for (int k = 0; k < 4; k++) {
            stage[k] = 2 * k; // {0, 2, 4, 6}
            oscPhase[k] = 0.0;
        }
    }

    int lenA;
    int lenB;
    std::vector<float> bufA;
    std::vector<float> bufB;
    int wa;
    int wb;
    double masterPhase; // glissando phase within one stage, [0,1)
    double modPhase;    // vibrato LFO phase, wraps at 2
    float toneGain;
    int stage[4];       // 0..7, 7 = silent
    double oscPhase[4];
};

class MobiusSawEffect : public EffectProcessorImpl<MobiusSawGroupState> {
  public:
    MobiusSawEffect() = default;
    ~MobiusSawEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            MobiusSawGroupState* pState,
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
    EngineEffectParameterPointer m_pEchoParameter;
    EngineEffectParameterPointer m_pModParameter;
    EngineEffectParameterPointer m_pFreezeParameter;

    DISALLOW_COPY_AND_ASSIGN(MobiusSawEffect);
};
