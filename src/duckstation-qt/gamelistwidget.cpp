#include "gamelistwidget.h"
#include "common/assert.h"
#include "common/string_util.h"
#include "core/host_settings.h"
#include "core/settings.h"
#include "frontend-common/game_list.h"
#include "gamelistmodel.h"
#include "gamelistrefreshthread.h"
#include "qthost.h"
#include "qtutils.h"
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QGuiApplication>
#include <QtGui/QPixmap>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>

static constexpr float MIN_SCALE = 0.1f;
static constexpr float MAX_SCALE = 2.0f;

static const char* SUPPORTED_FORMATS_STRING =
  QT_TRANSLATE_NOOP(GameListWidget, ".cue (Cue Sheets)\n"
                                    ".iso/.img (Single Track Image)\n"
                                    ".ecm (Error Code Modeling Image)\n"
                                    ".mds (Media Descriptor Sidecar)\n"
                                    ".chd (Compressed Hunks of Data)\n"
                                    ".pbp (PlayStation Portable, Only Decrypted)");

class GameListSortModel final : public QSortFilterProxyModel
{
public:
  explicit GameListSortModel(GameListModel* parent) : QSortFilterProxyModel(parent), m_model(parent) {}

  void setFilterType(GameList::EntryType type)
  {
    m_filter_type = type;
    invalidateRowsFilter();
  }
  void setFilterRegion(DiscRegion region)
  {
    m_filter_region = region;
    invalidateRowsFilter();
  }
  void setFilterName(const QString& name)
  {
    m_filter_name = name;
    invalidateRowsFilter();
  }

  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override
  {
    if (m_filter_type != GameList::EntryType::Count || m_filter_region != DiscRegion::Count || !m_filter_name.isEmpty())
    {
      const auto lock = GameList::GetLock();
      const GameList::Entry* entry = GameList::GetEntryByIndex(source_row);
      if (m_filter_type != GameList::EntryType::Count && entry->type != m_filter_type)
        return false;
      if (m_filter_region != DiscRegion::Count && entry->region != m_filter_region)
        return false;
      if (!m_filter_name.isEmpty() &&
          !QString::fromStdString(entry->title).contains(m_filter_name, Qt::CaseInsensitive))
        return false;
    }

    return QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent);
  }

  bool lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const override
  {
    return m_model->lessThan(source_left, source_right, source_left.column());
  }

private:
  GameListModel* m_model;
  GameList::EntryType m_filter_type = GameList::EntryType::Count;
  DiscRegion m_filter_region = DiscRegion::Count;
  QString m_filter_name;
};

GameListWidget::GameListWidget(QWidget* parent /* = nullptr */) : QWidget(parent) {}

GameListWidget::~GameListWidget() = default;

