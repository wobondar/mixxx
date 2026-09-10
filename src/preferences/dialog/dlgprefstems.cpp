#include "preferences/dialog/dlgprefstems.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>

#include "moc_dlgprefstems.cpp"

using mixxx::stemconfig::kGroup;

namespace {

// Combo box index to config value
constexpr int kModes[] = {0, 2, 3, 4};
constexpr int kBins[] = {1536, 2048};

template<size_t N>
int indexOf(const int (&values)[N], int value) {
    for (size_t i = 0; i < N; ++i) {
        if (values[i] == value) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

QFileInfoList cacheFiles(const QString& dir) {
    return QDir(dir).entryInfoList(
            {QStringLiteral("*") + QLatin1String(mixxx::stemconfig::kCacheSuffix)},
            QDir::Files);
}

} // namespace

DlgPrefStems::DlgPrefStems(QWidget* pParent, UserSettingsPointer pConfig)
        : DlgPreferencePage(pParent),
          m_pConfig(pConfig) {
    setupUi(this);
    connect(browseModelDirButton, &QPushButton::clicked, this, &DlgPrefStems::slotBrowseModelDir);
    connect(clearCacheButton, &QPushButton::clicked, this, &DlgPrefStems::slotClearCache);
    connect(cacheCheckBox, &QCheckBox::toggled, cacheSizeSpinBox, &QSpinBox::setEnabled);
    setScrollSafeGuardForAllInputWidgets(this);
}

void DlgPrefStems::slotUpdate() {
    using namespace mixxx::stemconfig;
    modeComboBox->setCurrentIndex(
            indexOf(kModes, m_pConfig->getValue(ConfigKey(kGroup, "mode"), kDefaultMode)));
    bandwidthComboBox->setCurrentIndex(
            indexOf(kBins, m_pConfig->getValue(ConfigKey(kGroup, "bins"), kDefaultBins)));
    for (int i = 0; i < kMaxNumberOfDecks; ++i) {
        deckCheckBoxes()[i]->setChecked(m_pConfig->getValue(
                ConfigKey(kGroup, QStringLiteral("deck%1").arg(i + 1)), false));
    }
    threadsSpinBox->setValue(m_pConfig->getValue(ConfigKey(kGroup, "threads"), kDefaultThreads));
    modelDirLineEdit->setText(modelDirectory(m_pConfig));
    cacheCheckBox->setChecked(m_pConfig->getValue(ConfigKey(kGroup, "cache"), kDefaultCache));
    cacheSizeSpinBox->setValue(
            m_pConfig->getValue(ConfigKey(kGroup, "cache_max_mb"), kDefaultCacheMaxMb) / 1024);
    updateCacheUsage();
}

void DlgPrefStems::slotApply() {
    m_pConfig->setValue(ConfigKey(kGroup, "mode"), kModes[modeComboBox->currentIndex()]);
    m_pConfig->setValue(ConfigKey(kGroup, "bins"), kBins[bandwidthComboBox->currentIndex()]);
    for (int i = 0; i < kMaxNumberOfDecks; ++i) {
        m_pConfig->setValue(ConfigKey(kGroup, QStringLiteral("deck%1").arg(i + 1)),
                deckCheckBoxes()[i]->isChecked());
    }
    m_pConfig->setValue(ConfigKey(kGroup, "threads"), threadsSpinBox->value());
    // The default follows the settings directory; only a chosen path is stored
    if (modelDirLineEdit->text() == mixxx::stemconfig::defaultModelDirectory(m_pConfig)) {
        m_pConfig->remove(ConfigKey(kGroup, "model_dir"));
    } else {
        m_pConfig->setValue(ConfigKey(kGroup, "model_dir"), modelDirLineEdit->text());
    }
    m_pConfig->setValue(ConfigKey(kGroup, "cache"), cacheCheckBox->isChecked());
    m_pConfig->setValue(ConfigKey(kGroup, "cache_max_mb"), cacheSizeSpinBox->value() * 1024);
}

void DlgPrefStems::slotResetToDefaults() {
    using namespace mixxx::stemconfig;
    modeComboBox->setCurrentIndex(indexOf(kModes, kDefaultMode));
    bandwidthComboBox->setCurrentIndex(indexOf(kBins, kDefaultBins));
    for (QCheckBox* pCheckBox : deckCheckBoxes()) {
        pCheckBox->setChecked(false);
    }
    threadsSpinBox->setValue(kDefaultThreads);
    modelDirLineEdit->setText(defaultModelDirectory(m_pConfig));
    cacheCheckBox->setChecked(kDefaultCache);
    cacheSizeSpinBox->setValue(kDefaultCacheMaxMb / 1024);
}

void DlgPrefStems::slotBrowseModelDir() {
    const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Choose the stem model directory"), modelDirLineEdit->text());
    if (!dir.isEmpty()) {
        modelDirLineEdit->setText(dir);
    }
}

void DlgPrefStems::slotClearCache() {
    const auto files = cacheFiles(mixxx::stemconfig::cacheDirectory(m_pConfig));
    for (const QFileInfo& info : files) {
        QFile::remove(info.absoluteFilePath());
    }
    updateCacheUsage();
}

std::array<QCheckBox*, kMaxNumberOfDecks> DlgPrefStems::deckCheckBoxes() {
    return {deck1CheckBox, deck2CheckBox, deck3CheckBox, deck4CheckBox};
}

void DlgPrefStems::updateCacheUsage() {
    const auto files = cacheFiles(mixxx::stemconfig::cacheDirectory(m_pConfig));
    qint64 total = 0;
    for (const QFileInfo& info : files) {
        total += info.size();
    }
    cacheUsageLabel->setText(tr("%1 tracks, %2 GB")
                    .arg(files.size())
                    .arg(static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1));
    clearCacheButton->setEnabled(!files.isEmpty());
}
