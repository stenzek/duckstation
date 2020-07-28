#pragma once

#include <QtWidgets/QWidget>

#include "ui_gamelistsettingswidget.h"

class QtHostInterface;

class GameListSearchDirectoriesModel;

class GameListSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GameListSettingsWidget(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~GameListSettingsWidget();

public Q_SLOTS:
  void addSearchDirectory(QWidget* parent_widget);

private Q_SLOTS:
  void onDirectoryListItemClicked(const QModelIndex& index);
  void onDirectoryListContextMenuRequested(const QPoint& point);
  void onAddSearchDirectoryButtonClicked();
  void onRemoveSearchDirectoryButtonClicked();
  void onScanForNewGamesClicked();
  void onRescanAllGamesClicked();
  void onUpdateRedumpDatabaseButtonClicked();

protected:
  void resizeEvent(QResizeEvent* event);

private:
  bool downloadRedumpDatabase(const QString& download_path);

  QtHostInterface* m_host_interface;

  Ui::GameListSettingsWidget m_ui;

  GameListSearchDirectoriesModel* m_search_directories_model = nullptr;
};
