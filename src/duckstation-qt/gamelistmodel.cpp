#include "gamelistmodel.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/string_util.h"
#include "core/system.h"
#include "qthost.h"
#include "qtutils.h"
#include <QtConcurrent/QtConcurrent>
#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPainter>

static constexpr std::array<const char*, GameListModel::Column_Count> s_column_names = {
  {"Type", "Serial", "Title", "File Title", "Developer", "Publisher", "Genre", "Year", "Players", "Size", "Region",
   "Compatibility", "Cover"}};

static constexpr int COVER_ART_WIDTH = 512;
static constexpr int COVER_ART_HEIGHT = 512;
static constexpr int COVER_ART_SPACING = 32;
static constexpr int MIN_COVER_CACHE_SIZE = 256;

static int DPRScale(int size, float dpr)
{
  return static_cast<int>(static_cast<float>(size) * dpr);
}

static int DPRUnscale(int size, float dpr)
{
  return static_cast<int>(static_cast<float>(size) / dpr);
}

static void resizeAndPadPixmap(QPixmap* pm, int expected_width, int expected_height, float dpr)
{
  const int dpr_expected_width = DPRScale(expected_width, dpr);
  const int dpr_expected_height = DPRScale(expected_height, dpr);
  if (pm->width() == dpr_expected_width && pm->height() == dpr_expected_height)
    return;

  *pm = pm->scaled(dpr_expected_width, dpr_expected_height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  if (pm->width() == dpr_expected_width && pm->height() == dpr_expected_height)
    return;

  // QPainter works in unscaled coordinates.
  int xoffs = 0;
  int yoffs = 0;
  if (pm->width() < dpr_expected_width)
    xoffs = DPRUnscale((dpr_expected_width - pm->width()) / 2, dpr);
  if (pm->height() < dpr_expected_height)
    yoffs = DPRUnscale((dpr_expected_height - pm->height()) / 2, dpr);

  QPixmap padded_image(dpr_expected_width, dpr_expected_height);
  padded_image.setDevicePixelRatio(dpr);
  padded_image.fill(Qt::transparent);
  QPainter painter;
  if (painter.begin(&padded_image))
  {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawPixmap(xoffs, yoffs, *pm);
    painter.setCompositionMode(QPainter::CompositionMode_Destination);
    painter.fillRect(padded_image.rect(), QColor(0, 0, 0, 0));
    painter.end();
  }

  *pm = padded_image;
}

static QPixmap createPlaceholderImage(const QPixmap& placeholder_pixmap, int width, int height, float scale,
                                      const std::string& title)
{
  const float dpr = qApp->devicePixelRatio();
  QPixmap pm(placeholder_pixmap.copy());
  pm.setDevicePixelRatio(dpr);
  if (pm.isNull())
    return QPixmap();

  resizeAndPadPixmap(&pm, width, height, dpr);
  QPainter painter;
  if (painter.begin(&pm))
  {
    QFont font;
    font.setPointSize(std::max(static_cast<int>(32.0f * scale), 1));
    painter.setFont(font);
    painter.setPen(Qt::white);

    const QRect text_rc(0, 0, static_cast<int>(static_cast<float>(width)),
                        static_cast<int>(static_cast<float>(height)));
    painter.drawText(text_rc, Qt::AlignCenter | Qt::TextWordWrap, QString::fromStdString(title));
    painter.end();
  }

  return pm;
}

std::optional<GameListModel::Column> GameListModel::getColumnIdForName(std::string_view name)
{
  for (int column = 0; column < Column_Count; column++)
  {
    if (name == s_column_names[column])
      return static_cast<Column>(column);
  }

  return std::nullopt;
}

const char* GameListModel::getColumnName(Column col)
{
  return s_column_names[static_cast<int>(col)];
}

GameListModel::GameListModel(QObject* parent /* = nullptr */)
  : QAbstractTableModel(parent), m_cover_pixmap_cache(MIN_COVER_CACHE_SIZE)
{
  loadCommonImages();
  setColumnDisplayNames();
}
GameListModel::~GameListModel() = default;

void GameListModel::setCoverScale(float scale)
{
  if (m_cover_scale == scale)
    return;

  m_cover_pixmap_cache.Clear();
  m_cover_scale = scale;
  m_loading_pixmap = QPixmap(getCoverArtWidth(), getCoverArtHeight());
  m_loading_pixmap.fill(QColor(0, 0, 0, 0));
}

void GameListModel::refreshCovers()
{
  m_cover_pixmap_cache.Clear();
  refresh();
}

void GameListModel::updateCacheSize(int width, int height)
{
  // This is a bit conversative, since it doesn't consider padding, but better to be over than under.
  const int cover_width = getCoverArtWidth();
  const int cover_height = getCoverArtHeight();
  const int num_columns = ((width + (cover_width - 1)) / cover_width);
  const int num_rows = ((height + (cover_height - 1)) / cover_height);
  m_cover_pixmap_cache.SetMaxCapacity(static_cast<int>(std::max(num_columns * num_rows, MIN_COVER_CACHE_SIZE)));
}

void GameListModel::reloadCommonImages()
{
  loadCommonImages();
  refresh();
}

void GameListModel::loadOrGenerateCover(const GameList::Entry* ge)
{
  QFuture<QPixmap> future =
    QtConcurrent::run([this, path = ge->path, title = ge->title, serial = ge->serial]() -> QPixmap {
      QPixmap image;
      const std::string cover_path(GameList::GetCoverImagePath(path, serial, title));
      if (!cover_path.empty())
      {
        const float dpr = qApp->devicePixelRatio();
        image = QPixmap(QString::fromStdString(cover_path));
        if (!image.isNull())
        {
          image.setDevicePixelRatio(dpr);
          resizeAndPadPixmap(&image, getCoverArtWidth(), getCoverArtHeight(), dpr);
        }
      }

      if (image.isNull())
        image =
          createPlaceholderImage(m_placeholder_pixmap, getCoverArtWidth(), getCoverArtHeight(), m_cover_scale, title);

      return image;
    });

  // Context must be 'this' so we run on the UI thread.
  future.then(this, [this, path = ge->path](QPixmap pm) {
    m_cover_pixmap_cache.Insert(std::move(path), std::move(pm));
    invalidateCoverForPath(path);
  });
}

void GameListModel::invalidateCoverForPath(const std::string& path)
{
  // This isn't ideal, but not sure how else we can get the row, when it might change while scanning...
  auto lock = GameList::GetLock();
  const u32 count = GameList::GetEntryCount();
  std::optional<u32> row;
  for (u32 i = 0; i < count; i++)
  {
    if (GameList::GetEntryByIndex(i)->path == path)
    {
      row = i;
      break;
    }
  }
  if (!row.has_value())
  {
    // Game removed?
    return;
  }

  const QModelIndex mi(index(static_cast<int>(row.value()), Column_Cover));
  emit dataChanged(mi, mi, {Qt::DecorationRole});
}

int GameListModel::getCoverArtWidth() const
{
  return std::max(static_cast<int>(static_cast<float>(COVER_ART_WIDTH) * m_cover_scale), 1);
}

int GameListModel::getCoverArtHeight() const
{
  return std::max(static_cast<int>(static_cast<float>(COVER_ART_HEIGHT) * m_cover_scale), 1);
}

int GameListModel::getCoverArtSpacing() const
{
  return std::max(static_cast<int>(static_cast<float>(COVER_ART_SPACING) * m_cover_scale), 1);
}

int GameListModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid())
    return 0;

  return static_cast<int>(GameList::GetEntryCount());
}

