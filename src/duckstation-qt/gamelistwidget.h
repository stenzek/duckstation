#pragma once
#include "core/game_list.h"
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTableView>

class GameListModel;

class QtHostInterface;

class GameListWidget : public QStackedWidget
{
  Q_OBJECT

public:
  GameListWidget(QWidget* parent = nullptr);
  ~GameListWidget();

  void initialize(QtHostInterface* host_interface);

Q_SIGNALS:
  void bootEntryRequested(const GameList::GameListEntry& entry);

private Q_SLOTS:
  void onGameListRefreshed();
  void onTableViewItemDoubleClicked(const QModelIndex& index);

protected:
  void resizeEvent(QResizeEvent* event);

private:
  QtHostInterface* m_host_interface = nullptr;
  GameList* m_game_list = nullptr;

  GameListModel* m_table_model = nullptr;
  QTableView* m_table_view = nullptr;
};
