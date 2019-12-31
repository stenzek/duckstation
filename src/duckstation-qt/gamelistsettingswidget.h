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

private Q_SLOTS:
  void onAddSearchDirectoryButtonPressed();
  void onRemoveSearchDirectoryButtonPressed();
  void onRefreshGameListButtonPressed();
  void onBrowseRedumpPathButtonPressed();
  void onDownloadRedumpDatabaseButtonPressed();

protected:
  void resizeEvent(QResizeEvent* event);

private:
  QtHostInterface* m_host_interface;

  Ui::GameListSettingsWidget m_ui;

  GameListSearchDirectoriesModel* m_search_directories_model = nullptr;
};
