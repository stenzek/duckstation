#include "gamelistwidget.h"
#include "common/string_util.h"
#include "core/game_list.h"
#include "core/settings.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QPixmap>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>

class GameListModel final : public QAbstractTableModel
{
public:
  enum Column : int
  {
    Column_Type,
    Column_Code,
    Column_Title,
    Column_FileTitle,
    Column_Region,
    Column_Size,

    Column_Count
  };

  static inline constexpr std::array<const char*, Column_Count> s_column_names = {
    {"Type", "Code", "Title", "File Title", "Region", "Size"}};

  static std::optional<Column> getColumnIdForName(std::string_view name)
  {
    for (int column = 0; column < Column_Count; column++)
    {
      if (name == s_column_names[column])
        return static_cast<Column>(column);
    }

    return std::nullopt;
  }

  GameListModel(GameList* game_list, QObject* parent = nullptr) : QAbstractTableModel(parent), m_game_list(game_list)
  {
    loadCommonImages();
  }
  ~GameListModel() = default;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override
  {
    if (parent.isValid())
      return 0;

    return static_cast<int>(m_game_list->GetEntryCount());
  }

  int columnCount(const QModelIndex& parent = QModelIndex()) const override
  {
    if (parent.isValid())
      return 0;

    return Column_Count;
  }

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
  {
    if (!index.isValid())
      return {};

    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(m_game_list->GetEntryCount()))
      return {};

    const GameListEntry& ge = m_game_list->GetEntries()[row];

    switch (role)
    {
      case Qt::DisplayRole:
      {
        switch (index.column())
        {
          case Column_Code:
            return QString::fromStdString(ge.code);

          case Column_Title:
            return QString::fromStdString(ge.title);

          case Column_FileTitle:
          {
            const std::string_view file_title(GameList::GetTitleForPath(ge.path.c_str()));
            return QString::fromUtf8(file_title.data(), static_cast<int>(file_title.length()));
          }

          case Column_Size:
            return QString("%1 MB").arg(static_cast<double>(ge.total_size) / 1048576.0, 0, 'f', 2);

          default:
            return {};
        }
      }

      case Qt::InitialSortOrderRole:
      {
        switch (index.column())
        {
          case Column_Type:
            return static_cast<int>(ge.type);

          case Column_Code:
            return QString::fromStdString(ge.code);

          case Column_Title:
            return QString::fromStdString(ge.title);

          case Column_FileTitle:
          {
            const std::string_view file_title(GameList::GetTitleForPath(ge.path.c_str()));
            return QString::fromUtf8(file_title.data(), static_cast<int>(file_title.length()));
          }

          case Column_Region:
            return static_cast<int>(ge.region);

          case Column_Size:
            return static_cast<qulonglong>(ge.total_size);

          default:
            return {};
        }
      }

      case Qt::DecorationRole:
      {
        switch (index.column())
        {
          case Column_Type:
          {
            switch (ge.type)
            {
              case GameListEntryType::Disc:
                return m_type_disc_pixmap;
              case GameListEntryType::PSExe:
              default:
                return m_type_exe_pixmap;
            }
          }

          case Column_Region:
          {
            switch (ge.region)
            {
              case DiscRegion::NTSC_J:
                return m_region_jp_pixmap;
              case DiscRegion::NTSC_U:
                return m_region_us_pixmap;
              case DiscRegion::PAL:
              default:
                return m_region_eu_pixmap;
            }
          }

          default:
            return {};
        }

        default:
          return {};
      }
    }
  }

  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
  {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= Column_Count)
      return {};

    return tr(s_column_names[section]);
  }

  void refresh()
  {
    beginResetModel();
    endResetModel();
  }

  bool titlesLessThan(int left_row, int right_row, bool ascending) const
  {
    if (left_row < 0 || left_row >= static_cast<int>(m_game_list->GetEntryCount()) || right_row < 0 ||
        right_row >= static_cast<int>(m_game_list->GetEntryCount()))
    {
      return false;
    }

    const GameListEntry& left = m_game_list->GetEntries().at(left_row);
    const GameListEntry& right = m_game_list->GetEntries().at(right_row);
    return ascending ? (left.title < right.title) : (right.title < left.title);
  }

