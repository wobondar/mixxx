#include <gtest/gtest.h>

#include <QDir>
#include <cmath>
#include <vector>

#include "sources/soundsourceproxy.h"
#include "stems/spleeterprocessor.h"
#include "stems/stemtrack.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"

// Compares the C++ separation against reference Spleeter output on a real
// excerpt. Needs MIXXX_LIVESTEMS_TESTDATA pointing at a directory holding
// ex60.wav, models/spleeter-4stems/*.onnx and reference/<stem>.wav separated
// from the same excerpt with the same models and bin count.
namespace {

const QStringList kStems = {QStringLiteral("drums"),
        QStringLiteral("bass"),
        QStringLiteral("other"),
        QStringLiteral("vocals")};

std::vector<float> readStereo(const QString& path, SINT* pNumFrames) {
    auto pTrack = Track::newTemporary(path);
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stereo());
    auto pSource = SoundSourceProxy(pTrack).openAudioSource(params);
    if (!pSource) {
        return {};
    }
    const SINT frames = pSource->frameIndexRange().length();
    std::vector<float> data(static_cast<size_t>(frames) * 2);
    pSource->readSampleFrames(mixxx::WritableSampleFrames(
            mixxx::IndexRange::forward(0, frames),
            mixxx::SampleBuffer::WritableSlice(data.data(), frames * 2)));
    *pNumFrames = frames;
    return data;
}

double snrDb(const std::vector<float>& reference, const std::vector<float>& got) {
    double signal = 0.0;
    double noise = 0.0;
    for (size_t i = 0; i < reference.size(); ++i) {
        signal += static_cast<double>(reference[i]) * reference[i];
        const double d = reference[i] - got[i];
        noise += d * d;
    }
    return 10.0 * std::log10(signal / std::max(noise, 1e-20));
}

class LiveStemsTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    void SetUp() override {
        const QByteArray dir = qgetenv("MIXXX_LIVESTEMS_TESTDATA");
        if (dir.isEmpty()) {
            GTEST_SKIP() << "MIXXX_LIVESTEMS_TESTDATA not set";
        }
        m_dir = QDir(QString::fromUtf8(dir));
        m_mix = readStereo(m_dir.filePath(QStringLiteral("ex60.wav")), &m_numFrames);
        ASSERT_FALSE(m_mix.empty());
        mixxx::SpleeterModelConfig config;
        config.directory = m_dir.filePath(QStringLiteral("models/spleeter-4stems"));
        config.bins = 1536;
        config.threads = 4;
        m_pProcessor = std::make_unique<mixxx::SpleeterProcessor>(config);
        ASSERT_TRUE(m_pProcessor->isValid()) << m_pProcessor->errorMessage().toStdString();
        for (int k = 0; k < 4; ++k) {
            m_fold.push_back(kStems.indexOf(m_pProcessor->stemNames().at(k)));
            ASSERT_GE(m_fold.back(), 0);
        }
    }

    void readMix(SINT frame, SINT numFrames, float* out) const {
        for (SINT i = 0; i < numFrames; ++i) {
            const SINT f = frame + i;
            const bool inside = f >= 0 && f < m_numFrames;
            out[i * 2] = inside ? m_mix[f * 2] : 0.0f;
            out[i * 2 + 1] = inside ? m_mix[f * 2 + 1] : 0.0f;
        }
    }

    std::vector<float> stemAsFloat(const mixxx::StemTrack& track, int stem) const {
        std::vector<float> out(static_cast<size_t>(m_numFrames) * 2);
        for (SINT f = 0; f < m_numFrames; ++f) {
            const int16_t* data = track.frameData(f);
            out[f * 2] = data[stem * 2] / 32767.0f;
            out[f * 2 + 1] = data[stem * 2 + 1] / 32767.0f;
        }
        return out;
    }

    QDir m_dir;
    SINT m_numFrames = 0;
    std::vector<float> m_mix;
    std::vector<int> m_fold;
    std::unique_ptr<mixxx::SpleeterProcessor> m_pProcessor;
};

TEST_F(LiveStemsTest, MatchesPythonReference) {
    mixxx::StemTrack track(m_numFrames, 4, 44100, false);
    mixxx::SpleeterProcessor::Run run;
    m_pProcessor->beginRun(&run);
    const auto reader = [this](SINT frame, SINT numFrames, float* out) {
        readMix(frame, numFrames, out);
    };
    for (SINT split = 0; split < track.numRegions(); ++split) {
        ASSERT_TRUE(m_pProcessor->processSplit(&run, split, reader, &track, m_fold, true));
    }
    EXPECT_EQ(track.doneRegionCount(), track.numRegions());
    for (int k = 0; k < 4; ++k) {
        SINT refFrames = 0;
        const auto reference = readStereo(
                m_dir.filePath(QStringLiteral("reference/%1.wav").arg(kStems[k])), &refFrames);
        ASSERT_EQ(refFrames, m_numFrames);
        const double snr = snrDb(reference, stemAsFloat(track, k));
        EXPECT_GT(snr, 60.0) << kStems[k].toStdString() << " SNR " << snr << " dB";
    }
}

