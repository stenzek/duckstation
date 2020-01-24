#pragma once
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTableView>

class GameList;
struct GameListEntry;

class GameListModel;
class GameListSortModel;

class QtHostInterface;

class GameListWidget : public QStackedWidget
{
  Q_OBJECT

public:
  GameListWidget(QWidget* parent = nullptr);
  ~GameListWidget();

  void initialize(QtHostInterface* host_interface);

Q_SIGNALS:
  void entrySelected(const GameListEntry* entry);
  void bootEntryRequested(const GameListEntry* entry);

private Q_SLOTS:
  void onGameListRefreshed();
  void onTableViewItemDoubleClicked(const QModelIndex& index);
  void onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous);

protected:
  void resizeEvent(QResizeEvent* event);

private:
  QtHostInterface* m_host_interface = nullptr;
  GameList* m_game_list = nullptr;

  GameListModel* m_table_model = nullptr;
  GameListSortModel* m_table_sort_model = nullptr;
  QTableView* m_table_view = nullptr;
};
