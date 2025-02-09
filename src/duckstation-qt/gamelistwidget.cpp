// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistwidget.h"
#include "gamelistmodel.h"
#include "gamelistrefreshthread.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"

#include "core/fullscreen_ui.h"
#include "core/game_list.h"
#include "core/host.h"
#include "core/settings.h"

#include "common/assert.h"
#include "common/string_util.h"

#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyledItemDelegate>

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

  bool isMergingDiscSets() const { return m_merge_disc_sets; }

  void setMergeDiscSets(bool enabled)
  {
    m_merge_disc_sets = enabled;
    invalidateRowsFilter();
  }

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
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = m_model->hasTakenGameList() ?
                                     m_model->getTakenGameListEntry(static_cast<u32>(source_row)) :
                                     GameList::GetEntryByIndex(static_cast<u32>(source_row));
    if (!entry)
      return false;

    if (m_merge_disc_sets)
    {
      if (entry->disc_set_member)
        return false;
    }
    else
    {
      if (entry->IsDiscSet())
        return false;
    }

    if (m_filter_type != GameList::EntryType::Count && entry->type != m_filter_type)
      return false;

    if (m_filter_region != DiscRegion::Count && entry->region != m_filter_region)
      return false;

    if (!m_filter_name.isEmpty() && !QString::fromStdString(entry->title).contains(m_filter_name, Qt::CaseInsensitive))
      return false;

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
  bool m_merge_disc_sets = true;
};

namespace {
class GameListCenterIconStyleDelegate final : public QStyledItemDelegate
{
public:
  GameListCenterIconStyleDelegate(QWidget* parent) : QStyledItemDelegate(parent) {}
  ~GameListCenterIconStyleDelegate() = default;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    // https://stackoverflow.com/questions/32216568/how-to-set-icon-center-in-qtableview
    Q_ASSERT(index.isValid());

    const QRect& r = option.rect;
    const QPixmap pix = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
    const int pix_width = static_cast<int>(pix.width() / pix.devicePixelRatio());
    const int pix_height = static_cast<int>(pix.height() / pix.devicePixelRatio());

    // draw pixmap at center of item
    const QPoint p = QPoint((r.width() - pix_width) / 2, (r.height() - pix_height) / 2);
    painter->drawPixmap(r.topLeft() + p, pix);
  }
};

class GameListAchievementsStyleDelegate : public QStyledItemDelegate
{
public:
  GameListAchievementsStyleDelegate(QWidget* parent, GameListModel* model, GameListSortModel* sort_model)
    : QStyledItemDelegate(parent), m_model(model), m_sort_model(sort_model)
  {
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    Q_ASSERT(index.isValid());

    u32 num_achievements = 0;
    u32 num_unlocked = 0;
    u32 num_unlocked_hardcore = 0;
    bool mastered = false;

    const auto get_data_from_entry = [&num_achievements, &num_unlocked, &num_unlocked_hardcore,
                                      &mastered](const GameList::Entry* entry) {
      if (!entry)
        return;

      num_achievements = entry->num_achievements;
      num_unlocked = entry->unlocked_achievements;
      num_unlocked_hardcore = entry->unlocked_achievements_hc;
      mastered = entry->AreAchievementsMastered();
    };

    const QModelIndex source_index = m_sort_model->mapToSource(index);
    if (m_model->hasTakenGameList()) [[unlikely]]
    {
      get_data_from_entry(m_model->getTakenGameListEntry(static_cast<u32>(source_index.row())));
    }
    else
    {
      const auto lock = GameList::GetLock();
      get_data_from_entry(GameList::GetEntryByIndex(static_cast<u32>(source_index.row())));
    }

    QRect r = option.rect;

    const QPixmap& icon = (num_achievements > 0) ? (mastered ? m_model->getMasteredAchievementsPixmap() :
                                                               m_model->getHasAchievementsPixmap()) :
                                                   m_model->getNoAchievementsPixmap();
    const int icon_height = static_cast<int>(icon.width() / icon.devicePixelRatio());
    painter->drawPixmap(r.topLeft() + QPoint(4, (r.height() - icon_height) / 2), icon);
    r.setLeft(r.left() + 4 + icon.width());

    if (num_achievements > 0)
    {
      const QFontMetrics fm(painter->fontMetrics());

      // display hardcore in parenthesis only if there are actually hc unlocks
      const bool display_hardcore = (num_unlocked > 0 && num_unlocked_hardcore > 0);
      const bool display_hardcore_only = (num_unlocked == 0 && num_unlocked_hardcore > 0);
      const QString first = QStringLiteral("%1").arg(display_hardcore_only ? num_unlocked_hardcore : num_unlocked);
      const QString total = QStringLiteral("/%3").arg(num_achievements);

      const QPalette& palette = static_cast<QWidget*>(parent())->palette();
      const QColor hc_color = QColor(44, 151, 250);

      painter->setPen(display_hardcore_only ? hc_color : palette.color(QPalette::WindowText));
      painter->drawText(r, Qt::AlignVCenter, first);
      r.setLeft(r.left() + fm.size(Qt::TextSingleLine, first).width());

      if (display_hardcore)
      {
        const QString hc = QStringLiteral("(%2)").arg(num_unlocked_hardcore);
        painter->setPen(hc_color);
        painter->drawText(r, Qt::AlignVCenter, hc);
        r.setLeft(r.left() + fm.size(Qt::TextSingleLine, hc).width());
      }

      painter->setPen(palette.color(QPalette::WindowText));
      painter->drawText(r, Qt::AlignVCenter, total);
    }
    else
    {
      painter->drawText(r, Qt::AlignVCenter, QStringLiteral("N/A"));
    }
  }

private:
  GameListModel* m_model;
  GameListSortModel* m_sort_model;
};

} // namespace

