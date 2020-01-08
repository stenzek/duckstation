#include "gamelistwidget.h"
#include "core/settings.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtGui/QPixmap>
#include <QtWidgets/QHeaderView>

class GameListModel : public QAbstractTableModel
{
public:
  enum Column : int
  {
    // Column_Icon,
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

          case Column_Region:
            // return QString(Settings::GetConsoleRegionName(ge.region));
            return {};

          case Column_Size:
            return QString("%1 MB").arg(static_cast<double>(ge.total_size) / 1048576.0, 0, 'f', 2);

          default:
            return {};
        }
      }

      case Qt::DecorationRole:
      {
        switch (index.column())
        {
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

private:
  void loadCommonImages()
  {
    // TODO: Use svg instead of png
    m_region_jp_pixmap.load(QStringLiteral(":/icons/flag-jp.png"));
    m_region_us_pixmap.load(QStringLiteral(":/icons/flag-us.png"));
    m_region_eu_pixmap.load(QStringLiteral(":/icons/flag-eu.png"));
  }

  GameList* m_game_list;
  int m_size;

  QPixmap m_region_jp_pixmap;
  QPixmap m_region_eu_pixmap;
  QPixmap m_region_us_pixmap;
};

GameListWidget::GameListWidget(QWidget* parent /* = nullptr */) : QStackedWidget(parent) {}

GameListWidget::~GameListWidget() = default;

void GameListWidget::initialize(QtHostInterface* host_interface)
{
  m_host_interface = host_interface;
  m_game_list = host_interface->getGameList();

  connect(m_host_interface, &QtHostInterface::gameListRefreshed, this, &GameListWidget::onGameListRefreshed);

  m_table_model = new GameListModel(m_game_list, this);
  m_table_view = new QTableView(this);
  m_table_view->setModel(m_table_model);
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

  insertWidget(0, m_table_view);
  setCurrentIndex(0);
}

void GameListWidget::onGameListRefreshed()
{
  m_table_model->refresh();
}

void GameListWidget::onTableViewItemDoubleClicked(const QModelIndex& index)
{
  if (!index.isValid() || index.row() >= static_cast<int>(m_game_list->GetEntryCount()))
    return;

  const GameList::GameListEntry& entry = m_game_list->GetEntries().at(index.row());
  emit bootEntryRequested(entry);
}

void GameListWidget::resizeEvent(QResizeEvent* event)
{
  QStackedWidget::resizeEvent(event);

  QtUtils::ResizeColumnsForTableView(m_table_view, {100, -1, 60, 100});
}