int GameListModel::columnCount(const QModelIndex& parent) const
{
  if (parent.isValid())
    return 0;

  return Column_Count;
}

QVariant GameListModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return {};

  const int row = index.row();
  if (row < 0 || row >= static_cast<int>(GameList::GetEntryCount()))
    return {};

  const auto lock = GameList::GetLock();
  const GameList::Entry* ge = GameList::GetEntryByIndex(row);
  if (!ge)
    return {};

  switch (role)
  {
    case Qt::DisplayRole:
    {
      switch (index.column())
      {
        case Column_Serial:
          return QString::fromStdString(ge->serial);

        case Column_Title:
          return QString::fromStdString(ge->title);

        case Column_FileTitle:
          return QtUtils::StringViewToQString(Path::GetFileTitle(ge->path));

        case Column_Developer:
          return QString::fromStdString(ge->developer);

        case Column_Publisher:
          return QString::fromStdString(ge->publisher);

        case Column_Genre:
          return QString::fromStdString(ge->genre);

        case Column_Year:
        {
          if (ge->release_date != 0)
          {
            return QStringLiteral("%1").arg(
              QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ge->release_date), Qt::UTC).date().year());
          }
          else
          {
            return QString();
          }
        }

        case Column_Players:
        {
          if (ge->min_players == ge->max_players)
            return QStringLiteral("%1").arg(ge->min_players);
          else
            return QStringLiteral("%1-%2").arg(ge->min_players).arg(ge->max_players);
        }

        case Column_Size:
          return QString("%1 MB").arg(static_cast<double>(ge->total_size) / 1048576.0, 0, 'f', 2);

        case Column_Cover:
        {
          if (m_show_titles_for_covers)
            return QString::fromStdString(ge->title);
          else
            return {};
        }

        default:
          return {};
      }
    }

    case Qt::InitialSortOrderRole:
    {
      switch (index.column())
      {
        case Column_Type:
          return static_cast<int>(ge->type);

        case Column_Serial:
          return QString::fromStdString(ge->serial);

        case Column_Title:
        case Column_Cover:
          return QString::fromStdString(ge->title);

        case Column_FileTitle:
          return QtUtils::StringViewToQString(Path::GetFileTitle(ge->path));

        case Column_Developer:
          return QString::fromStdString(ge->developer);

        case Column_Publisher:
          return QString::fromStdString(ge->publisher);

        case Column_Genre:
          return QString::fromStdString(ge->genre);

        case Column_Year:
          return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ge->release_date), Qt::UTC).date().year();

        case Column_Players:
          return static_cast<int>(ge->max_players);

        case Column_Region:
          return static_cast<int>(ge->region);

        case Column_Compatibility:
          return static_cast<int>(ge->compatibility);

        case Column_Size:
          return static_cast<qulonglong>(ge->total_size);

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
          // TODO: Test for settings
          return m_type_pixmaps[static_cast<u32>(ge->type)];
        }

        case Column_Region:
        {
          return m_region_pixmaps[static_cast<u32>(ge->region)];
        }

        case Column_Compatibility:
        {
          return m_compatibility_pixmaps[static_cast<u32>(ge->compatibility)];
        }

        case Column_Cover:
        {
          QPixmap* pm = m_cover_pixmap_cache.Lookup(ge->path);
          if (pm)
            return *pm;

          // We insert the placeholder into the cache, so that we don't repeatedly
          // queue loading jobs for this game.
          const_cast<GameListModel*>(this)->loadOrGenerateCover(ge);
          return *m_cover_pixmap_cache.Insert(ge->path, m_loading_pixmap);
        }
        break;

        default:
          return {};
      }

      default:
        return {};
    }
  }
}