GameListWidget::GameListWidget(QWidget* parent /* = nullptr */) : QWidget(parent)
{
}

GameListWidget::~GameListWidget() = default;

void GameListWidget::initialize()
{
  const float cover_scale = Host::GetBaseFloatSettingValue("UI", "GameListCoverArtScale", 0.45f);
  const bool show_cover_titles = Host::GetBaseBoolSettingValue("UI", "GameListShowCoverTitles", true);
  const bool merge_disc_sets = Host::GetBaseBoolSettingValue("UI", "GameListMergeDiscSets", true);
  const bool show_game_icons = Host::GetBaseBoolSettingValue("UI", "GameListShowGameIcons", true);
  m_model = new GameListModel(cover_scale, show_cover_titles, show_game_icons, this);
  m_model->updateCacheSize(width(), height());

  m_sort_model = new GameListSortModel(m_model);
  m_sort_model->setSourceModel(m_model);
  m_sort_model->setMergeDiscSets(merge_disc_sets);

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
  connect(m_ui.viewMergeDiscSets, &QPushButton::toggled, this, &GameListWidget::setMergeDiscSets);
  connect(m_ui.filterType, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_sort_model->setFilterType((index == 0) ? GameList::EntryType::Count :
                                               static_cast<GameList::EntryType>(index - 1));
  });
  connect(m_ui.filterRegion, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_sort_model->setFilterRegion((index == 0) ? DiscRegion::Count : static_cast<DiscRegion>(index - 1));
  });
  connect(m_ui.searchText, &QLineEdit::textChanged, this,
          [this](const QString& text) { m_sort_model->setFilterName(text); });
  connect(m_ui.searchText, &QLineEdit::returnPressed, this, &GameListWidget::onSearchReturnPressed);

  GameListCenterIconStyleDelegate* center_icon_delegate = new GameListCenterIconStyleDelegate(this);
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
  m_table_view->setItemDelegateForColumn(GameListModel::Column_Icon, center_icon_delegate);
  m_table_view->setItemDelegateForColumn(GameListModel::Column_Region, center_icon_delegate);
  m_table_view->setItemDelegateForColumn(GameListModel::Column_Achievements,
                                         new GameListAchievementsStyleDelegate(this, m_model, m_sort_model));

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
  m_list_view->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list_view->setViewMode(QListView::IconMode);
  m_list_view->setResizeMode(QListView::Adjust);
  m_list_view->setUniformItemSizes(true);
  m_list_view->setItemAlignment(Qt::AlignHCenter);
  m_list_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_list_view->setFrameStyle(QFrame::NoFrame);
  m_list_view->setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);
  m_list_view->verticalScrollBar()->setSingleStep(15);
  onCoverScaleChanged();

  connect(m_list_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);
  connect(m_list_view, &GameListGridListView::zoomIn, this, &GameListWidget::gridZoomIn);
  connect(m_list_view, &GameListGridListView::zoomOut, this, &GameListWidget::gridZoomOut);
  connect(m_list_view, &QListView::activated, this, &GameListWidget::onListViewItemActivated);
  connect(m_list_view, &QListView::customContextMenuRequested, this, &GameListWidget::onListViewContextMenuRequested);
  connect(m_model, &GameListModel::coverScaleChanged, this, &GameListWidget::onCoverScaleChanged);

  m_ui.stack->insertWidget(1, m_list_view);

  m_empty_widget = new QWidget(m_ui.stack);
  m_empty_ui.setupUi(m_empty_widget);
  m_empty_ui.supportedFormats->setText(qApp->translate("GameListWidget", SUPPORTED_FORMATS_STRING));
  connect(m_empty_ui.addGameDirectory, &QPushButton::clicked, this, [this]() { emit addGameDirectoryRequested(); });
  connect(m_empty_ui.scanForNewGames, &QPushButton::clicked, this, [this]() { refresh(false); });
  m_ui.stack->insertWidget(2, m_empty_widget);

  const bool grid_view = Host::GetBaseBoolSettingValue("UI", "GameListGridView", false);
  if (grid_view)
    m_ui.stack->setCurrentIndex(1);
  else
    m_ui.stack->setCurrentIndex(0);
  setFocusProxy(grid_view ? static_cast<QWidget*>(m_list_view) : static_cast<QWidget*>(m_table_view));

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

