#pragma once

#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

class TremoloV2State : public EffectState {
  public:
    TremoloV2State(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters),
              gain(0),
              currentFrame(0),
              quantizeEnabled(false),
              tripletEnabled(false){};
    ~TremoloV2State() override = default;

    double gain;
    unsigned int currentFrame;
    bool quantizeEnabled;
    bool tripletEnabled;
};

class TremoloV2Effect : public EffectProcessorImpl<TremoloV2State> {
  public:
    TremoloV2Effect() = default;
    ~TremoloV2Effect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            TremoloV2State* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pDepthParameter;
    EngineEffectParameterPointer m_pRateParameter;
    EngineEffectParameterPointer m_pWidthParameter;
    EngineEffectParameterPointer m_pWaveformParameter;
    EngineEffectParameterPointer m_pPhaseParameter;
    EngineEffectParameterPointer m_pQuantizeParameter;
    EngineEffectParameterPointer m_pTripletParameter;

    DISALLOW_COPY_AND_ASSIGN(TremoloV2Effect);
};
