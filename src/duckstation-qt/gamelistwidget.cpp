#include "gamelistwidget.h"
#include "common/string_util.h"
#include "core/settings.h"
#include "frontend-common/game_list.h"
#include "gamelistmodel.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QPixmap>
#include <QtGui/QWheelEvent>
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
    return m_model->lessThan(source_left, source_right, source_left.column());
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

  m_model = new GameListModel(m_game_list, this);
  m_model->setCoverScale(host_interface->GetFloatSettingValue("UI", "GameListCoverArtScale", 0.45f));
  m_model->setShowCoverTitles(host_interface->GetBoolSettingValue("UI", "GameListShowCoverTitles", true));

  m_sort_model = new GameListSortModel(m_model);
  m_sort_model->setSourceModel(m_model);
  m_table_view = new QTableView(this);
  m_table_view->setModel(m_sort_model);
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
  connect(m_table_view, &QTableView::activated, this, &GameListWidget::onTableViewItemActivated);
  connect(m_table_view, &QTableView::customContextMenuRequested, this,
          &GameListWidget::onTableViewContextMenuRequested);
  connect(m_table_view->horizontalHeader(), &QHeaderView::customContextMenuRequested, this,
          &GameListWidget::onTableViewHeaderContextMenuRequested);
  connect(m_table_view->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
          &GameListWidget::onTableViewHeaderSortIndicatorChanged);

  insertWidget(0, m_table_view);

  m_list_view = new GameListGridListView(this);
  m_list_view->setModel(m_sort_model);
  m_list_view->setModelColumn(GameListModel::Column_Cover);
  m_list_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_list_view->setViewMode(QListView::IconMode);
  m_list_view->setResizeMode(QListView::Adjust);
  m_list_view->setUniformItemSizes(true);
  m_list_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_list_view->setFrameStyle(QFrame::NoFrame);
  m_list_view->setSpacing(m_model->getCoverArtSpacing());
  updateListFont();

  connect(m_list_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);
  connect(m_list_view, &GameListGridListView::zoomIn, this, &GameListWidget::gridZoomIn);
  connect(m_list_view, &GameListGridListView::zoomOut, this, &GameListWidget::gridZoomOut);
  connect(m_list_view, &QListView::activated, this, &GameListWidget::onListViewItemActivated);
  connect(m_list_view, &QListView::customContextMenuRequested, this, &GameListWidget::onListViewContextMenuRequested);

  insertWidget(1, m_list_view);

  if (m_host_interface->GetBoolSettingValue("UI", "GameListGridView", false))
    setCurrentIndex(1);
  else
    setCurrentIndex(0);

  resizeTableViewColumnsToFit();
}

bool GameListWidget::isShowingGameList() const
{
  return currentIndex() == 0;
}

bool GameListWidget::isShowingGameGrid() const
{
  return currentIndex() == 1;
}

bool GameListWidget::getShowGridCoverTitles() const
{
  return m_model->getShowCoverTitles();
}

void GameListWidget::onGameListRefreshed()
{
  m_model->refresh();
}

void GameListWidget::onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
  const QModelIndex source_index = m_sort_model->mapToSource(current);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
  {
    emit entrySelected(nullptr);
    return;
  }

  const GameListEntry& entry = m_game_list->GetEntries().at(source_index.row());
  emit entrySelected(&entry);
}

