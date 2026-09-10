#include "stems/stemestimator.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <chrono>
#include <cstring>

#include "mixer/playermanager.h"
#include "moc_stemestimator.cpp"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "util/defs.h"
#include "util/logger.h"

namespace mixxx {

namespace {

const Logger kLogger("StemEstimator");

struct CacheHeader {
    char magic[8];
    uint32_t version;
    uint32_t numStems;
    int64_t numFrames;
};
constexpr char kCacheMagic[8] = {'M', 'I', 'X', 'X', 'S', 'T', 'E', 'M'};
constexpr uint32_t kCacheVersion = 1;

QString hashFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (!hash.addData(&file)) {
        return QString();
    }
    return QString::fromLatin1(hash.result().toHex());
}

StemPresentation presentationForMode(int mode) {
    StemPresentation p;
    switch (mode) {
    case 2:
        p.numStems = 2;
        p.names = {QStringLiteral("Instrumental"), QStringLiteral("Vocals")};
        p.colors = {QColor("#CC79A7"), QColor("#56B4E9")};
        p.fold = {{QStringLiteral("accompaniment"), 0}, {QStringLiteral("vocals"), 1}};
        p.modelSubdir = QStringLiteral("spleeter-2stems");
        break;
    case 3:
        p.numStems = 3;
        p.names = {QStringLiteral("Drums"), QStringLiteral("Music"), QStringLiteral("Vocals")};
        p.colors = {QColor("#009E73"), QColor("#CC79A7"), QColor("#56B4E9")};
        p.fold = {{QStringLiteral("drums"), 0},
                {QStringLiteral("bass"), 1},
                {QStringLiteral("other"), 1},
                {QStringLiteral("vocals"), 2}};
        p.modelSubdir = QStringLiteral("spleeter-4stems");
        break;
    case 4:
        p.numStems = 4;
        p.names = {QStringLiteral("Drums"),
                QStringLiteral("Bass"),
                QStringLiteral("Other"),
                QStringLiteral("Vocals")};
        p.colors = {QColor("#009E73"), QColor("#D55E00"), QColor("#CC79A7"), QColor("#56B4E9")};
        p.fold = {{QStringLiteral("drums"), 0},
                {QStringLiteral("bass"), 1},
                {QStringLiteral("other"), 2},
                {QStringLiteral("vocals"), 3}};
        p.modelSubdir = QStringLiteral("spleeter-4stems");
        break;
    default:
        break;
    }
    return p;
}

} // namespace

StemEstimator* StemEstimator::s_pInstance = nullptr;

// static
void StemEstimator::initialize(UserSettingsPointer pConfig) {
    if (s_pInstance) {
        return;
    }
    const int mode = pConfig->getValue(
            ConfigKey(stemconfig::kGroup, "mode"), stemconfig::kDefaultMode);
    if (mode == 0) {
        return;
    }
    s_pInstance = new StemEstimator(pConfig);
    if (!s_pInstance->isEnabled()) {
        kLogger.warning() << "disabled:" << s_pInstance->m_pProcessor->errorMessage();
        delete s_pInstance;
        s_pInstance = nullptr;
    }
}

// static
void StemEstimator::shutdown() {
    delete s_pInstance;
    s_pInstance = nullptr;
}

bool StemEstimator::isDeckEnabled(const QString& group) const {
    for (int i = 0; i < kMaxNumberOfDecks; ++i) {
        if (PlayerManager::groupForDeck(i) == group) {
            return m_pConfig->getValue(
                    ConfigKey(stemconfig::kGroup, QStringLiteral("deck%1").arg(i + 1)), false);
        }
    }
    return false;
}

