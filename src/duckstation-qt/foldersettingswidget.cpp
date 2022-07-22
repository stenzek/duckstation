#include <QtWidgets/QMessageBox>
#include <algorithm>

#include "foldersettingswidget.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

FolderSettingsWidget::FolderSettingsWidget(SettingsDialog* dialog, QWidget* parent) : QWidget(parent)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.cache, m_ui.cacheBrowse, m_ui.cacheOpen, m_ui.cacheReset,
                                                 "Folders", "Cache", Path::Combine(EmuFolders::DataRoot, "cache"));
  SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.covers, m_ui.coversBrowse, m_ui.coversOpen, m_ui.coversReset,
                                                 "Folders", "Covers", Path::Combine(EmuFolders::DataRoot, "covers"));
  SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.screenshots, m_ui.screenshotsBrowse, m_ui.screenshotsOpen,
                                                 m_ui.screenshotsReset, "Folders", "Screenshots",
                                                 Path::Combine(EmuFolders::DataRoot, "screenshots"));
  SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.saveStates, m_ui.saveStatesBrowse, m_ui.saveStatesOpen,
                                                 m_ui.saveStatesReset, "Folders", "SaveStates",
                                                 Path::Combine(EmuFolders::DataRoot, "savestates"));
}

FolderSettingsWidget::~FolderSettingsWidget() = default;
