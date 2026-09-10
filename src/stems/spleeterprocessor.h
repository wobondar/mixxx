#pragma once

#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

#include "stems/stemtrack.h"
#include "util/types.h"

class FFTReal;

namespace Ort {
class Env;
class Session;
} // namespace Ort

namespace mixxx {

struct SpleeterModelConfig {
    /// Directory holding one ONNX U-Net per stem with "stem" and
    /// "stem_index" metadata keys.
    QString directory;
    /// Spectrogram bins fed to the network: 1024 (11 kHz), 1536 (16 kHz)
    /// or 2048 (22 kHz).
    int bins = 1536;
    int threads = 1;
};

/// Fills `out` with `numFrames` interleaved stereo frames starting at `frame`
/// of the mix, zero outside the track.
using StemAudioReader = std::function<void(SINT frame, SINT numFrames, float* out)>;

/// Spleeter inference on ONNX Runtime: periodic Hann STFT of kFftSize with
/// hop kHop on a signal padded with one leading frame of zeros, one U-Net per
/// stem on splits of kSplitFrames spectrogram frames, squared-magnitude ratio
/// masks, zeros above the bin count, inverse by windowed overlap-add.
///
/// Splits are processed in order within a run. Processing split s finalizes
/// region s of the StemTrack, provided split s-1 was processed earlier in the
/// same run (or does not exist); a run that starts mid-track therefore
/// processes one context split first without marking it done.
class SpleeterProcessor {
  public:
    static constexpr int kFftSize = 4096;
    static constexpr int kHop = 1024;
    static constexpr int kSplitFrames = 512;

    explicit SpleeterProcessor(const SpleeterModelConfig& config);
    ~SpleeterProcessor();

    bool isValid() const {
        return m_valid;
    }
    QString errorMessage() const {
        return m_error;
    }
    /// Stem names in model index order.
    QStringList stemNames() const {
        return m_stemNames;
    }
    int numModelStems() const {
        return static_cast<int>(m_sessions.size());
    }
    int bins() const {
        return m_bins;
    }

    /// Overlap-add state of one run of consecutive splits. Owned by the
    /// caller so several tracks can share one processor's model sessions.
    struct Run {
        // Per stem, per channel accumulator covering one region plus the
        // tail of its last frame
        std::vector<std::vector<double>> accum;
        SINT nextSplit = -1;
    };

    /// Discards overlap-add state; the next processed split starts the run.
    void beginRun(Run* run) const;

    /// Runs the models on one split and overlap-adds the result. When
    /// `finalize` is set the region is written to `track` (model stems folded
    /// through `fold`, which maps model stem index to track stem index) and
    /// marked done. Returns false on inference failure.
    bool processSplit(Run* run,
            SINT split,
            const StemAudioReader& reader,
            StemTrack* track,
            const std::vector<int>& fold,
            bool finalize);

    static SINT numSplits(SINT numFrames) {
        return StemTrack::regionOf(numFrames - 1) + 1;
    }

  private:
    bool loadSessions(const SpleeterModelConfig& config);
    void computeSpectra(const float* stereo, SINT numSamples);
    bool runModels();
    void overlapAdd(Run* run);
    void flushRegion(const Run& run, SINT split, StemTrack* track, const std::vector<int>& fold);

    bool m_valid = false;
    QString m_error;
    int m_bins;
    QStringList m_stemNames;
    std::vector<std::unique_ptr<Ort::Session>> m_sessions;
    std::unique_ptr<FFTReal> m_fft;
    std::vector<double> m_window;

    // Per channel, per frame: complex spectrum of the mix (kFftSize/2+1 bins)
    std::vector<double> m_specReal[2];
    std::vector<double> m_specImag[2];
    // Model input and per-stem output, laid out (channel, 1, frame, bin)
    std::vector<float> m_input;
    std::vector<std::vector<float>> m_output;
    std::vector<double> m_frameTime;
    std::vector<double> m_frameReal;
    std::vector<double> m_frameImag;
    std::vector<double> m_fullReal;
    std::vector<double> m_fullImag;
    std::vector<float> m_mix;
};

} // namespace mixxx