void GameListWidget::initialize()
{
  m_model = new GameListModel(this);
  m_model->setCoverScale(Host::GetBaseFloatSettingValue("UI", "GameListCoverArtScale", 0.45f));
  m_model->setShowCoverTitles(Host::GetBaseBoolSettingValue("UI", "GameListShowCoverTitles", true));

  m_sort_model = new GameListSortModel(m_model);
  m_sort_model->setSourceModel(m_model);

  m_ui.setupUi(this);
  for (u32 type = 0; type < static_cast<u32>(GameList::EntryType::Count); type++)
  {
    m_ui.filterType->addItem(
      QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(type)),
      qApp->translate("GameList", GameList::GetEntryTypeDisplayName(static_cast<GameList::EntryType>(type))));
  }
  for (u32 region = 0; region < static_cast<u32>(DiscRegion::Count); region++)
  {
    m_ui.filterRegion->addItem(QtUtils::GetIconForRegion(static_cast<DiscRegion>(region)),
                               QString::fromUtf8(Settings::GetDiscRegionName(static_cast<DiscRegion>(region))));
  }

  connect(m_ui.viewGameList, &QPushButton::clicked, this, &GameListWidget::showGameList);
  connect(m_ui.viewGameGrid, &QPushButton::clicked, this, &GameListWidget::showGameGrid);
  connect(m_ui.gridScale, &QSlider::valueChanged, this, &GameListWidget::gridIntScale);
  connect(m_ui.viewGridTitles, &QPushButton::toggled, this, &GameListWidget::setShowCoverTitles);
  connect(m_ui.filterType, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_sort_model->setFilterType((index == 0) ? GameList::EntryType::Count :
                                               static_cast<GameList::EntryType>(index - 1));
  });
  connect(m_ui.filterRegion, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_sort_model->setFilterRegion((index == 0) ? DiscRegion::Count : static_cast<DiscRegion>(index - 1));
  });
  connect(m_ui.searchText, &QLineEdit::textChanged, this,
          [this](const QString& text) { m_sort_model->setFilterName(text); });

  // Works around a strange bug where after hiding the game list, the cursor for the whole window changes to a beam..
  // m_ui.searchText->setCursor(QCursor(Qt::ArrowCursor));

  m_table_view = new QTableView(m_ui.stack);
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
  m_table_view->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);

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

  m_ui.stack->insertWidget(0, m_table_view);

  m_list_view = new GameListGridListView(m_ui.stack);
  m_list_view->setModel(m_sort_model);
  m_list_view->setModelColumn(GameListModel::Column_Cover);
  m_list_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_list_view->setViewMode(QListView::IconMode);
  m_list_view->setResizeMode(QListView::Adjust);
  m_list_view->setUniformItemSizes(true);
  m_list_view->setItemAlignment(Qt::AlignHCenter);
  m_list_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_list_view->setFrameStyle(QFrame::NoFrame);
  m_list_view->setSpacing(m_model->getCoverArtSpacing());
  m_list_view->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);

  updateListFont();

  connect(m_list_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);
  connect(m_list_view, &GameListGridListView::zoomIn, this, &GameListWidget::gridZoomIn);
  connect(m_list_view, &GameListGridListView::zoomOut, this, &GameListWidget::gridZoomOut);
  connect(m_list_view, &QListView::activated, this, &GameListWidget::onListViewItemActivated);
  connect(m_list_view, &QListView::customContextMenuRequested, this, &GameListWidget::onListViewContextMenuRequested);

  m_ui.stack->insertWidget(1, m_list_view);

  m_empty_widget = new QWidget(m_ui.stack);
  m_empty_ui.setupUi(m_empty_widget);
  m_empty_ui.supportedFormats->setText(qApp->translate("GameListWidget", SUPPORTED_FORMATS_STRING));
  connect(m_empty_ui.addGameDirectory, &QPushButton::clicked, this, [this]() { emit addGameDirectoryRequested(); });
  connect(m_empty_ui.scanForNewGames, &QPushButton::clicked, this, [this]() { refresh(false); });
  m_ui.stack->insertWidget(2, m_empty_widget);

  if (Host::GetBaseBoolSettingValue("UI", "GameListGridView", false))
    m_ui.stack->setCurrentIndex(1);
  else
    m_ui.stack->setCurrentIndex(0);

  updateToolbar();
  resizeTableViewColumnsToFit();
}

bool GameListWidget::isShowingGameList() const
{
  return m_ui.stack->currentIndex() == 0;
}

bool GameListWidget::isShowingGameGrid() const
{
  return m_ui.stack->currentIndex() == 1;
}

bool GameListWidget::getShowGridCoverTitles() const
{
  return m_model->getShowCoverTitles();
}

void GameListWidget::refresh(bool invalidate_cache)
{
  cancelRefresh();

  m_refresh_thread = new GameListRefreshThread(invalidate_cache);
  connect(m_refresh_thread, &GameListRefreshThread::refreshProgress, this, &GameListWidget::onRefreshProgress,
          Qt::QueuedConnection);
  connect(m_refresh_thread, &GameListRefreshThread::refreshComplete, this, &GameListWidget::onRefreshComplete,
          Qt::QueuedConnection);
  m_refresh_thread->start();
}

