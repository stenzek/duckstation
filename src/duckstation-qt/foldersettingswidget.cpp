// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include <QtWidgets/QMessageBox>
#include <algorithm>

#include "foldersettingswidget.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

FolderSettingsWidget::FolderSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.cache, m_ui.cacheBrowse, tr("Select Cache Directory"),
                                                 m_ui.cacheOpen, m_ui.cacheReset, "Folders", "Cache",
                                                 Path::Combine(EmuFolders::DataRoot, "cache"));
  SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.covers, m_ui.coversBrowse, tr("Select Covers Directory"),
                                                 m_ui.coversOpen, m_ui.coversReset, "Folders", "Covers",
                                                 Path::Combine(EmuFolders::DataRoot, "covers"));
  SettingWidgetBinder::BindWidgetToFolderSetting(
    sif, m_ui.saveStates, m_ui.saveStatesBrowse, tr("Select Save States Directory"), m_ui.saveStatesOpen,
    m_ui.saveStatesReset, "Folders", "SaveStates", Path::Combine(EmuFolders::DataRoot, "savestates"));
  SettingWidgetBinder::BindWidgetToFolderSetting(
    sif, m_ui.screenshots, m_ui.screenshotsBrowse, tr("Select Screenshots Directory"), m_ui.screenshotsOpen,
    m_ui.screenshotsReset, "Folders", "Screenshots", Path::Combine(EmuFolders::DataRoot, "screenshots"));
  SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.videos, m_ui.videosBrowse, tr("Select Videos Directory"),
                                                 m_ui.videosOpen, m_ui.videosReset, "Folders", "Videos",
                                                 Path::Combine(EmuFolders::DataRoot, "videos"));
}

FolderSettingsWidget::~FolderSettingsWidget() = default;
