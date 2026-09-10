#include "stems/spleeterprocessor.h"

#include <onnxruntime_cxx_api.h>

#include <QDir>
#include <algorithm>
#include <array>
#include <cmath>

#include "dsp/transforms/FFT.h"
#include "util/logger.h"

namespace mixxx {

namespace {

const Logger kLogger("SpleeterProcessor");

constexpr int kNumBins = SpleeterProcessor::kFftSize / 2 + 1;
constexpr SINT kRegion = StemTrack::kRegionFrames;
constexpr SINT kChunkSamples = kRegion + SpleeterProcessor::kFftSize - SpleeterProcessor::kHop;
constexpr SINT kAccumSamples = kRegion + SpleeterProcessor::kFftSize;
constexpr float kMaskEpsilon = 1e-10f;
constexpr double kWindowCompensation = 2.0 / 3.0;
constexpr const char* kInputName = "x";
constexpr const char* kOutputName = "y";

Ort::Env& ortEnv() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mixxx-stems");
    return env;
}

std::string metadataValue(const Ort::ModelMetadata& metadata,
        Ort::AllocatorWithDefaultOptions& allocator,
        const char* key) {
    auto value = metadata.LookupCustomMetadataMapAllocated(key, allocator);
    return value ? std::string(value.get()) : std::string();
}

} // namespace

SpleeterProcessor::SpleeterProcessor(const SpleeterModelConfig& config)
        : m_bins(config.bins) {
    if (m_bins <= 0 || m_bins > kNumBins - 1 || m_bins % 64 != 0) {
        m_error = QStringLiteral("unsupported bin count %1").arg(m_bins);
        return;
    }
    if (!loadSessions(config)) {
        return;
    }
    m_fft = std::make_unique<FFTReal>(kFftSize);
    m_window.resize(kFftSize);
    for (int n = 0; n < kFftSize; ++n) {
        m_window[n] = 0.5 - 0.5 * std::cos(2.0 * M_PI * n / kFftSize);
    }
    for (auto& spec : m_specReal) {
        spec.resize(static_cast<size_t>(kSplitFrames) * kNumBins);
    }
    for (auto& spec : m_specImag) {
        spec.resize(static_cast<size_t>(kSplitFrames) * kNumBins);
    }
    m_input.resize(static_cast<size_t>(2) * kSplitFrames * m_bins);
    m_output.assign(m_sessions.size(), std::vector<float>(m_input.size()));
    m_frameTime.resize(kFftSize);
    m_frameReal.resize(kNumBins);
    m_frameImag.resize(kNumBins);
    m_fullReal.resize(kFftSize);
    m_fullImag.resize(kFftSize);
    m_mix.resize(static_cast<size_t>(kChunkSamples) * 2);
    m_valid = true;
}

SpleeterProcessor::~SpleeterProcessor() = default;

bool SpleeterProcessor::loadSessions(const SpleeterModelConfig& config) {
    QDir dir(config.directory);
    const QStringList files = dir.entryList({QStringLiteral("*.onnx")}, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        m_error = QStringLiteral("no ONNX models in %1").arg(config.directory);
        return false;
    }
    std::vector<std::pair<int, QString>> order;
    std::vector<std::unique_ptr<Ort::Session>> sessions(files.size());
    QStringList names(files.size());
    Ort::AllocatorWithDefaultOptions allocator;
    try {
        for (const QString& file : files) {
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads(std::max(1, config.threads));
            options.SetInterOpNumThreads(1);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            const QByteArray path = dir.absoluteFilePath(file).toUtf8();
            auto session = std::make_unique<Ort::Session>(ortEnv(), path.constData(), options);
            const Ort::ModelMetadata metadata = session->GetModelMetadata();
            const std::string stem = metadataValue(metadata, allocator, "stem");
            const std::string index = metadataValue(metadata, allocator, "stem_index");
            if (stem.empty() || index.empty()) {
                m_error = QStringLiteral("%1 lacks stem metadata").arg(file);
                return false;
            }
            const int stemIndex = std::stoi(index);
            if (stemIndex < 0 || stemIndex >= static_cast<int>(files.size()) ||
                    sessions[stemIndex]) {
                m_error = QStringLiteral("%1 has an invalid stem index").arg(file);
                return false;
            }
            sessions[stemIndex] = std::move(session);
            names[stemIndex] = QString::fromStdString(stem);
        }
    } catch (const Ort::Exception& e) {
        m_error = QString::fromUtf8(e.what());
        return false;
    }
    m_sessions = std::move(sessions);
    m_stemNames = names;
    return true;
}

void SpleeterProcessor::beginRun(Run* run) const {
    run->accum.assign(m_sessions.size() * 2, std::vector<double>(kAccumSamples, 0.0));
    run->nextSplit = -1;
}

bool SpleeterProcessor::processSplit(Run* run,
        SINT split,
        const StemAudioReader& reader,
        StemTrack* track,
        const std::vector<int>& fold,
        bool finalize) {
    if (!m_valid || !track || !run) {
        return false;
    }
    if (run->accum.size() != m_sessions.size() * 2) {
        beginRun(run);
    }
    // Padded sample p of the STFT input is mix frame p - kFftSize.
    reader(split * kRegion - kFftSize, kChunkSamples, m_mix.data());
    computeSpectra(m_mix.data(), kChunkSamples);
    if (!runModels()) {
        return false;
    }
    overlapAdd(run);
    if (finalize) {
        flushRegion(*run, split, track, fold);
    }
    // Slide the accumulator so the tail of this region's last frames lands
    // at the start of the next region.
    for (auto& accum : run->accum) {
        std::copy(accum.begin() + kRegion, accum.end(), accum.begin());
        std::fill(accum.begin() + (kAccumSamples - kRegion), accum.end(), 0.0);
    }
    run->nextSplit = split + 1;
    if (finalize) {
        track->markRegionDone(split);
    }
    return true;
}