StemEstimator::StemEstimator(UserSettingsPointer pConfig)
        : m_pConfig(pConfig),
          m_stop(false) {
    const int mode = pConfig->getValue(
            ConfigKey(stemconfig::kGroup, "mode"), stemconfig::kDefaultMode);
    m_presentation = presentationForMode(mode);
    if (m_presentation.numStems == 0) {
        return;
    }
    SpleeterModelConfig config;
    config.bins = pConfig->getValue(
            ConfigKey(stemconfig::kGroup, "bins"), stemconfig::kDefaultBins);
    config.threads = pConfig->getValue(
            ConfigKey(stemconfig::kGroup, "threads"), stemconfig::kDefaultThreads);
    config.directory = QDir(stemconfig::modelDirectory(pConfig))
                               .filePath(m_presentation.modelSubdir);
    m_bins = config.bins;
    m_pProcessor = std::make_unique<SpleeterProcessor>(config);
    if (!m_pProcessor->isValid()) {
        return;
    }
    for (const QString& name : m_pProcessor->stemNames()) {
        if (m_presentation.fold.find(name) == m_presentation.fold.end()) {
            kLogger.warning() << "model stem" << name << "has no place in mode" << mode;
            m_pProcessor.reset();
            return;
        }
    }
    kLogger.info() << "mode" << mode << "bins" << config.bins << "threads" << config.threads
                   << "models" << config.directory;
    m_thread = std::thread([this] { workerLoop(); });
}

StemEstimator::~StemEstimator() {
    m_stop.store(true);
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

StemTrackPointer StemEstimator::requestTrack(
        const QString& group, TrackPointer pTrack, SINT numFrames) {
    if (!isEnabled() || !pTrack || numFrames <= 0) {
        return nullptr;
    }
    auto pJob = std::make_shared<Job>();
    pJob->pTrack = pTrack;
    pJob->pStemTrack = std::make_shared<StemTrack>(numFrames, m_presentation.numStems);
    for (const QString& name : m_pProcessor->stemNames()) {
        pJob->fold.push_back(m_presentation.fold.at(name));
    }
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_jobs.find(group);
        if (it != m_jobs.end()) {
            it->second->cancelled.store(true);
        }
        m_jobs[group] = pJob;
    }
    m_wake.notify_all();
    return pJob->pStemTrack;
}

void StemEstimator::releaseTrack(const QString& group) {
    const std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_jobs.find(group);
    if (it != m_jobs.end()) {
        it->second->cancelled.store(true);
        m_jobs.erase(it);
    }
}

void StemEstimator::workerLoop() {
    size_t next = 0;
    while (!m_stop.load()) {
        std::vector<std::shared_ptr<Job>> jobs;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (auto it = m_jobs.begin(); it != m_jobs.end();) {
                if (it->second->failed ||
                        it->second->pStemTrack->doneRegionCount() ==
                                it->second->pStemTrack->numRegions()) {
                    it = m_jobs.erase(it);
                } else {
                    jobs.push_back(it->second);
                    ++it;
                }
            }
            if (jobs.empty()) {
                m_wake.wait_for(lock, std::chrono::milliseconds(200));
                continue;
            }
        }
        // One split per deck per round keeps two loading decks fair.
        next = (next + 1) % jobs.size();
        Job* pJob = jobs[next].get();
        if (!pJob->cancelled.load()) {
            step(pJob);
        }
    }
}