bool GameListWidget::isShowingGridCoverTitles() const
{
  return m_model->getShowCoverTitles();
}

bool GameListWidget::isMergingDiscSets() const
{
  return m_sort_model->isMergingDiscSets();
}

bool GameListWidget::isShowingGameIcons() const
{
  return m_model->getShowGameIcons();
}

void GameListWidget::refresh(bool invalidate_cache)
{
  cancelRefresh();

  if (!invalidate_cache)
    m_model->takeGameList();

  m_refresh_thread = new GameListRefreshThread(invalidate_cache);
  connect(m_refresh_thread, &GameListRefreshThread::refreshProgress, this, &GameListWidget::onRefreshProgress,
          Qt::QueuedConnection);
  connect(m_refresh_thread, &GameListRefreshThread::refreshComplete, this, &GameListWidget::onRefreshComplete,
          Qt::QueuedConnection);
  m_refresh_thread->start();
}

void GameListWidget::refreshModel()
{
  m_model->refresh();
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

void GameListWidget::reloadThemeSpecificImages()
{
  m_model->reloadThemeSpecificImages();
}

void GameListWidget::onRefreshProgress(const QString& status, int current, int total, float time)
{
  // Avoid spamming the UI on very short refresh (e.g. game exit).
  static constexpr float SHORT_REFRESH_TIME = 0.5f;
  if (!m_model->hasTakenGameList())
    m_model->refresh();

  // switch away from the placeholder while we scan, in case we find anything
  if (m_ui.stack->currentIndex() == 2)
  {
    const bool grid_view = Host::GetBaseBoolSettingValue("UI", "GameListGridView", false);
    m_ui.stack->setCurrentIndex(grid_view ? 1 : 0);
    setFocusProxy(grid_view ? static_cast<QWidget*>(m_list_view) : static_cast<QWidget*>(m_table_view));
  }

  if (!m_model->hasTakenGameList() || time >= SHORT_REFRESH_TIME)
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
  {
    m_ui.stack->setCurrentIndex(2);
    setFocusProxy(nullptr);
  }
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

  if (qApp->keyboardModifiers().testFlag(Qt::AltModifier))
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryByIndex(static_cast<u32>(source_index.row()));
    if (entry)
      SettingsWindow::openGamePropertiesDialog(entry->path, entry->title, entry->serial, entry->hash, entry->region);
  }
  else
  {
    emit entryActivated();
  }
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

void GameListWidget::onCoverScaleChanged()
{
  m_model->updateCacheSize(width(), height());

  m_list_view->setSpacing(m_model->getCoverArtSpacing());

  QFont font;
  font.setPointSizeF(20.0f * m_model->getCoverScale());
  m_list_view->setFont(font);
}

void GameListWidget::listZoom(float delta)
{
  const float new_scale = std::clamp(m_model->getCoverScale() + delta, MIN_SCALE, MAX_SCALE);
  Host::SetBaseFloatSettingValue("UI", "GameListCoverArtScale", new_scale);
  Host::CommitBaseSettingChanges();
  m_model->setCoverScale(new_scale);
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
  Host::CommitBaseSettingChanges();
  m_model->setCoverScale(new_scale);
  updateToolbar();

  m_model->refresh();
}

void GameListWidget::refreshGridCovers()
{
  m_model->refreshCovers();
  Host::RunOnCPUThread(&FullscreenUI::InvalidateCoverCache);
}

void GameListWidget::focusSearchWidget()
{
  m_ui.searchText->setFocus(Qt::ShortcutFocusReason);
}

void GameListWidget::onSearchReturnPressed()
{
  // Anything to switch focus to?
  const int rows = m_sort_model->rowCount();
  if (rows == 0)
    return;

  QAbstractItemView* const target =
    isShowingGameGrid() ? static_cast<QAbstractItemView*>(m_list_view) : static_cast<QAbstractItemView*>(m_table_view);
  target->selectionModel()->select(m_sort_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  target->setFocus(Qt::ShortcutFocusReason);
}

void GameListWidget::showGameList()
{
  if (m_ui.stack->currentIndex() == 0 || m_model->rowCount() == 0)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListGridView", false);
  Host::CommitBaseSettingChanges();
  m_ui.stack->setCurrentIndex(0);
  setFocusProxy(m_table_view);
  resizeTableViewColumnsToFit();
  updateToolbar();
  emit layoutChanged();
}

void GameListWidget::showGameGrid()
{
  if (m_ui.stack->currentIndex() == 1 || m_model->rowCount() == 0)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListGridView", true);
  Host::CommitBaseSettingChanges();
  m_ui.stack->setCurrentIndex(1);
  setFocusProxy(m_list_view);
  updateToolbar();
  emit layoutChanged();
}

void GameListWidget::setShowCoverTitles(bool enabled)
{
  if (m_model->getShowCoverTitles() == enabled)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListShowCoverTitles", enabled);
  Host::CommitBaseSettingChanges();
  m_model->setShowCoverTitles(enabled);
  if (isShowingGameGrid())
    m_model->refresh();
  updateToolbar();
  emit layoutChanged();
}

void GameListWidget::setMergeDiscSets(bool enabled)
{
  if (m_sort_model->isMergingDiscSets() == enabled)
  {
    updateToolbar();
    return;
  }

  Host::SetBaseBoolSettingValue("UI", "GameListMergeDiscSets", enabled);
  Host::CommitBaseSettingChanges();
  m_sort_model->setMergeDiscSets(enabled);
  updateToolbar();
  emit layoutChanged();
}

void GameListWidget::setShowGameIcons(bool enabled)
{
  if (m_model->getShowGameIcons() == enabled)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListShowGameIcons", enabled);
  Host::CommitBaseSettingChanges();
  m_model->setShowGameIcons(enabled);
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
    QSignalBlocker sb(m_ui.viewMergeDiscSets);
    m_ui.viewMergeDiscSets->setChecked(m_sort_model->isMergingDiscSets());
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
                                                     80,  // time played
                                                     80,  // last played
                                                     80,  // file size
                                                     80,  // size
                                                     50,  // region
                                                     90,  // achievements
                                                     100  // compatibility
                                                   });
}