void GameListWidget::onTableViewItemActivated(const QModelIndex& index)
{
  const QModelIndex source_index = m_sort_model->mapToSource(index);
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

void GameListWidget::onListViewItemActivated(const QModelIndex& index)
{
  const QModelIndex source_index = m_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
    return;

  const GameListEntry& entry = m_game_list->GetEntries().at(source_index.row());
  emit entryDoubleClicked(&entry);
}

void GameListWidget::onListViewContextMenuRequested(const QPoint& point)
{
  const GameListEntry* entry = getSelectedEntry();
  if (!entry)
    return;

  emit entryContextMenuRequested(m_list_view->mapToGlobal(point), entry);
}

void GameListWidget::onTableViewHeaderContextMenuRequested(const QPoint& point)
{
  QMenu menu;

  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    if (column == GameListModel::Column_Cover)
      continue;

    QAction* action = menu.addAction(m_model->getColumnDisplayName(column));
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

void GameListWidget::listZoom(float delta)
{
  static constexpr float MIN_SCALE = 0.1f;
  static constexpr float MAX_SCALE = 2.0f;

  const float new_scale = std::clamp(m_model->getCoverScale() + delta, MIN_SCALE, MAX_SCALE);
  m_host_interface->SetFloatSettingValue("UI", "GameListCoverArtScale", new_scale);
  m_model->setCoverScale(new_scale);
  updateListFont();

  m_model->refresh();
}

void GameListWidget::gridZoomIn()
{
  listZoom(0.05f);
}

void GameListWidget::gridZoomOut()
{
  listZoom(-0.05f);
}

void GameListWidget::refreshGridCovers()
{
  m_model->refreshCovers();
}

void GameListWidget::showGameList()
{
  if (currentIndex() == 0)
    return;

  m_host_interface->SetBoolSettingValue("UI", "GameListGridView", false);
  setCurrentIndex(0);
  resizeTableViewColumnsToFit();
}

void GameListWidget::showGameGrid()
{
  if (currentIndex() == 1)
    return;

  m_host_interface->SetBoolSettingValue("UI", "GameListGridView", true);
  setCurrentIndex(1);
}

void GameListWidget::setShowCoverTitles(bool enabled)
{
  if (m_model->getShowCoverTitles() == enabled)
    return;

  m_host_interface->SetBoolSettingValue("UI", "GameListShowCoverTitles", enabled);
  m_model->setShowCoverTitles(enabled);
  if (isShowingGameGrid())
    m_model->refresh();
}

void GameListWidget::updateListFont()
{
  QFont font;
  font.setPointSizeF(16.0f * m_model->getCoverScale());
  m_list_view->setFont(font);
}

void GameListWidget::resizeEvent(QResizeEvent* event)
{
  QStackedWidget::resizeEvent(event);
  resizeTableViewColumnsToFit();
}

void GameListWidget::resizeTableViewColumnsToFit()
{
  QtUtils::ResizeColumnsForTableView(m_table_view, {
                                                     32,  // type
                                                     80,  // code
                                                     -1,  // title
                                                     -1,  // file title
                                                     200, // developer
                                                     200, // publisher
                                                     200, // genre
                                                     50,  // year
                                                     100, // players
                                                     80, // size
                                                     50,  // region
                                                     100  // compatibility
                                                   });
}

static TinyString getColumnVisibilitySettingsKeyName(int column)
{
  return TinyString::FromFormat("Show%s", GameListModel::getColumnName(static_cast<GameListModel::Column>(column)));
}

void GameListWidget::loadTableViewColumnVisibilitySettings()
{
  static constexpr std::array<bool, GameListModel::Column_Count> DEFAULT_VISIBILITY = {{
    true,  // type
    true,  // code
    true,  // title
    false, // file title
    true,  // developer
    false, // publisher
    false, // genre
    true,  // year
    false, // players
    true,  // size
    true,  // region
    true   // compatibility
  }};

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
  m_sort_model->sort(sort_column, sort_descending ? Qt::DescendingOrder : Qt::AscendingOrder);
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
  if (currentIndex() == 0)
  {
    const QItemSelectionModel* selection_model = m_table_view->selectionModel();
    if (!selection_model->hasSelection())
      return nullptr;

    const QModelIndexList selected_rows = selection_model->selectedRows();
    if (selected_rows.empty())
      return nullptr;

    const QModelIndex source_index = m_sort_model->mapToSource(selected_rows[0]);
    if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
      return nullptr;

    return &m_game_list->GetEntries().at(source_index.row());
  }
  else
  {
    const QItemSelectionModel* selection_model = m_list_view->selectionModel();
    if (!selection_model->hasSelection())
      return nullptr;

    const QModelIndex source_index = m_sort_model->mapToSource(selection_model->currentIndex());
    if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
      return nullptr;

    return &m_game_list->GetEntries().at(source_index.row());
  }
}

GameListGridListView::GameListGridListView(QWidget* parent /*= nullptr*/) : QListView(parent) {}

void GameListGridListView::wheelEvent(QWheelEvent* e)
{
  if (e->modifiers() & Qt::ControlModifier)
  {
    int dy = e->angleDelta().y();
    if (dy != 0)
    {
      if (dy < 0)
        zoomOut();
      else
        zoomIn();

      return;
    }
  }

  QListView::wheelEvent(e);
}