bool StemEstimator::step(Job* pJob) {
    StemTrack* pTrack = pJob->pStemTrack.get();
    if (!pJob->cacheChecked) {
        pJob->cacheChecked = true;
        if (m_pConfig->getValue(
                    ConfigKey(stemconfig::kGroup, "cache"), stemconfig::kDefaultCache)) {
            pJob->cachePath = cachePathFor(*pJob);
            if (loadFromCache(pJob)) {
                return true;
            }
        }
    }
    if (!pJob->wall.isValid()) {
        pJob->wall.start();
    }
    QElapsedTimer stepTimer;
    stepTimer.start();
    const SINT numSplits = pTrack->numRegions();
    const SINT wanted = StemTrack::regionOf(
            std::clamp<SINT>(pTrack->wantedFrame(), 0, pTrack->numFrames() - 1));
    SINT split = pJob->run.nextSplit;
    const bool followPlayhead = !pTrack->isRegionDone(wanted) &&
            (split < wanted - 1 || split > wanted);
    if (split < 0 || followPlayhead || split >= numSplits || pTrack->isRegionDone(split)) {
        // Start (or restart) a run at the first undone region from the
        // playhead, wrapping to the head of the track.
        SINT target = pTrack->firstUndoneRegionFrom(followPlayhead ? wanted : std::max<SINT>(split, 0));
        if (target >= numSplits) {
            target = pTrack->firstUndoneRegionFrom(0);
        }
        if (target >= numSplits) {
            return false;
        }
        if (!pJob->pAudioSource && !openSource(pJob)) {
            pJob->failed = true;
            return false;
        }
        m_pProcessor->beginRun(&pJob->run);
        const auto reader = [this, pJob](SINT frame, SINT numFrames, float* out) {
            readMix(pJob, frame, numFrames, out);
        };
        if (target > 0) {
            // The previous split's frames overlap into this region; run it
            // for context without publishing it.
            if (!m_pProcessor->processSplit(
                        &pJob->run, target - 1, reader, pTrack, pJob->fold, false)) {
                pJob->failed = true;
                return false;
            }
        }
        split = target;
    }
    if (!pJob->pAudioSource && !openSource(pJob)) {
        pJob->failed = true;
        return false;
    }
    const auto reader = [this, pJob](SINT frame, SINT numFrames, float* out) {
        readMix(pJob, frame, numFrames, out);
    };
    if (!m_pProcessor->processSplit(&pJob->run, split, reader, pTrack, pJob->fold, true)) {
        pJob->failed = true;
        return false;
    }
    pJob->busyMs += stepTimer.elapsed();
    if (pTrack->doneRegionCount() == numSplits && !pJob->cancelled.load()) {
        const double seconds = static_cast<double>(pTrack->numFrames()) /
                pJob->pAudioSource->getSignalInfo().getSampleRate().toDouble();
        kLogger.info() << "separated" << pJob->pTrack->getLocation() << "in"
                       << pJob->wall.elapsed() / 1000.0 << "s wall,"
                       << pJob->busyMs / 1000.0 << "s busy,"
                       << seconds / (pJob->busyMs / 1000.0) << "x realtime";
        saveToCache(*pJob);
    }
    return true;
}

QString StemEstimator::cachePathFor(const Job& job) const {
    const QString hash = hashFile(job.pTrack->getLocation());
    if (hash.isEmpty()) {
        return QString();
    }
    return QDir(stemconfig::cacheDirectory(m_pConfig))
            .filePath(QStringLiteral("%1-%2-%3-%4%5")
                            .arg(hash,
                                    m_presentation.modelSubdir,
                                    QString::number(m_bins),
                                    QString::number(m_presentation.numStems),
                                    QLatin1String(stemconfig::kCacheSuffix)));
}