static TinyString getColumnVisibilitySettingsKeyName(int column)
{
  return TinyString::from_format("Show{}", GameListModel::getColumnName(static_cast<GameListModel::Column>(column)));
}

void GameListWidget::loadTableViewColumnVisibilitySettings()
{
  static constexpr std::array<bool, GameListModel::Column_Count> DEFAULT_VISIBILITY = {{
    true,  // type
    true,  // code
    true,  // title
    false, // file title
    false, // developer
    false, // publisher
    false, // genre
    false, // year
    false, // players
    true,  // time played
    true,  // last played
    true,  // file size
    false, // size
    true,  // region
    false, // achievements
    false  // compatibility
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
    Host::CommitBaseSettingChanges();
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings(int column)
{
  const bool visible = !m_table_view->isColumnHidden(column);
  Host::SetBaseBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column), visible);
  Host::CommitBaseSettingChanges();
}

void GameListWidget::loadTableViewColumnSortSettings()
{
  const GameListModel::Column DEFAULT_SORT_COLUMN = GameListModel::Column_Icon;
  const bool DEFAULT_SORT_DESCENDING = false;

  const GameListModel::Column sort_column =
    GameListModel::getColumnIdForName(Host::GetBaseStringSettingValue("GameListTableView", "SortColumn"))
      .value_or(DEFAULT_SORT_COLUMN);
  const bool sort_descending =
    Host::GetBaseBoolSettingValue("GameListTableView", "SortDescending", DEFAULT_SORT_DESCENDING);
  const Qt::SortOrder sort_order = sort_descending ? Qt::DescendingOrder : Qt::AscendingOrder;
  m_sort_model->sort(sort_column, sort_order);
  if (QHeaderView* hv = m_table_view->horizontalHeader())
    hv->setSortIndicator(sort_column, sort_order);
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
  Host::CommitBaseSettingChanges();
}

void GameListWidget::setTableViewColumnHidden(int column, bool hidden)
{
  DebugAssert(column < GameListModel::Column_Count);
  if (m_table_view->isColumnHidden(column) == hidden)
    return;

  m_table_view->setColumnHidden(column, hidden);
  saveTableViewColumnVisibilitySettings(column);
  resizeTableViewColumnsToFit();
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

GameListGridListView::GameListGridListView(QWidget* parent /*= nullptr*/) : QListView(parent)
{
}

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