private:
  void loadCommonImages()
  {
    // TODO: Use svg instead of png
    m_type_disc_pixmap.load(QStringLiteral(":/icons/media-optical-24.png"));
    m_type_exe_pixmap.load(QStringLiteral(":/icons/applications-system-24.png"));
    m_region_eu_pixmap.load(QStringLiteral(":/icons/flag-eu.png"));
    m_region_jp_pixmap.load(QStringLiteral(":/icons/flag-jp.png"));
    m_region_us_pixmap.load(QStringLiteral(":/icons/flag-us.png"));
    m_region_eu_pixmap.load(QStringLiteral(":/icons/flag-eu.png"));
  }

  GameList* m_game_list;

  QPixmap m_type_disc_pixmap;
  QPixmap m_type_exe_pixmap;

  QPixmap m_region_jp_pixmap;
  QPixmap m_region_eu_pixmap;
  QPixmap m_region_us_pixmap;
};

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
    const QVariant left = source_left.data(Qt::InitialSortOrderRole);
    const QVariant right = source_right.data(Qt::InitialSortOrderRole);
    if (left != right)
      return ascending ? (left < right) : (right > left);

    // fallback to sorting by title for equal items
    return m_model->titlesLessThan(source_left.row(), source_right.row(), ascending);
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
    QAction* action = menu.addAction(tr(GameListModel::s_column_names[column]));
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
  QtUtils::ResizeColumnsForTableView(m_table_view, {32, 80, -1, -1, 60, 100});
}

static QString getColumnVisibilitySettingsKeyName(int column)
{
  return QStringLiteral("GameListTableView/Show%1").arg(GameListModel::s_column_names[column]);
}

void GameListWidget::loadTableViewColumnVisibilitySettings()
{
  static constexpr std::array<bool, GameListModel::Column_Count> DEFAULT_VISIBILITY = {
    {true, true, true, false, true, true}};

  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    const bool visible =
      m_host_interface->getSettingValue(getColumnVisibilitySettingsKeyName(column), DEFAULT_VISIBILITY[column])
        .toBool();
    m_table_view->setColumnHidden(column, !visible);
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings()
{
  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    const bool visible = !m_table_view->isColumnHidden(column);
    m_host_interface->putSettingValue(getColumnVisibilitySettingsKeyName(column), visible);
  }
}

void GameListWidget::saveTableViewColumnVisibilitySettings(int column)
{
  const bool visible = !m_table_view->isColumnHidden(column);
  m_host_interface->putSettingValue(getColumnVisibilitySettingsKeyName(column), visible);
}

void GameListWidget::loadTableViewColumnSortSettings()
{
  const GameListModel::Column DEFAULT_SORT_COLUMN = GameListModel::Column_Type;
  const bool DEFAULT_SORT_DESCENDING = false;

  const GameListModel::Column sort_column =
    GameListModel::getColumnIdForName(
      m_host_interface->getSettingValue(QStringLiteral("GameListTableView/SortColumn")).toString().toStdString())
      .value_or(DEFAULT_SORT_COLUMN);
  const bool sort_descending =
    m_host_interface->getSettingValue(QStringLiteral("GameListTableView/SortDescending"), DEFAULT_SORT_DESCENDING)
      .toBool();
  m_table_sort_model->sort(sort_column, sort_descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void GameListWidget::saveTableViewColumnSortSettings()
{
  const int sort_column = m_table_view->horizontalHeader()->sortIndicatorSection();
  const bool sort_descending = (m_table_view->horizontalHeader()->sortIndicatorOrder() == Qt::DescendingOrder);

  if (sort_column >= 0 && sort_column < GameListModel::Column_Count)
  {
    m_host_interface->putSettingValue(QStringLiteral("GameListTableView/SortColumn"),
                                      QString::fromUtf8(GameListModel::s_column_names[sort_column]));
  }

  m_host_interface->putSettingValue(QStringLiteral("GameListTableView/SortDescending"), sort_descending);
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
