// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include <QtCore/QAbstractTableModel>
#include <QtWidgets/QWidget>
#include <string>
#include <vector>

#include "ui_gamelistsettingswidget.h"

class SettingsWindow;

class GameListSearchDirectoriesModel final : public QAbstractTableModel
{
  Q_OBJECT

public:
  explicit GameListSearchDirectoriesModel(QObject* parent = nullptr);
  ~GameListSearchDirectoriesModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;

  void reload();
  void addPath(std::string path, bool recursive);
  void removePath(const QModelIndex& index);
  void removePath(const std::string& path);
  const std::string& pathForIndex(const QModelIndex& index) const;

Q_SIGNALS:
  void settingsChanged();

private:
  struct Row
  {
    std::string path;
    bool recursive;
  };

  void save();

  std::vector<Row> m_rows;
};

class GameListSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GameListSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GameListSettingsWidget();

  bool addExcludedPath(const QString& path);
  void refreshExclusionList();

  void addSearchDirectory(QWidget* parent_widget);

private:
  void refreshDirectoryList();
  void addSearchDirectory(const QString& path, bool recursive);

  void onDirectoryListSelectionChanged();
  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onAddExcludedFileButtonClicked();
  void onAddExcludedFolderButtonClicked();
  void onRemoveExcludedPathButtonClicked();
  void onExcludedPathsSelectionChanged();
  void onScanForNewGamesClicked();
  void onRescanAllGamesClicked();

  Ui::GameListSettingsWidget m_ui;
  GameListSearchDirectoriesModel* m_directory_model;
};
