#pragma once

#include <cmath>
#include <limits>

#include "analyzer/analyzer.h"
#include "library/dao/analysisdao.h"
#include "util/performancetimer.h"
#include "util/sample.h"
#include "waveform/waveform.h"

//NOTS vrince some test to segment sound, to apply color in the waveform
//#define TEST_HEAT_MAP
#ifdef TEST_HEAT_MAP
class QImage;
#endif

class EngineFilterIIRBase;
class QSqlDatabase;

// How each visual stride condenses its audio samples into one byte per band.
enum class WaveformEnvelope {
    Peak = 0, // loudest absolute sample in the stride
    Rms = 1,  // root mean square of the stride
};

struct WaveformStride {
    WaveformStride(double samples,
            double averageSamples,
            int stemCount,
            WaveformEnvelope envelope)
            : m_position(0),
              m_stemCount(stemCount),
              m_length(samples),
              m_averageLength(averageSamples),
              m_averagePosition(0),
              m_averageDivisor(0),
              m_strideSamples(0),
              m_envelope(envelope),
              m_postScaleConversion(static_cast<float>(
                      std::numeric_limits<unsigned char>::max())) {
        reset();
    }

    inline void reset() {
        m_position = 0;
        m_averageDivisor = 0;
        m_strideSamples = 0;
        for (int i = 0; i < ChannelCount; ++i) {
            m_overallData[i] = 0.0f;
            m_averageOverallData[i] = 0.0f;
            SampleUtil::clear(m_filteredData[i], BandCount);
            SampleUtil::clear(m_averageFilteredData[i], BandCount);
            SampleUtil::clear(m_stemData[i], m_stemCount);
        }
    }

    // Folds one absolute sample into an accumulator: keeps the max for
    // Peak, sums squares for Rms.
    inline void accumulate(float* pDest, float source) const {
        if (m_envelope == WaveformEnvelope::Rms) {
            *pDest += source * source;
        } else if (source > *pDest) {
            *pDest = source;
        }
    }

    // Turns an accumulator into the stride's amplitude. Rms is scaled so a
    // full-scale sine lands on the same byte as its peak would.
    inline float finalize(float accumulated) const {
        if (m_envelope == WaveformEnvelope::Rms) {
            if (m_strideSamples <= 0) {
                return 0.0f;
            }
            constexpr float kSineRmsToPeak = 1.41421356f;
            return std::sqrt(accumulated / m_strideSamples) * kSineRmsToPeak;
        }
        return accumulated;
    }

    inline unsigned char toByte(float amplitude) const {
        return static_cast<unsigned char>(std::min(255.0,
                m_postScaleConversion * amplitude + 0.5));
    }

    inline void store(WaveformData* data) {
        for (int i = 0; i < ChannelCount; ++i) {
            WaveformData& datum = *(data + i);
            const float all = finalize(m_overallData[i]);
            const float low = finalize(m_filteredData[i][Low]);
            const float mid = finalize(m_filteredData[i][Mid]);
            const float high = finalize(m_filteredData[i][High]);
            datum.filtered.all = toByte(all);
            datum.filtered.low = toByte(low);
            datum.filtered.mid = toByte(mid);
            datum.filtered.high = toByte(high);
            for (int stemIdx = 0; stemIdx < m_stemCount; stemIdx++) {
                datum.stems[stemIdx] = toByte(finalize(m_stemData[i][stemIdx]));
            }
            // The summary averages finalized stride amplitudes.
            m_averageOverallData[i] += all;
            m_averageFilteredData[i][Low] += low;
            m_averageFilteredData[i][Mid] += mid;
            m_averageFilteredData[i][High] += high;
        }
        m_averageDivisor++;
        m_strideSamples = 0;
        // Reset the stride counters
        for (int i = 0; i < ChannelCount; ++i) {
            m_overallData[i] = 0.0f;
            for (int f = 0; f < BandCount; ++f) {
                m_filteredData[i][f] = 0.0f;
            }
            for (int stemIdx = 0; stemIdx < m_stemCount; ++stemIdx) {
                m_stemData[i][stemIdx] = 0.0f;
            }
        }
    }