void SpleeterProcessor::computeSpectra(const float* stereo, SINT numSamples) {
    Q_UNUSED(numSamples);
    for (int ch = 0; ch < 2; ++ch) {
        for (int j = 0; j < kSplitFrames; ++j) {
            const float* start = stereo + static_cast<size_t>(j) * kHop * 2 + ch;
            for (int n = 0; n < kFftSize; ++n) {
                m_frameTime[n] = start[static_cast<size_t>(n) * 2] * m_window[n];
            }
            // qm-dsp writes the full mirrored spectrum; only the first
            // kNumBins are kept
            m_fft->forward(m_frameTime.data(), m_fullReal.data(), m_fullImag.data());
            double* real = m_specReal[ch].data() + static_cast<size_t>(j) * kNumBins;
            double* imag = m_specImag[ch].data() + static_cast<size_t>(j) * kNumBins;
            std::copy(m_fullReal.begin(), m_fullReal.begin() + kNumBins, real);
            std::copy(m_fullImag.begin(), m_fullImag.begin() + kNumBins, imag);
            float* input = m_input.data() + (static_cast<size_t>(ch) * kSplitFrames + j) * m_bins;
            for (int b = 0; b < m_bins; ++b) {
                input[b] = static_cast<float>(std::sqrt(real[b] * real[b] + imag[b] * imag[b]));
            }
        }
    }
}

bool SpleeterProcessor::runModels() {
    const std::array<int64_t, 4> shape = {2, 1, kSplitFrames, m_bins};
    try {
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(
                memory, m_input.data(), m_input.size(), shape.data(), shape.size());
        for (size_t k = 0; k < m_sessions.size(); ++k) {
            auto outputs = m_sessions[k]->Run(
                    Ort::RunOptions{nullptr}, &kInputName, &input, 1, &kOutputName, 1);
            const float* data = outputs[0].GetTensorData<float>();
            std::copy(data, data + m_output[k].size(), m_output[k].begin());
        }
    } catch (const Ort::Exception& e) {
        kLogger.warning() << "inference failed:" << e.what();
        return false;
    }
    return true;
}

void SpleeterProcessor::overlapAdd(Run* run) {
    const size_t numStems = m_sessions.size();
    const float epsilonShare = kMaskEpsilon / static_cast<float>(numStems);
    std::vector<float> masks(numStems * m_bins);
    for (int ch = 0; ch < 2; ++ch) {
        for (int j = 0; j < kSplitFrames; ++j) {
            const size_t frameOffset = (static_cast<size_t>(ch) * kSplitFrames + j) * m_bins;
            const double* real = m_specReal[ch].data() + static_cast<size_t>(j) * kNumBins;
            const double* imag = m_specImag[ch].data() + static_cast<size_t>(j) * kNumBins;
            for (int b = 0; b < m_bins; ++b) {
                float sum = kMaskEpsilon;
                for (size_t s = 0; s < numStems; ++s) {
                    const float magnitude = m_output[s][frameOffset + b];
                    masks[s * m_bins + b] = magnitude * magnitude;
                    sum += masks[s * m_bins + b];
                }
                for (size_t s = 0; s < numStems; ++s) {
                    masks[s * m_bins + b] = (masks[s * m_bins + b] + epsilonShare) / sum;
                }
            }
            for (size_t k = 0; k < numStems; ++k) {
                const float* mask = masks.data() + k * m_bins;
                for (int b = 0; b < m_bins; ++b) {
                    m_frameReal[b] = real[b] * mask[b];
                    m_frameImag[b] = imag[b] * mask[b];
                }
                std::fill(m_frameReal.begin() + m_bins, m_frameReal.end(), 0.0);
                std::fill(m_frameImag.begin() + m_bins, m_frameImag.end(), 0.0);
                m_fft->inverse(m_frameReal.data(), m_frameImag.data(), m_frameTime.data());
                double* accum = run->accum[k * 2 + ch].data() + static_cast<size_t>(j) * kHop;
                for (int n = 0; n < kFftSize; ++n) {
                    accum[n] += m_frameTime[n] * m_window[n];
                }
            }
        }
    }
}

void SpleeterProcessor::flushRegion(
        const Run& run, SINT split, StemTrack* track, const std::vector<int>& fold) {
    const size_t numStems = m_sessions.size();
    const int outStems = track->numStems();
    std::vector<double> mixed(static_cast<size_t>(outStems) * 2);
    const SINT firstFrame = split * kRegion - kFftSize;
    const SINT begin = std::max<SINT>(0, -firstFrame);
    const SINT end = std::min<SINT>(kRegion, track->numFrames() - firstFrame);
    for (SINT p = begin; p < end; ++p) {
        std::fill(mixed.begin(), mixed.end(), 0.0);
        for (size_t k = 0; k < numStems; ++k) {
            const int target = fold[k];
            mixed[target * 2] += run.accum[k * 2][p];
            mixed[target * 2 + 1] += run.accum[k * 2 + 1][p];
        }
        int16_t* out = track->frameData(firstFrame + p);
        for (int i = 0; i < outStems * 2; ++i) {
            const double value = std::clamp(mixed[i] * kWindowCompensation, -1.0, 1.0);
            out[i] = static_cast<int16_t>(std::lrint(value * 32767.0));
        }
    }
}

} // namespace mixxx
