// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include <QtWidgets/QWidget>
#include <string>

#include "ui_gamelistsettingswidget.h"

class SettingsWindow;

class GameListSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GameListSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GameListSettingsWidget();

  bool addExcludedPath(const std::string& path);
  void refreshExclusionList();

public Q_SLOTS:
  void addSearchDirectory(QWidget* parent_widget);

private Q_SLOTS:
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

protected:
  bool event(QEvent* event) override;

private:
  void addPathToTable(const std::string& path, bool recursive);
  void refreshDirectoryList();
  void addSearchDirectory(const QString& path, bool recursive);
  void removeSearchDirectory(const QString& path);

  Ui::GameListSettingsWidget m_ui;
};
