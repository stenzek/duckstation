#pragma once
#include <string>
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
  QtHostInterface* m_host_interface;

  Ui::GameListSettingsWidget m_ui;

  GameListSearchDirectoriesModel* m_search_directories_model = nullptr;
};