QVariant GameListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= Column_Count)
    return {};

  return m_column_display_names[section];
}

void GameListModel::refresh()
{
  beginResetModel();
  endResetModel();
}

bool GameListModel::titlesLessThan(int left_row, int right_row) const
{
  if (left_row < 0 || left_row >= static_cast<int>(GameList::GetEntryCount()) || right_row < 0 ||
      right_row >= static_cast<int>(GameList::GetEntryCount()))
  {
    return false;
  }

  const GameList::Entry* left = GameList::GetEntryByIndex(left_row);
  const GameList::Entry* right = GameList::GetEntryByIndex(right_row);
  return (StringUtil::Strcasecmp(left->title.c_str(), right->title.c_str()) < 0);
}

bool GameListModel::lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column) const
{
  if (!left_index.isValid() || !right_index.isValid())
    return false;

  const int left_row = left_index.row();
  const int right_row = right_index.row();
  if (left_row < 0 || left_row >= static_cast<int>(GameList::GetEntryCount()) || right_row < 0 ||
      right_row >= static_cast<int>(GameList::GetEntryCount()))
  {
    return false;
  }

  const auto lock = GameList::GetLock();
  const GameList::Entry* left = GameList::GetEntryByIndex(left_row);
  const GameList::Entry* right = GameList::GetEntryByIndex(right_row);
  if (!left || !right)
    return false;

  switch (column)
  {
    case Column_Type:
    {
      if (left->type == right->type)
        return titlesLessThan(left_row, right_row);

      return (static_cast<int>(left->type) < static_cast<int>(right->type));
    }

    case Column_Serial:
    {
      if (left->serial == right->serial)
        return titlesLessThan(left_row, right_row);
      return (StringUtil::Strcasecmp(left->serial.c_str(), right->serial.c_str()) < 0);
    }

    case Column_Title:
    {
      return titlesLessThan(left_row, right_row);
    }

    case Column_FileTitle:
    {
      const std::string_view file_title_left(Path::GetFileTitle(left->path));
      const std::string_view file_title_right(Path::GetFileTitle(right->path));
      if (file_title_left == file_title_right)
        return titlesLessThan(left_row, right_row);

      const std::size_t smallest = std::min(file_title_left.size(), file_title_right.size());
      return (StringUtil::Strncasecmp(file_title_left.data(), file_title_right.data(), smallest) < 0);
    }

    case Column_Region:
    {
      if (left->region == right->region)
        return titlesLessThan(left_row, right_row);
      return (static_cast<int>(left->region) < static_cast<int>(right->region));
    }

    case Column_Compatibility:
    {
      if (left->compatibility == right->compatibility)
        return titlesLessThan(left_row, right_row);

      return (static_cast<int>(left->compatibility) < static_cast<int>(right->compatibility));
    }

    case Column_Size:
    {
      if (left->total_size == right->total_size)
        return titlesLessThan(left_row, right_row);

      return (left->total_size < right->total_size);
    }

    case Column_Genre:
    {
      if (left->genre == right->genre)
        return titlesLessThan(left_row, right_row);
      return (StringUtil::Strcasecmp(left->genre.c_str(), right->genre.c_str()) < 0);
    }

    case Column_Developer:
    {
      if (left->developer == right->developer)
        return titlesLessThan(left_row, right_row);
      return (StringUtil::Strcasecmp(left->developer.c_str(), right->developer.c_str()) < 0);
    }

    case Column_Publisher:
    {
      if (left->publisher == right->publisher)
        return titlesLessThan(left_row, right_row);
      return (StringUtil::Strcasecmp(left->publisher.c_str(), right->publisher.c_str()) < 0);
    }

    case Column_Year:
    {
      if (left->release_date == right->release_date)
        return titlesLessThan(left_row, right_row);

      return (left->release_date < right->release_date);
    }

    case Column_Players:
    {
      u8 left_players = (left->min_players << 4) + left->max_players;
      u8 right_players = (right->min_players << 4) + right->max_players;
      if (left_players == right_players)
        return titlesLessThan(left_row, right_row);

      return (left_players < right_players);
    }

    default:
      return false;
  }
}

