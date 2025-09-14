// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include <QtWidgets/QWidget>
#include <string>

#include "ui_gamelistsettingswidget.h"

class SettingsWindow;

class GameListSettingsWidget : public QWidget
{
public:
  GameListSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GameListSettingsWidget();

  bool addExcludedPath(const QString& path);
  void refreshExclusionList();

  void addSearchDirectory(QWidget* parent_widget);

private:
  void addPathToTable(const std::string& path, bool recursive);
  void refreshDirectoryList();
  void addSearchDirectory(const QString& path, bool recursive);
  void removeSearchDirectory(const QString& path);

  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onSearchDirectoriesSelectionChanged();
  void onAddExcludedFileButtonClicked();
  void onAddExcludedFolderButtonClicked();
  void onRemoveExcludedPathButtonClicked();
  void onExcludedPathsSelectionChanged();
  void onScanForNewGamesClicked();
  void onRescanAllGamesClicked();

  Ui::GameListSettingsWidget m_ui;
};
