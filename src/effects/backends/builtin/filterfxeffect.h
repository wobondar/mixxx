#pragma once

#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class FilterFXGroupState : public EffectState {
  public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kTableSize = 129;

    FilterFXGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters),
              currentFrame(0),
              tableRate(0),
              rg(0),
              rgTarget(0),
              rgStep(0),
              prevDry(1.0f),
              hpfMode(false),
              phaseInv(false),
              gateGain(1.0f),
              gateClosing(false) {
        clearFilters();
    }
    ~FilterFXGroupState() override = default;

    void clearFilters() {
        for (int c = 0; c < kMaxChannels; c++) {
            for (int i = 0; i < 7; i++) {
                state[c][i] = 0.0f;
            }
        }
    }

    double currentFrame;
    double tableRate; // sample rate the coefficient tables were built for
    // low-pass-mode and high-pass-mode coefficient table pairs
    float mainTabLp[kTableSize];
    float resoTabLp[kTableSize];
    float mainTabHp[kTableSize];
    float resoTabHp[kTableSize];
    // per-channel filter memory: [w0, y1, y2, y3, y4, y5, y6]
    float state[kMaxChannels][7];
    // resonance-gain slew (linear ramp)
    float rg;
    float rgTarget;
    float rgStep;
    float prevDry;
    // active (post-gate) topology switches
    bool hpfMode;
    bool phaseInv;
    // change gate: brief mute around tone/phase switches
    float gateGain;
    bool gateClosing;
};

class FilterFXEffect : public EffectProcessorImpl<FilterFXGroupState> {
  public:
    FilterFXEffect() = default;
    ~FilterFXEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            FilterFXGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pRateParameter;
    EngineEffectParameterPointer m_pDepthParameter;
    EngineEffectParameterPointer m_pResonanceParameter;
    EngineEffectParameterPointer m_pLfoAmpParameter;
    EngineEffectParameterPointer m_pToneParameter;
    EngineEffectParameterPointer m_pPhaseParameter;

    DISALLOW_COPY_AND_ASSIGN(FilterFXEffect);
};