void GameListModel::loadCommonImages()
{
  for (u32 i = 0; i < static_cast<u32>(GameList::EntryType::Count); i++)
    m_type_pixmaps[i] = QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(i)).pixmap(QSize(24, 24));

  for (u32 i = 0; i < static_cast<u32>(DiscRegion::Count); i++)
    m_region_pixmaps[i] = QtUtils::GetIconForRegion(static_cast<DiscRegion>(i)).pixmap(42, 30);

  for (int i = 0; i < static_cast<int>(GameDatabase::CompatibilityRating::Count); i++)
    m_compatibility_pixmaps[i] = QtUtils::GetIconForCompatibility(static_cast<GameDatabase::CompatibilityRating>(i)).pixmap(96, 24);

  m_placeholder_pixmap.load(QStringLiteral("%1/images/cover-placeholder.png").arg(QtHost::GetResourcesBasePath()));
  setCoverScale(1.0f);
}

void GameListModel::setColumnDisplayNames()
{
  m_column_display_names[Column_Type] = tr("Type");
  m_column_display_names[Column_Serial] = tr("Code");
  m_column_display_names[Column_Title] = tr("Title");
  m_column_display_names[Column_FileTitle] = tr("File Title");
  m_column_display_names[Column_Developer] = tr("Developer");
  m_column_display_names[Column_Publisher] = tr("Publisher");
  m_column_display_names[Column_Genre] = tr("Genre");
  m_column_display_names[Column_Year] = tr("Year");
  m_column_display_names[Column_Players] = tr("Players");
  m_column_display_names[Column_Size] = tr("Size");
  m_column_display_names[Column_Region] = tr("Region");
  m_column_display_names[Column_Compatibility] = tr("Compatibility");
}
