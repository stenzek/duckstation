// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistmodel.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/system.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/string_util.h"

#include <QtConcurrent/QtConcurrent>
#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPainter>

static constexpr std::array<const char*, GameListModel::Column_Count> s_column_names = {
  {"Icon", "Serial", "Title", "File Title", "Developer", "Publisher", "Genre", "Year", "Players", "Time Played",
   "Last Played", "Size", "File Size", "Region", "Compatibility", "Cover"}};

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

GameListModel::GameListModel(float cover_scale, bool show_cover_titles, bool show_game_icons,
                             QObject* parent /* = nullptr */)
  : QAbstractTableModel(parent), m_show_titles_for_covers(show_cover_titles), m_show_game_icons(show_game_icons),
    m_memcard_pixmap_cache(128)
{
  loadCommonImages();
  setCoverScale(cover_scale);
  setColumnDisplayNames();

  if (m_show_game_icons)
    GameList::ReloadMemcardTimestampCache();
}

GameListModel::~GameListModel() = default;

void GameListModel::setShowGameIcons(bool enabled)
{
  m_show_game_icons = enabled;

  beginResetModel();
  m_memcard_pixmap_cache.Clear();
  if (enabled)
    GameList::ReloadMemcardTimestampCache();
  endResetModel();
}

void GameListModel::setCoverScale(float scale)
{
  if (m_cover_scale == scale)
    return;

  m_cover_pixmap_cache.Clear();
  m_cover_scale = scale;
  m_loading_pixmap = QPixmap(getCoverArtWidth(), getCoverArtHeight());
  m_loading_pixmap.fill(QColor(0, 0, 0, 0));

  emit coverScaleChanged();
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

void GameListModel::reloadThemeSpecificImages()
{
  loadThemeSpecificImages();
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
  std::optional<u32> row;
  if (hasTakenGameList())
  {
    for (u32 i = 0; i < static_cast<u32>(m_taken_entries->size()); i++)
    {
      if (path == m_taken_entries.value()[i].path)
      {
        row = i;
        break;
      }
    }
  }
  else
  {
    // This isn't ideal, but not sure how else we can get the row, when it might change while scanning...
    auto lock = GameList::GetLock();
    const u32 count = GameList::GetEntryCount();
    for (u32 i = 0; i < count; i++)
    {
      if (GameList::GetEntryByIndex(i)->path == path)
      {
        row = i;
        break;
      }
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

QString GameListModel::formatTimespan(time_t timespan)
{
  // avoid an extra string conversion
  const u32 hours = static_cast<u32>(timespan / 3600);
  const u32 minutes = static_cast<u32>((timespan % 3600) / 60);
  if (hours > 0)
    return qApp->translate("GameList", "%n hours", "", hours);
  else
    return qApp->translate("GameList", "%n minutes", "", minutes);
}

const QPixmap& GameListModel::getIconPixmapForEntry(const GameList::Entry* ge) const
{
  // We only do this for discs/disc sets for now.
  if (m_show_game_icons && (!ge->serial.empty() && (ge->IsDisc() || ge->IsDiscSet())))
  {
    QPixmap* item = m_memcard_pixmap_cache.Lookup(ge->serial);
    if (item)
      return *item;

    // Assumes game list lock is held.
    const std::string path = GameList::GetGameIconPath(ge->serial, ge->path);
    QPixmap pm;
    if (!path.empty() && pm.load(QString::fromStdString(path)))
    {
      fixIconPixmapSize(pm);
      return *m_memcard_pixmap_cache.Insert(ge->serial, std::move(pm));
    }

    return *m_memcard_pixmap_cache.Insert(ge->serial, m_type_pixmaps[static_cast<u32>(ge->type)]);
  }

  return m_type_pixmaps[static_cast<u32>(ge->type)];
}

QIcon GameListModel::getIconForGame(const QString& path)
{
  QIcon ret;

  if (m_show_game_icons)
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryForPath(path.toStdString());

    // See above.
    if (entry && !entry->serial.empty() && (entry->IsDisc() || entry->IsDiscSet()))
    {
      const std::string icon_path = GameList::GetGameIconPath(entry->serial, entry->path);
      if (!icon_path.empty())
      {
        QPixmap newpm;
        if (!icon_path.empty() && newpm.load(QString::fromStdString(icon_path)))
        {
          fixIconPixmapSize(newpm);
          ret = QIcon(*m_memcard_pixmap_cache.Insert(entry->serial, std::move(newpm)));
        }
      }
    }
  }

  return ret;
}

void GameListModel::fixIconPixmapSize(QPixmap& pm)
{
  const qreal dpr = pm.devicePixelRatio();
  const int width = static_cast<int>(static_cast<float>(pm.width()) * dpr);
  const int height = static_cast<int>(static_cast<float>(pm.height()) * dpr);
  const int max_dim = std::max(width, height);
  if (max_dim == 16)
    return;

  const float wanted_dpr = qApp->devicePixelRatio();
  pm.setDevicePixelRatio(wanted_dpr);

  const float scale = static_cast<float>(max_dim) / 16.0f / wanted_dpr;
  const int new_width = static_cast<int>(static_cast<float>(width) / scale);
  const int new_height = static_cast<int>(static_cast<float>(height) / scale);
  pm = pm.scaled(new_width, new_height);
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
  if (parent.isValid()) [[unlikely]]
    return 0;

  if (m_taken_entries.has_value())
    return static_cast<int>(m_taken_entries->size());

  const auto lock = GameList::GetLock();
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
  if (!index.isValid()) [[unlikely]]
    return {};

  const int row = index.row();
  DebugAssert(row >= 0);

  if (m_taken_entries.has_value()) [[unlikely]]
  {
    if (static_cast<u32>(row) >= m_taken_entries->size())
      return {};

    return data(index, role, &m_taken_entries.value()[row]);
  }
  else
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* ge = GameList::GetEntryByIndex(static_cast<u32>(row));
    if (!ge)
      return {};

    return data(index, role, ge);
  }
}

QVariant GameListModel::data(const QModelIndex& index, int role, const GameList::Entry* ge) const
{
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

        case Column_FileSize:
          return (ge->file_size >= 0) ?
                   QString("%1 MB").arg(static_cast<double>(ge->file_size) / 1048576.0, 0, 'f', 2) :
                   tr("Unknown");

        case Column_UncompressedSize:
          return QString("%1 MB").arg(static_cast<double>(ge->uncompressed_size) / 1048576.0, 0, 'f', 2);

        case Column_TimePlayed:
        {
          if (ge->total_played_time == 0)
            return {};
          else
            return formatTimespan(ge->total_played_time);
        }

        case Column_LastPlayed:
          return QtUtils::StringViewToQString(GameList::FormatTimestamp(ge->last_played_time));

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
        case Column_Icon:
          return static_cast<int>(ge->GetSortType());

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

        case Column_TimePlayed:
          return static_cast<qlonglong>(ge->total_played_time);

        case Column_LastPlayed:
          return static_cast<qlonglong>(ge->last_played_time);

        case Column_FileSize:
          return static_cast<qulonglong>(ge->file_size);

        case Column_UncompressedSize:
          return static_cast<qulonglong>(ge->uncompressed_size);

        default:
          return {};
      }
    }

    case Qt::DecorationRole:
    {
      switch (index.column())
      {
        case Column_Icon:
        {
          return getIconPixmapForEntry(ge);
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

bool GameListModel::hasTakenGameList() const
{
  return m_taken_entries.has_value();
}

void GameListModel::takeGameList()
{
  const auto lock = GameList::GetLock();
  m_taken_entries = GameList::TakeEntryList();

  // If it's empty (e.g. first boot), don't use it.
  if (m_taken_entries->empty())
    m_taken_entries.reset();
}

void GameListModel::refresh()
{
  beginResetModel();

  m_taken_entries.reset();

  // Invalidate memcard LRU cache, forcing a re-query of the memcard timestamps.
  m_memcard_pixmap_cache.Clear();

  endResetModel();
}

bool GameListModel::titlesLessThan(const GameList::Entry* left, const GameList::Entry* right) const
{
  return (StringUtil::Strcasecmp(left->title.c_str(), right->title.c_str()) < 0);
}

bool GameListModel::lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column) const
{
  if (!left_index.isValid() || !right_index.isValid())
    return false;

  const int left_row = left_index.row();
  const int right_row = right_index.row();

  if (m_taken_entries.has_value()) [[unlikely]]
  {
    const GameList::Entry* left =
      (static_cast<u32>(left_row) < m_taken_entries->size()) ? &m_taken_entries.value()[left_row] : nullptr;
    const GameList::Entry* right =
      (static_cast<u32>(right_row) < m_taken_entries->size()) ? &m_taken_entries.value()[right_row] : nullptr;
    if (!left || !right)
      return false;

    return lessThan(left, right, column);
  }
  else
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* left = GameList::GetEntryByIndex(left_row);
    const GameList::Entry* right = GameList::GetEntryByIndex(right_row);
    if (!left || !right)
      return false;

    return lessThan(left, right, column);
  }
}

bool GameListModel::lessThan(const GameList::Entry* left, const GameList::Entry* right, int column) const
{
  switch (column)
  {
    case Column_Icon:
    {
      const GameList::EntryType lst = left->GetSortType();
      const GameList::EntryType rst = right->GetSortType();
      if (lst == rst)
        return titlesLessThan(left, right);

      return (static_cast<int>(lst) < static_cast<int>(rst));
    }

    case Column_Serial:
    {
      if (left->serial == right->serial)
        return titlesLessThan(left, right);
      return (StringUtil::Strcasecmp(left->serial.c_str(), right->serial.c_str()) < 0);
    }

    case Column_Title:
    {
      return titlesLessThan(left, right);
    }

    case Column_FileTitle:
    {
      const std::string_view file_title_left = Path::GetFileTitle(left->path);
      const std::string_view file_title_right = Path::GetFileTitle(right->path);
      if (file_title_left == file_title_right)
        return titlesLessThan(left, right);

      const std::size_t smallest = std::min(file_title_left.size(), file_title_right.size());
      return (StringUtil::Strncasecmp(file_title_left.data(), file_title_right.data(), smallest) < 0);
    }

    case Column_Region:
    {
      if (left->region == right->region)
        return titlesLessThan(left, right);
      return (static_cast<int>(left->region) < static_cast<int>(right->region));
    }

    case Column_Compatibility:
    {
      if (left->compatibility == right->compatibility)
        return titlesLessThan(left, right);

      return (static_cast<int>(left->compatibility) < static_cast<int>(right->compatibility));
    }

    case Column_FileSize:
    {
      if (left->file_size == right->file_size)
        return titlesLessThan(left, right);

      return (left->file_size < right->file_size);
    }

    case Column_UncompressedSize:
    {
      if (left->uncompressed_size == right->uncompressed_size)
        return titlesLessThan(left, right);

      return (left->uncompressed_size < right->uncompressed_size);
    }

    case Column_Genre:
    {
      if (left->genre == right->genre)
        return titlesLessThan(left, right);
      return (StringUtil::Strcasecmp(left->genre.c_str(), right->genre.c_str()) < 0);
    }

    case Column_Developer:
    {
      if (left->developer == right->developer)
        return titlesLessThan(left, right);
      return (StringUtil::Strcasecmp(left->developer.c_str(), right->developer.c_str()) < 0);
    }

    case Column_Publisher:
    {
      if (left->publisher == right->publisher)
        return titlesLessThan(left, right);
      return (StringUtil::Strcasecmp(left->publisher.c_str(), right->publisher.c_str()) < 0);
    }

    case Column_Year:
    {
      if (left->release_date == right->release_date)
        return titlesLessThan(left, right);

      return (left->release_date < right->release_date);
    }

    case Column_TimePlayed:
    {
      if (left->total_played_time == right->total_played_time)
        return titlesLessThan(left, right);

      return (left->total_played_time < right->total_played_time);
    }

    case Column_LastPlayed:
    {
      if (left->last_played_time == right->last_played_time)
        return titlesLessThan(left, right);

      return (left->last_played_time < right->last_played_time);
    }

    case Column_Players:
    {
      u8 left_players = (left->min_players << 4) + left->max_players;
      u8 right_players = (right->min_players << 4) + right->max_players;
      if (left_players == right_players)
        return titlesLessThan(left, right);

      return (left_players < right_players);
    }

    default:
      return false;
  }
}

void GameListModel::loadThemeSpecificImages()
{
  for (u32 i = 0; i < static_cast<u32>(GameList::EntryType::Count); i++)
    m_type_pixmaps[i] = QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(i)).pixmap(QSize(24, 24));

  for (u32 i = 0; i < static_cast<u32>(DiscRegion::Count); i++)
    m_region_pixmaps[i] = QtUtils::GetIconForRegion(static_cast<DiscRegion>(i)).pixmap(42, 30);
}

void GameListModel::loadCommonImages()
{
  loadThemeSpecificImages();

  for (int i = 0; i < static_cast<int>(GameDatabase::CompatibilityRating::Count); i++)
  {
    m_compatibility_pixmaps[i] =
      QtUtils::GetIconForCompatibility(static_cast<GameDatabase::CompatibilityRating>(i)).pixmap(96, 24);
  }

  m_placeholder_pixmap.load(QStringLiteral("%1/images/cover-placeholder.png").arg(QtHost::GetResourcesBasePath()));
}

void GameListModel::setColumnDisplayNames()
{
  m_column_display_names[Column_Icon] = tr("Icon");
  m_column_display_names[Column_Serial] = tr("Serial");
  m_column_display_names[Column_Title] = tr("Title");
  m_column_display_names[Column_FileTitle] = tr("File Title");
  m_column_display_names[Column_Developer] = tr("Developer");
  m_column_display_names[Column_Publisher] = tr("Publisher");
  m_column_display_names[Column_Genre] = tr("Genre");
  m_column_display_names[Column_Year] = tr("Year");
  m_column_display_names[Column_Players] = tr("Players");
  m_column_display_names[Column_TimePlayed] = tr("Time Played");
  m_column_display_names[Column_LastPlayed] = tr("Last Played");
  m_column_display_names[Column_FileSize] = tr("Size");
  m_column_display_names[Column_UncompressedSize] = tr("Raw Size");
  m_column_display_names[Column_Region] = tr("Region");
  m_column_display_names[Column_Compatibility] = tr("Compatibility");
}