void GameListWidget::cancelRefresh()
{
  if (!m_refresh_thread)
    return;

  m_refresh_thread->cancel();
  m_refresh_thread->wait();
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  AssertMsg(!m_refresh_thread, "Game list thread should be unreferenced by now");
}

void GameListWidget::onRefreshProgress(const QString& status, int current, int total)
{
  // switch away from the placeholder while we scan, in case we find anything
  if (m_ui.stack->currentIndex() == 2)
    m_ui.stack->setCurrentIndex(Host::GetBaseBoolSettingValue("UI", "GameListGridView", false) ? 1 : 0);

  m_model->refresh();
  emit refreshProgress(status, current, total);
}

void GameListWidget::onRefreshComplete()
{
  m_model->refresh();
  emit refreshComplete();

  AssertMsg(m_refresh_thread, "Has a refresh thread");
  m_refresh_thread->wait();
  delete m_refresh_thread;
  m_refresh_thread = nullptr;

  // if we still had no games, switch to the helper widget
  if (m_model->rowCount() == 0)
    m_ui.stack->setCurrentIndex(2);
}

void GameListWidget::onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
  const QModelIndex source_index = m_sort_model->mapToSource(current);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
    return;

  emit selectionChanged();
}

void GameListWidget::onTableViewItemActivated(const QModelIndex& index)
{
  const QModelIndex source_index = m_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
    return;

  emit entryActivated();
}

void GameListWidget::onTableViewContextMenuRequested(const QPoint& point)
{
  emit entryContextMenuRequested(m_table_view->mapToGlobal(point));
}

void GameListWidget::onListViewItemActivated(const QModelIndex& index)
{
  const QModelIndex source_index = m_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
    return;

  emit entryActivated();
}

void GameListWidget::onListViewContextMenuRequested(const QPoint& point)
{
  emit entryContextMenuRequested(m_list_view->mapToGlobal(point));
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
  const float new_scale = std::clamp(m_model->getCoverScale() + delta, MIN_SCALE, MAX_SCALE);
  Host::SetBaseFloatSettingValue("UI", "GameListCoverArtScale", new_scale);
  m_model->setCoverScale(new_scale);
  updateListFont();
  updateToolbar();

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

void GameListWidget::gridIntScale(int int_scale)
{
  const float new_scale = std::clamp(static_cast<float>(int_scale) / 100.0f, MIN_SCALE, MAX_SCALE);

  Host::SetBaseFloatSettingValue("UI", "GameListCoverArtScale", new_scale);
  m_model->setCoverScale(new_scale);
  updateListFont();
  updateToolbar();

  m_model->refresh();
}

void GameListWidget::refreshGridCovers()
{
  m_model->refreshCovers();
}

void GameListWidget::showGameList()
{
  if (m_ui.stack->currentIndex() == 0 || m_model->rowCount() == 0)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListGridView", false);
  m_ui.stack->setCurrentIndex(0);
  resizeTableViewColumnsToFit();
  updateToolbar();
  emit layoutChange();
}

void GameListWidget::showGameGrid()
{
  if (m_ui.stack->currentIndex() == 1 || m_model->rowCount() == 0)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListGridView", true);
  m_ui.stack->setCurrentIndex(1);
  updateToolbar();
  emit layoutChange();
}

void GameListWidget::setShowCoverTitles(bool enabled)
{
  if (m_model->getShowCoverTitles() == enabled)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListShowCoverTitles", enabled);
  m_model->setShowCoverTitles(enabled);
  if (isShowingGameGrid())
    m_model->refresh();
  updateToolbar();
  emit layoutChange();
}

void GameListWidget::updateListFont()
{
  QFont font;
  font.setPointSizeF(16.0f * m_model->getCoverScale());
  m_list_view->setFont(font);
}

void GameListWidget::updateToolbar()
{
  const bool grid_view = isShowingGameGrid();
  {
    QSignalBlocker sb(m_ui.viewGameGrid);
    m_ui.viewGameGrid->setChecked(grid_view);
  }
  {
    QSignalBlocker sb(m_ui.viewGameList);
    m_ui.viewGameList->setChecked(!grid_view);
  }
  {
    QSignalBlocker sb(m_ui.viewGridTitles);
    m_ui.viewGridTitles->setChecked(m_model->getShowCoverTitles());
  }
  {
    QSignalBlocker sb(m_ui.gridScale);
    m_ui.gridScale->setValue(static_cast<int>(m_model->getCoverScale() * 100.0f));
  }

  m_ui.viewGridTitles->setEnabled(grid_view);
  m_ui.gridScale->setEnabled(grid_view);
}

void GameListWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  resizeTableViewColumnsToFit();
}

