// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include <QtCore/QAbstractTableModel>
#include <QtCore/QStringListModel>
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
  Qt::DropActions supportedDropActions() const override;
  QStringList mimeTypes() const override;
  bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                       const QModelIndex& parent) const override;
  bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                    const QModelIndex& parent) override;

  void reload();
  void addPath(std::string path, bool recursive);
  void removePath(const QModelIndex& index);
  void removePath(const std::string& path);
  const std::string& pathForIndex(const QModelIndex& index) const;

Q_SIGNALS:
  void settingsChanged();
  void pathsDropped(const QStringList& paths);

private:
  struct Row
  {
    std::string path;
    bool recursive;
  };

  void save();

  std::vector<Row> m_rows;
};

class GameListExcludedPathsModel final : public QStringListModel
{
  Q_OBJECT

public:
  explicit GameListExcludedPathsModel(QObject* parent = nullptr);
  ~GameListExcludedPathsModel() override;

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  Qt::DropActions supportedDropActions() const override;
  QStringList mimeTypes() const override;
  bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                       const QModelIndex& parent) const override;
  bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                    const QModelIndex& parent) override;

Q_SIGNALS:
  void pathsDropped(const QStringList& paths);
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

protected:
  bool eventFilter(QObject* object, QEvent* event) override;

private:
  void refreshDirectoryList();
  void addSearchDirectory(const QString& path, bool recursive);
  void promptAndAddSearchDirectory(const QString& path);

  void onDirectoryListSelectionChanged();
  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onAddExcludedFileButtonClicked();
  void onAddExcludedFolderButtonClicked();
  void onRemoveExcludedPathButtonClicked();
  void onExcludedPathsSelectionChanged();
  void onExcludedPathsContextMenuRequested(const QPoint& point);
  void onScanForNewGamesClicked();
  void onRescanAllGamesClicked();

  Ui::GameListSettingsWidget m_ui;
  GameListSearchDirectoriesModel* m_directory_model;
  GameListExcludedPathsModel* m_excluded_paths_model;
};
