#include "gamelistwidget.h"
#include "common/string_util.h"
#include "core/game_list.h"
#include "core/settings.h"
#include "gamelistmodel.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QPixmap>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>

class GameListSortModel final : public QSortFilterProxyModel
{
public:
  GameListSortModel(GameListModel* parent) : QSortFilterProxyModel(parent), m_model(parent) {}

  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override
  {
    // TODO: Search
    return QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent);
  }

  bool lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const override
  {
    const bool ascending = sortOrder() == Qt::AscendingOrder;
    return m_model->lessThan(source_left, source_right, source_left.column(), ascending);
  }

private:
  GameListModel* m_model;
};

GameListWidget::GameListWidget(QWidget* parent /* = nullptr */) : QStackedWidget(parent) {}

GameListWidget::~GameListWidget() = default;

void GameListWidget::initialize(QtHostInterface* host_interface)
{
  m_host_interface = host_interface;
  m_game_list = host_interface->getGameList();

  connect(m_host_interface, &QtHostInterface::gameListRefreshed, this, &GameListWidget::onGameListRefreshed);

  m_table_model = new GameListModel(m_game_list, this);
  m_table_sort_model = new GameListSortModel(m_table_model);
  m_table_sort_model->setSourceModel(m_table_model);
  m_table_view = new QTableView(this);
  m_table_view->setModel(m_table_sort_model);
  m_table_view->setSortingEnabled(true);
  m_table_view->setSelectionMode(QAbstractItemView::SingleSelection);
  m_table_view->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_table_view->setAlternatingRowColors(true);
  m_table_view->setShowGrid(false);
  m_table_view->setCurrentIndex({});
  m_table_view->horizontalHeader()->setHighlightSections(false);
  m_table_view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
  m_table_view->verticalHeader()->hide();
  m_table_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

  loadTableViewColumnVisibilitySettings();
  loadTableViewColumnSortSettings();

  connect(m_table_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);
  connect(m_table_view, &QTableView::doubleClicked, this, &GameListWidget::onTableViewItemDoubleClicked);
  connect(m_table_view, &QTableView::customContextMenuRequested, this,
          &GameListWidget::onTableViewContextMenuRequested);
  connect(m_table_view->horizontalHeader(), &QHeaderView::customContextMenuRequested, this,
          &GameListWidget::onTableViewHeaderContextMenuRequested);
  connect(m_table_view->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
          &GameListWidget::onTableViewHeaderSortIndicatorChanged);

  insertWidget(0, m_table_view);
  setCurrentIndex(0);

  resizeTableViewColumnsToFit();
}

void GameListWidget::onGameListRefreshed()
{
  m_table_model->refresh();
}

void GameListWidget::onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
  const QModelIndex source_index = m_table_sort_model->mapToSource(current);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
  {
    emit entrySelected(nullptr);
    return;
  }

  const GameListEntry& entry = m_game_list->GetEntries().at(source_index.row());
  emit entrySelected(&entry);
}

void GameListWidget::onTableViewItemDoubleClicked(const QModelIndex& index)
{
  const QModelIndex source_index = m_table_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
    return;

  const GameListEntry& entry = m_game_list->GetEntries().at(source_index.row());
  emit entryDoubleClicked(&entry);
}

void GameListWidget::onTableViewContextMenuRequested(const QPoint& point)
{
  const GameListEntry* entry = getSelectedEntry();
  if (!entry)
    return;

  emit entryContextMenuRequested(m_table_view->mapToGlobal(point), entry);
}

void GameListWidget::onTableViewHeaderContextMenuRequested(const QPoint& point)
{
  QMenu menu;

  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    QAction* action = menu.addAction(m_table_model->getColumnDisplayName(column));
    action->setCheckable(true);
    action->setChecked(!m_table_view->isColumnHidden(column));
    connect(action, &QAction::toggled, [this, column](bool enabled) {
      m_table_view->setColumnHidden(column, !enabled);
      saveTableViewColumnVisibilitySettings(column);
      resizeTableViewColumnsToFit();
    });
  }

  menu.exec(m_table_view->mapToGlobal(point));
}

void GameListWidget::onTableViewHeaderSortIndicatorChanged(int, Qt::SortOrder)
{
  saveTableViewColumnSortSettings();
}

void GameListWidget::resizeEvent(QResizeEvent* event)
{
  QStackedWidget::resizeEvent(event);
  resizeTableViewColumnsToFit();
}

void GameListWidget::resizeTableViewColumnsToFit()
{
  QtUtils::ResizeColumnsForTableView(m_table_view, {32, 80, -1, -1, 100, 60, 100});
}

static TinyString getColumnVisibilitySettingsKeyName(int column)
{
  return TinyString::FromFormat("Show%s", GameListModel::getColumnName(static_cast<GameListModel::Column>(column)));
}

void GameListWidget::loadTableViewColumnVisibilitySettings()
{
  static constexpr std::array<bool, GameListModel::Column_Count> DEFAULT_VISIBILITY = {
    {true, true, true, false, true, true, true}};

  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    const bool visible = m_host_interface->GetBoolSettingValue(
      "GameListTableView", getColumnVisibilitySettingsKeyName(column), DEFAULT_VISIBILITY[column]);
    m_table_view->setColumnHidden(column, !visible);
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings()
{
  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    const bool visible = !m_table_view->isColumnHidden(column);
    m_host_interface->SetBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column), visible);
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings(int column)
{
  const bool visible = !m_table_view->isColumnHidden(column);
  m_host_interface->SetBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column), visible);
}

void GameListWidget::loadTableViewColumnSortSettings()
{
  const GameListModel::Column DEFAULT_SORT_COLUMN = GameListModel::Column_Type;
  const bool DEFAULT_SORT_DESCENDING = false;

  const GameListModel::Column sort_column =
    GameListModel::getColumnIdForName(m_host_interface->GetStringSettingValue("GameListTableView", "SortColumn"))
      .value_or(DEFAULT_SORT_COLUMN);
  const bool sort_descending =
    m_host_interface->GetBoolSettingValue("GameListTableView", "SortDescending", DEFAULT_SORT_DESCENDING);
  m_table_sort_model->sort(sort_column, sort_descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void GameListWidget::saveTableViewColumnSortSettings()
{
  const int sort_column = m_table_view->horizontalHeader()->sortIndicatorSection();
  const bool sort_descending = (m_table_view->horizontalHeader()->sortIndicatorOrder() == Qt::DescendingOrder);

  if (sort_column >= 0 && sort_column < GameListModel::Column_Count)
  {
    m_host_interface->SetStringSettingValue(
      "GameListTableView", "SortColumn", GameListModel::getColumnName(static_cast<GameListModel::Column>(sort_column)));
  }

  m_host_interface->SetBoolSettingValue("GameListTableView", "SortDescending", sort_descending);
}

const GameListEntry* GameListWidget::getSelectedEntry() const
{
  const QItemSelectionModel* selection_model = m_table_view->selectionModel();
  if (!selection_model->hasSelection())
    return nullptr;

  const QModelIndexList selected_rows = selection_model->selectedRows();
  if (selected_rows.empty())
    return nullptr;

  const QModelIndex source_index = m_table_sort_model->mapToSource(selected_rows[0]);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
    return nullptr;

  return &m_game_list->GetEntries().at(source_index.row());
}