TEST_F(LiveStemsTest, RestartMidTrackMatchesFullRun) {
    mixxx::StemTrack full(m_numFrames, 4, 44100, false);
    mixxx::StemTrack partial(m_numFrames, 4, 44100, false);
    const auto reader = [this](SINT frame, SINT numFrames, float* out) {
        readMix(frame, numFrames, out);
    };
    mixxx::SpleeterProcessor::Run run;
    m_pProcessor->beginRun(&run);
    for (SINT split = 0; split < full.numRegions(); ++split) {
        ASSERT_TRUE(m_pProcessor->processSplit(&run, split, reader, &full, m_fold, true));
    }
    ASSERT_GE(full.numRegions(), 3);
    // A run that starts at region 2 processes region 1 for context only.
    m_pProcessor->beginRun(&run);
    ASSERT_TRUE(m_pProcessor->processSplit(&run, 1, reader, &partial, m_fold, false));
    ASSERT_TRUE(m_pProcessor->processSplit(&run, 2, reader, &partial, m_fold, true));
    EXPECT_FALSE(partial.isRegionDone(1));
    EXPECT_TRUE(partial.isRegionDone(2));
    const SINT begin = 2 * mixxx::StemTrack::kRegionFrames - mixxx::StemTrack::kRegionOffset;
    const SINT end = std::min(begin + mixxx::StemTrack::kRegionFrames, m_numFrames);
    for (SINT f = begin; f < end; ++f) {
        for (int i = 0; i < 8; ++i) {
            ASSERT_EQ(full.frameData(f)[i], partial.frameData(f)[i]) << "frame " << f;
        }
    }
}

// The visual bytes need no model
void fillSine(mixxx::StemTrack* pTrack, int stem, SINT numFrames, double radPerFrame) {
    for (SINT f = 0; f < numFrames; ++f) {
        int16_t* data = pTrack->frameData(f);
        for (int i = 0; i < pTrack->numStems() * 2; ++i) {
            data[i] = 0;
        }
        const auto v = static_cast<int16_t>(std::lround(32767.0 * std::sin(f * radPerFrame)));
        data[stem * 2] = v;
        data[stem * 2 + 1] = v;
    }
}

TEST(StemTrackVisualTest, PeakBytesFollowRegionFlags) {
    const SINT numFrames = mixxx::StemTrack::kRegionFrames + 5000;
    mixxx::StemTrack track(numFrames, 4, 44100, false);
    EXPECT_DOUBLE_EQ(track.visualRatio(), 100.0);
    EXPECT_EQ(track.numVisualFrames(), numFrames / 100 + 1);
    // Half a period fits in one stride, so every stride holds a peak sample
    fillSine(&track, 2, numFrames, 0.05);
    track.markRegionDone(0);
    const SINT lastDoneFrame = mixxx::StemTrack::regionFirstFrame(1) - 1;
    const auto* early = track.visualData(track.visualFrameOf(1000));
    EXPECT_EQ(early[2 * 2].load(), 255);
    EXPECT_EQ(early[2 * 2 + 1].load(), 255);
    EXPECT_EQ(early[0].load(), 0);
    EXPECT_EQ(early[3 * 2].load(), 0);
    EXPECT_TRUE(track.isFrameDone(lastDoneFrame));
    EXPECT_FALSE(track.isFrameDone(lastDoneFrame + 1));
    const auto* late = track.visualData(track.visualFrameOf(lastDoneFrame + 2000));
    EXPECT_EQ(late[2 * 2].load(), 0);
    track.markRegionDone(1);
    EXPECT_EQ(late[2 * 2].load(), 255);
}

TEST(StemTrackVisualTest, RmsBytesMatchPeakForASine) {
    const SINT numFrames = 20000;
    mixxx::StemTrack track(numFrames, 4, 48000, true);
    // Many periods per stride so the stride's mean square sits at one half
    fillSine(&track, 0, numFrames, 0.5);
    track.markRegionDone(0);
    const auto* bytes = track.visualData(track.visualFrameOf(10000));
    EXPECT_NEAR(bytes[0].load(), 255, 4);
    EXPECT_EQ(bytes[2].load(), 0);
}

} // namespace
