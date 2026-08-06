#pragma once

#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class SweepGroupState : public EffectState {
  public:
    SweepGroupState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        clear();
        parallel = false;
    }
    ~SweepGroupState() override = default;

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
    bool parallel;
};

class SweepEffect : public EffectProcessorImpl<SweepGroupState> {
  public:
    SweepEffect() = default;
    ~SweepEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            SweepGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pSweepParameter;
    EngineEffectParameterPointer m_pCenterParameter;

    DISALLOW_COPY_AND_ASSIGN(SweepEffect);
};
