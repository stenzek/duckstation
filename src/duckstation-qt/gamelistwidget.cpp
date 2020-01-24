#include "gamelistwidget.h"
#include "core/settings.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QPixmap>
#include <QtWidgets/QHeaderView>

class GameListModel final : public QAbstractTableModel
{
public:
  enum Column : int
  {
    Column_Type,
    Column_Code,
    Column_Title,
    Column_Region,
    Column_Size,

    Column_Count
  };

  GameListModel(GameList* game_list, QObject* parent = nullptr)
    : QAbstractTableModel(parent), m_game_list(game_list), m_size(static_cast<int>(m_game_list->GetEntryCount()))
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

    const GameList::GameListEntry& ge = m_game_list->GetEntries()[row];

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
              case GameList::EntryType::Disc:
                return m_type_disc_pixmap;
              case GameList::EntryType::PSExe:
              default:
                return m_type_exe_pixmap;
            }
          }

          case Column_Region:
          {
            switch (ge.region)
            {
              case ConsoleRegion::NTSC_J:
                return m_region_jp_pixmap;
              case ConsoleRegion::NTSC_U:
                return m_region_us_pixmap;
              case ConsoleRegion::PAL:
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
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
      return {};

    switch (section)
    {
      case Column_Type:
        return "Type";

      case Column_Code:
        return "Code";

      case Column_Title:
        return "Title";

      case Column_Region:
        return "Region";

      case Column_Size:
        return "Size";

      default:
        return {};
    }
  }

  void refresh()
  {
    if (m_size > 0)
    {
      beginRemoveRows(QModelIndex(), 0, m_size - 1);
      endRemoveRows();
    }

    m_size = static_cast<int>(m_game_list->GetEntryCount());
    beginInsertRows(QModelIndex(), 0, m_size - 1);
    endInsertRows();
  }

  bool titlesLessThan(int left_row, int right_row, bool ascending) const
  {
    if (left_row < 0 || left_row >= static_cast<int>(m_game_list->GetEntryCount()) || right_row < 0 ||
        right_row >= static_cast<int>(m_game_list->GetEntryCount()))
    {
      return false;
    }

    const GameList::GameListEntry& left = m_game_list->GetEntries().at(left_row);
    const GameList::GameListEntry& right = m_game_list->GetEntries().at(right_row);
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
  int m_size;

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
      return ascending ? (left < right) : (right < left);

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
  m_table_view->setAlternatingRowColors(true);
  m_table_view->setShowGrid(false);
  m_table_view->setCurrentIndex({});
  m_table_view->horizontalHeader()->setHighlightSections(false);
  m_table_view->verticalHeader()->hide();
  m_table_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  m_table_view->resizeColumnsToContents();

  connect(m_table_view, &QTableView::doubleClicked, this, &GameListWidget::onTableViewItemDoubleClicked);
  connect(m_table_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);

  insertWidget(0, m_table_view);
  setCurrentIndex(0);
}

void GameListWidget::onGameListRefreshed()
{
  m_table_model->refresh();
}

void GameListWidget::onTableViewItemDoubleClicked(const QModelIndex& index)
{
  const QModelIndex source_index = m_table_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
    return;

  const GameList::GameListEntry& entry = m_game_list->GetEntries().at(source_index.row());
  emit bootEntryRequested(&entry);
}

void GameListWidget::onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
  const QModelIndex source_index = m_table_sort_model->mapToSource(current);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
  {
    emit entrySelected(nullptr);
    return;
  }

  const GameList::GameListEntry& entry = m_game_list->GetEntries().at(source_index.row());
  emit entrySelected(&entry);
}

void GameListWidget::resizeEvent(QResizeEvent* event)
{
  QStackedWidget::resizeEvent(event);

  QtUtils::ResizeColumnsForTableView(m_table_view, {32, 80, -1, 60, 100});
}
