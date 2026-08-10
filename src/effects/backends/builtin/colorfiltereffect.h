#pragma once

#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class ColorFilterGroupState : public EffectState {
  public:
    struct Coef {
        double b0, b1, b2, a1, a2;
    };

    ColorFilterGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        clear();
    }
    ~ColorFilterGroupState() override = default;

    void clear() {
        for (int f = 0; f < 2; f++) {
            for (int c = 0; c < 2; c++) {
                s1[f][c] = 0.0;
                s2[f][c] = 0.0;
            }
        }
    }

    // filter index 0 = LPF, 1 = HPF; transposed DF-II state per channel
    double s1[2][2];
    double s2[2][2];
    // 8-bit control codes, slewed one step at a time on a fixed clock
    int colorCode = 128;
    int paramCode = 128;
    double framesUntilStep = 0.0;
    double coefRate = 0.0; // sample rate the coefficients were built for
    // mix gains, ramped per sample (a de-click, not a fade)
    float lpfGain = 0.0f;
    float dryGain = 1.0f;
    float hpfGain = 0.0f;
    Coef lpf{};
    Coef hpf{};
};

class ColorFilterEffect : public EffectProcessorImpl<ColorFilterGroupState> {
  public:
    ColorFilterEffect() = default;
    ~ColorFilterEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            ColorFilterGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pColorParameter;
    EngineEffectParameterPointer m_pParamParameter;

    DISALLOW_COPY_AND_ASSIGN(ColorFilterEffect);
};
