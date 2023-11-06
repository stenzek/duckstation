// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include <string>
#include <QtWidgets/QWidget>

#include "ui_gamelistsettingswidget.h"

class SettingsWindow;
class GameListSearchDirectoriesModel;

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
  void onDirectoryListItemClicked(const QModelIndex& index);
  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onAddExcludedPathButtonClicked();
  void onRemoveExcludedPathButtonClicked();
  void onScanForNewGamesClicked();
  void onRescanAllGamesClicked();

protected:
  void resizeEvent(QResizeEvent* event);

private:
  Ui::GameListSettingsWidget m_ui;

  GameListSearchDirectoriesModel* m_search_directories_model = nullptr;
};