bool StemEstimator::loadFromCache(Job* pJob) {
    if (pJob->cachePath.isEmpty()) {
        return false;
    }
    QFile file(pJob->cachePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    StemTrack* pTrack = pJob->pStemTrack.get();
    CacheHeader header;
    if (file.read(reinterpret_cast<char*>(&header), sizeof(header)) != sizeof(header) ||
            std::memcmp(header.magic, kCacheMagic, sizeof(kCacheMagic)) != 0 ||
            header.version != kCacheVersion ||
            header.numStems != static_cast<uint32_t>(pTrack->numStems()) ||
            header.numFrames != pTrack->numFrames()) {
        kLogger.warning() << "ignoring unusable cache file" << pJob->cachePath;
        return false;
    }
    const qint64 bytes = static_cast<qint64>(pTrack->numFrames()) * pTrack->numStems() * 2 *
            sizeof(int16_t);
    if (file.read(reinterpret_cast<char*>(pTrack->frameData(0)), bytes) != bytes) {
        kLogger.warning() << "short cache file" << pJob->cachePath;
        return false;
    }
    for (SINT region = 0; region < pTrack->numRegions(); ++region) {
        pTrack->markRegionDone(region);
    }
    // Last use decides eviction order
    file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    kLogger.debug() << "cache hit" << pJob->pTrack->getLocation();
    return true;
}

void StemEstimator::saveToCache(const Job& job) {
    if (job.cachePath.isEmpty()) {
        return;
    }
    const QString cacheDir = stemconfig::cacheDirectory(m_pConfig);
    if (!QDir().mkpath(cacheDir)) {
        kLogger.warning() << "cannot create" << cacheDir;
        return;
    }
    const StemTrack* pTrack = job.pStemTrack.get();
    CacheHeader header;
    std::memcpy(header.magic, kCacheMagic, sizeof(kCacheMagic));
    header.version = kCacheVersion;
    header.numStems = static_cast<uint32_t>(pTrack->numStems());
    header.numFrames = pTrack->numFrames();
    const qint64 bytes = static_cast<qint64>(pTrack->numFrames()) * pTrack->numStems() * 2 *
            sizeof(int16_t);
    QSaveFile file(job.cachePath);
    if (!file.open(QIODevice::WriteOnly) ||
            file.write(reinterpret_cast<const char*>(&header), sizeof(header)) !=
                    sizeof(header) ||
            file.write(reinterpret_cast<const char*>(pTrack->frameData(0)), bytes) != bytes ||
            !file.commit()) {
        kLogger.warning() << "cannot write" << job.cachePath << file.errorString();
        return;
    }
    evictCache(job.cachePath);
}

void StemEstimator::evictCache(const QString& keep) {
    const qint64 maxBytes =
            static_cast<qint64>(m_pConfig->getValue(
                    ConfigKey(stemconfig::kGroup, "cache_max_mb"),
                    stemconfig::kDefaultCacheMaxMb)) *
            1024 * 1024;
    const QDir dir(stemconfig::cacheDirectory(m_pConfig));
    // Newest first, so eviction pops from the back
    const QFileInfoList files = dir.entryInfoList(
            {QStringLiteral("*") + QLatin1String(stemconfig::kCacheSuffix)},
            QDir::Files,
            QDir::Time);
    qint64 total = 0;
    for (const QFileInfo& info : files) {
        total += info.size();
    }
    for (auto it = files.rbegin(); it != files.rend() && total > maxBytes; ++it) {
        if (it->absoluteFilePath() == keep) {
            continue;
        }
        if (QFile::remove(it->absoluteFilePath())) {
            total -= it->size();
            kLogger.debug() << "evicted" << it->fileName();
        }
    }
}

bool StemEstimator::openSource(Job* pJob) {
    AudioSource::OpenParams params;
    params.setChannelCount(audio::ChannelCount::stereo());
    pJob->pAudioSource = SoundSourceProxy(pJob->pTrack).openAudioSource(params);
    if (!pJob->pAudioSource) {
        kLogger.warning() << "cannot open" << pJob->pTrack->getLocation();
        return false;
    }
    if (pJob->pAudioSource->getSignalInfo().getChannelCount() != audio::ChannelCount::stereo()) {
        kLogger.warning() << "not stereo, skipping" << pJob->pTrack->getLocation();
        pJob->pAudioSource.reset();
        return false;
    }
    return true;
}

void StemEstimator::readMix(Job* pJob, SINT frame, SINT numFrames, float* out) {
    std::fill(out, out + numFrames * 2, 0.0f);
    const SINT total = pJob->pAudioSource->frameIndexRange().end();
    const SINT begin = std::max<SINT>(frame, 0);
    const SINT end = std::min<SINT>(frame + numFrames, total);
    if (end <= begin) {
        return;
    }
    const SINT count = end - begin;
    float* target = out + (begin - frame) * 2;
    const auto readable = pJob->pAudioSource->readSampleFrames(WritableSampleFrames(
            IndexRange::forward(begin, count), SampleBuffer::WritableSlice(target, count * 2)));
    if (readable.frameIndexRange().length() != count) {
        kLogger.debug() << "short read at" << begin << "got" << readable.frameIndexRange().length();
    }
}

} // namespace mixxx