    inline void averageStore(WaveformData* data) {
        if (m_averageDivisor) {
            for (int i = 0; i < ChannelCount; ++i) {
                WaveformData& datum = *(data + i);
                datum.filtered.all = toByte(m_averageOverallData[i] / m_averageDivisor);
                datum.filtered.low = toByte(m_averageFilteredData[i][Low] / m_averageDivisor);
                datum.filtered.mid = toByte(m_averageFilteredData[i][Mid] / m_averageDivisor);
                datum.filtered.high = toByte(m_averageFilteredData[i][High] / m_averageDivisor);
            }
        } else {
            // This is the case if The Overview Waveform has more samples than the detailed waveform
            for (int i = 0; i < ChannelCount; ++i) {
                WaveformData& datum = *(data + i);
                datum.filtered.all = toByte(finalize(m_overallData[i]));
                datum.filtered.low = toByte(finalize(m_filteredData[i][Low]));
                datum.filtered.mid = toByte(finalize(m_filteredData[i][Mid]));
                datum.filtered.high = toByte(finalize(m_filteredData[i][High]));
            }
        }

        m_averageDivisor = 0;
        for (int i = 0; i < ChannelCount; ++i) {
            m_averageOverallData[i] = 0.0f;
            for (int f = 0; f < BandCount; ++f) {
                m_averageFilteredData[i][f] = 0.0f;
            }
        }
    }

    int m_position;
    int m_stemCount;
    double m_length;
    double m_averageLength;
    int m_averagePosition;
    int m_averageDivisor;
    int m_strideSamples;
    WaveformEnvelope m_envelope;

    float m_overallData[ChannelCount];
    float m_filteredData[ChannelCount][BandCount];
    float m_stemData[ChannelCount][mixxx::kMaxSupportedStems];

    float m_averageOverallData[ChannelCount];
    float m_averageFilteredData[ChannelCount][BandCount];

    float m_postScaleConversion;
};

class AnalyzerWaveform : public Analyzer {
  public:
    AnalyzerWaveform(
            UserSettingsPointer pConfig,
            const QSqlDatabase& dbConnection);
    ~AnalyzerWaveform() override;

    bool initialize(const AnalyzerTrack& track,
            mixxx::audio::SampleRate sampleRate,
            mixxx::audio::ChannelCount channelCount,
            SINT frameLength) override;
    bool processSamples(const CSAMPLE* buffer, SINT count) override;
    void storeResults(TrackPointer tio) override;
    void cleanup() override;

  private:
    bool shouldAnalyze(TrackPointer tio) const;

    void storeCurrentStridePower();
    void resetCurrentStride();

    void createFilters(mixxx::audio::SampleRate sampleRate);
    void destroyFilters();

    UserSettingsPointer m_pConfig;
    mutable AnalysisDao m_analysisDao;

    WaveformPointer m_waveform;
    WaveformPointer m_waveformSummary;
    WaveformData* m_waveformData;
    WaveformData* m_waveformSummaryData;

    WaveformStride m_stride;

    int m_currentStride;
    int m_currentSummaryStride;
    mixxx::audio::ChannelCount m_channelCount;

    struct Filters {
        std::unique_ptr<EngineFilterIIRBase> low;
        std::unique_ptr<EngineFilterIIRBase> mid;
        std::unique_ptr<EngineFilterIIRBase> high;
    };

    Filters m_filters;

    struct Buffers {
        std::vector<float> low;
        std::vector<float> mid;
        std::vector<float> high;

        SINT size;

        Buffers()
                : low(),
                  mid(),
                  high(),
                  size(0) {
        }
    };

    Buffers m_buffers;

    PerformanceTimer m_timer;

#ifdef TEST_HEAT_MAP
    QImage* test_heatMap;
#endif
};
