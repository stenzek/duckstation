// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "foldersettingswidget.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include <algorithm>

#include "moc_foldersettingswidget.cpp"

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
}

FolderSettingsWidget::~FolderSettingsWidget() = default;
