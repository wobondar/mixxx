#pragma once

#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class PhaserFXGroupState : public EffectState {
  public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxStages = 60;

    PhaserFXGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters),
              currentFrame(0),
              warpRate(0),
              bottomWarped(0),
              peakWarped(0),
              stages{6, 6},
              phaseInv(false),
              prevFb(0.0f),
              prevDry(1.0f),
              prevWet(0.0f),
              gateGain(1.0f),
              gateClosing(false) {
        clearCore();
    }
    ~PhaserFXGroupState() override = default;

    void clearCore() {
        for (int c = 0; c < kMaxChannels; c++) {
            for (int s = 0; s < kMaxStages; s++) {
                apState[c][s] = 0.0f;
            }
            lastOut[c] = 0.0f;
        }
    }

    double currentFrame;
    double warpRate; // sample rate the endpoints were prewarped for
    double bottomWarped;
    double peakWarped;
    // one-multiplier lattice all-pass memory, one word per stage
    float apState[kMaxChannels][kMaxStages];
    float lastOut[kMaxChannels];
    // active (post-gate) per-channel stage counts and LFO inversion
    int stages[kMaxChannels];
    bool phaseInv;
    // per-buffer ramp anchors
    float prevFb;
    float prevDry;
    float prevWet;
    // change gate: brief mute around stage/phase switches
    float gateGain;
    bool gateClosing;
};

class PhaserFXEffect : public EffectProcessorImpl<PhaserFXGroupState> {
  public:
    PhaserFXEffect() = default;
    ~PhaserFXEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            PhaserFXGroupState* pState,
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
    EngineEffectParameterPointer m_pFeedbackParameter;
    EngineEffectParameterPointer m_pStagesParameter;
    EngineEffectParameterPointer m_pToneParameter;
    EngineEffectParameterPointer m_pPhaseParameter;

    DISALLOW_COPY_AND_ASSIGN(PhaserFXEffect);
};
