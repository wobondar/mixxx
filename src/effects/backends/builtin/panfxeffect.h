#pragma once

#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class PanFXGroupState : public EffectState {
  public:
    static constexpr int kMaxChannels = 2;
    // Per channel: 2 biquads LOW chain, 4 MID chain, 2 HIGH chain.
    static constexpr int kBiquadsPerChannel = 8;

    PanFXGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters),
              currentFrame(0),
              tableRate(0),
              gain{1.0, 1.0},
              prevDry(1.0f),
              prevWet(0.0f) {
        for (int b = 0; b < 3; b++) {
            bandGain[b] = 1.0f;
        }
        clearFilters();
    }
    ~PanFXGroupState() override = default;

    void clearFilters() {
        for (int c = 0; c < kMaxChannels; c++) {
            for (int b = 0; b < kBiquadsPerChannel; b++) {
                z1[c][b] = 0.0f;
                z2[c][b] = 0.0f;
            }
        }
    }

    double currentFrame;
    double tableRate; // sample rate the biquad coefficients were built for
    // Normalized biquad coefficients for the two crossover corners:
    // [0] = LP low corner, [1] = HP low corner,
    // [2] = LP high corner, [3] = HP high corner.
    float b0[4], b1[4], b2[4], a1[4], a2[4];
    // Direct-form-II-transposed memory per channel per biquad.
    float z1[kMaxChannels][kBiquadsPerChannel];
    float z2[kMaxChannels][kBiquadsPerChannel];
    // Slewed per-channel VCA gain.
    double gain[kMaxChannels];
    // Slewed per-band effect-entry gains (0 = band bypasses the pan).
    float bandGain[3];
    float prevDry;
    float prevWet;
};

class PanFXEffect : public EffectProcessorImpl<PanFXGroupState> {
  public:
    PanFXEffect() = default;
    ~PanFXEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            PanFXGroupState* pState,
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
    EngineEffectParameterPointer m_pLfoAmpParameter;
    EngineEffectParameterPointer m_pLfoParameter;
    EngineEffectParameterPointer m_pLowCutParameter;
    EngineEffectParameterPointer m_pHiCutParameter;

    DISALLOW_COPY_AND_ASSIGN(PanFXEffect);
};
