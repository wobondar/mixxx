#pragma once

#include <array>

#include "preferences/dialog/dlgpreferencepage.h"
#include "preferences/dialog/ui_dlgprefstemsdlg.h"
#include "preferences/usersettings.h"
#include "stems/stemestimator.h"
#include "util/defs.h"

class QWidget;

/// Live stem separation settings. Which settings need a restart is decided
/// by the estimator; the page only states it.
class DlgPrefStems : public DlgPreferencePage, public Ui::DlgPrefStemsDlg {
    Q_OBJECT
  public:
    DlgPrefStems(QWidget* pParent, UserSettingsPointer pConfig);

  public slots:
    void slotUpdate() override;
    void slotApply() override;
    void slotResetToDefaults() override;

  private slots:
    void slotBrowseModelDir();
    void slotClearCache();

  private:
    std::array<QCheckBox*, kMaxNumberOfDecks> deckCheckBoxes();
    void updateCacheUsage();

    UserSettingsPointer m_pConfig;
};