void GameListWidget::resizeTableViewColumnsToFit()
{
  QtUtils::ResizeColumnsForTableView(m_table_view, {
                                                     45,  // type
                                                     80,  // code
                                                     -1,  // title
                                                     -1,  // file title
                                                     200, // developer
                                                     200, // publisher
                                                     200, // genre
                                                     50,  // year
                                                     100, // players
                                                     80,  // size
                                                     50,  // region
                                                     100  // compatibility
                                                   });
}

void GameListWidget::reloadCommonImages()
{
  m_model->reloadCommonImages();
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
    const bool visible = Host::GetBaseBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column),
                                                       DEFAULT_VISIBILITY[column]);
    m_table_view->setColumnHidden(column, !visible);
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings()
{
  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    const bool visible = !m_table_view->isColumnHidden(column);
    Host::SetBaseBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column), visible);
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings(int column)
{
  const bool visible = !m_table_view->isColumnHidden(column);
  Host::SetBaseBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column), visible);
}

void GameListWidget::loadTableViewColumnSortSettings()
{
  const GameListModel::Column DEFAULT_SORT_COLUMN = GameListModel::Column_Type;
  const bool DEFAULT_SORT_DESCENDING = false;

  const GameListModel::Column sort_column =
    GameListModel::getColumnIdForName(Host::GetBaseStringSettingValue("GameListTableView", "SortColumn"))
      .value_or(DEFAULT_SORT_COLUMN);
  const bool sort_descending =
    Host::GetBaseBoolSettingValue("GameListTableView", "SortDescending", DEFAULT_SORT_DESCENDING);
  m_sort_model->sort(sort_column, sort_descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void GameListWidget::saveTableViewColumnSortSettings()
{
  const int sort_column = m_table_view->horizontalHeader()->sortIndicatorSection();
  const bool sort_descending = (m_table_view->horizontalHeader()->sortIndicatorOrder() == Qt::DescendingOrder);

  if (sort_column >= 0 && sort_column < GameListModel::Column_Count)
  {
    Host::SetBaseStringSettingValue("GameListTableView", "SortColumn",
                                    GameListModel::getColumnName(static_cast<GameListModel::Column>(sort_column)));
  }

  Host::SetBaseBoolSettingValue("GameListTableView", "SortDescending", sort_descending);
}

const GameList::Entry* GameListWidget::getSelectedEntry() const
{
  if (m_ui.stack->currentIndex() == 0)
  {
    const QItemSelectionModel* selection_model = m_table_view->selectionModel();
    if (!selection_model->hasSelection())
      return nullptr;

    const QModelIndexList selected_rows = selection_model->selectedRows();
    if (selected_rows.empty())
      return nullptr;

    const QModelIndex source_index = m_sort_model->mapToSource(selected_rows[0]);
    if (!source_index.isValid())
      return nullptr;

    return GameList::GetEntryByIndex(source_index.row());
  }
  else
  {
    const QItemSelectionModel* selection_model = m_list_view->selectionModel();
    if (!selection_model->hasSelection())
      return nullptr;

    const QModelIndex source_index = m_sort_model->mapToSource(selection_model->currentIndex());
    if (!source_index.isValid())
      return nullptr;

    return GameList::GetEntryByIndex(source_index.row());
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
