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
  void entryDoubleClicked(const GameListEntry* entry);
  void entryContextMenuRequested(const QPoint& point, const GameListEntry* entry);

private Q_SLOTS:
  void onGameListRefreshed();
  void onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous);
  void onTableViewItemDoubleClicked(const QModelIndex& index);
  void onTableViewContextMenuRequested(const QPoint& point);
  void onTableViewHeaderContextMenuRequested(const QPoint& point);

protected:
  void resizeEvent(QResizeEvent* event);

private:
  const GameListEntry* getSelectedEntry() const;
  void resizeTableViewColumnsToFit();

  QtHostInterface* m_host_interface = nullptr;
  GameList* m_game_list = nullptr;

  GameListModel* m_table_model = nullptr;
  GameListSortModel* m_table_sort_model = nullptr;
  QTableView* m_table_view = nullptr;
};
