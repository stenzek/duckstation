#pragma once
#include <string>
#include <QtWidgets/QWidget>

#include "ui_gamelistsettingswidget.h"

class SettingsDialog;
class GameListSearchDirectoriesModel;

class GameListSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GameListSettingsWidget(SettingsDialog* dialog, QWidget* parent);
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
