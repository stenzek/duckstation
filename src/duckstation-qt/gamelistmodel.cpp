// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistmodel.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/system.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/string_util.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPainter>

static constexpr std::array<const char*, GameListModel::Column_Count> s_column_names = {
  {"Icon", "Serial", "Title", "File Title", "Developer", "Publisher", "Genre", "Year", "Players", "Time Played",
   "Last Played", "Size", "File Size", "Region", "Achievements", "Compatibility", "Cover"}};

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

GameListCoverLoader::GameListCoverLoader(const GameList::Entry* ge, const QImage& placeholder_image, int width,
                                         int height, float scale)
  : QObject(nullptr), m_path(ge->path), m_serial(ge->serial), m_title(ge->title),
    m_placeholder_image(placeholder_image), m_width(width), m_height(height), m_scale(scale),
    m_dpr(qApp->devicePixelRatio())
{
}

GameListCoverLoader::~GameListCoverLoader() = default;

void GameListCoverLoader::loadOrGenerateCover()
{
  QPixmap image;
  const std::string cover_path(GameList::GetCoverImagePath(m_path, m_serial, m_title));
  if (!cover_path.empty())
  {
    m_image.load(QString::fromStdString(cover_path));
    if (!m_image.isNull())
    {
      m_image.setDevicePixelRatio(m_dpr);
      resizeAndPadImage();
    }
  }

  if (m_image.isNull())
    createPlaceholderImage();

  // Have to pass through the UI thread, because the thread pool isn't a QThread...
  // Can't create pixmaps on the worker thread, have to create it on the UI thread.
  QtHost::RunOnUIThread([this]() {
    if (!m_image.isNull())
      emit coverLoaded(m_path, QPixmap::fromImage(m_image));
    else
      emit coverLoaded(m_path, QPixmap());
    delete this;
  });
}

void GameListCoverLoader::createPlaceholderImage()
{
  m_image = m_placeholder_image.copy();
  m_image.setDevicePixelRatio(m_dpr);
  if (m_image.isNull())
    return;

  resizeAndPadImage();

  QPainter painter;
  if (painter.begin(&m_image))
  {
    QFont font;
    font.setPointSize(std::max(static_cast<int>(32.0f * m_scale), 1));
    painter.setFont(font);
    painter.setPen(Qt::white);

    const QRect text_rc(0, 0, static_cast<int>(static_cast<float>(m_width)),
                        static_cast<int>(static_cast<float>(m_height)));
    painter.drawText(text_rc, Qt::AlignCenter | Qt::TextWordWrap, QString::fromStdString(m_title));
    painter.end();
  }
}

void GameListCoverLoader::resizeAndPadImage()
{
  const int dpr_expected_width = DPRScale(m_width, m_dpr);
  const int dpr_expected_height = DPRScale(m_height, m_dpr);
  if (m_image.width() == dpr_expected_width && m_image.height() == dpr_expected_height)
    return;

  m_image = m_image.scaled(dpr_expected_width, dpr_expected_height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  if (m_image.width() == dpr_expected_width && m_image.height() == dpr_expected_height)
    return;

  // QPainter works in unscaled coordinates.
  int xoffs = 0;
  int yoffs = 0;
  if (m_image.width() < dpr_expected_width)
    xoffs = DPRUnscale((dpr_expected_width - m_image.width()) / 2, m_dpr);
  if (m_image.height() < dpr_expected_height)
    yoffs = DPRUnscale((dpr_expected_height - m_image.height()) / 2, m_dpr);

  QPixmap padded_image(dpr_expected_width, dpr_expected_height);
  padded_image.setDevicePixelRatio(m_dpr);
  padded_image.fill(Qt::transparent);
  QPainter painter;
  if (painter.begin(&padded_image))
  {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(xoffs, yoffs, m_image);
    painter.setCompositionMode(QPainter::CompositionMode_Destination);
    painter.fillRect(padded_image.rect(), QColor(0, 0, 0, 0));
    painter.end();
  }
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

  connect(g_emu_thread, &EmuThread::gameListRowsChanged, this, &GameListModel::rowsChanged);
}

GameListModel::~GameListModel()
{
  // wait for all cover loads to finish, they're using m_placeholder_image
  System::WaitForAllAsyncTasks();
}

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
  if (m_loading_pixmap.load(QStringLiteral("%1/images/placeholder.png").arg(QtHost::GetResourcesBasePath())))
  {
    m_loading_pixmap.setDevicePixelRatio(qApp->devicePixelRatio());
    resizeAndPadPixmap(&m_loading_pixmap, getCoverArtWidth(), getCoverArtHeight(), qApp->devicePixelRatio());
  }
  else
  {
    m_loading_pixmap = QPixmap(getCoverArtWidth(), getCoverArtHeight());
    m_loading_pixmap.fill(QColor(0, 0, 0, 0));
  }

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
  // NOTE: Must get connected before queuing, because otherwise you risk a race.
  GameListCoverLoader* loader =
    new GameListCoverLoader(ge, m_placeholder_image, getCoverArtWidth(), getCoverArtHeight(), m_cover_scale);
  connect(loader, &GameListCoverLoader::coverLoaded, this, &GameListModel::coverLoaded);
  System::QueueAsyncTask([loader]() { loader->loadOrGenerateCover(); });
}

void GameListModel::coverLoaded(const std::string& path, const QPixmap& pixmap)
{
  m_cover_pixmap_cache.Insert(path, pixmap);
  invalidateCoverForPath(path);
}

void GameListModel::rowsChanged(const QList<int>& rows)
{
  const QList<int> roles_changed = {Qt::DisplayRole};

  // try to collapse multiples
  size_t start = 0;
  size_t idx = 0;
  const size_t size = rows.size();
  for (; idx < size;)
  {
    if ((idx + 1) < size && rows[idx + 1] == (rows[idx] + 1))
    {
      idx++;
    }
    else
    {
      emit dataChanged(createIndex(rows[start], 0), createIndex(rows[idx], Column_Count - 1), roles_changed);
      start = ++idx;
    }
  }
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

const QPixmap& GameListModel::getFlagPixmapForEntry(const GameList::Entry* ge) const
{
  static constexpr u32 FLAG_PIXMAP_WIDTH = 30;
  static constexpr u32 FLAG_PIXMAP_HEIGHT = 20;

  const std::string_view name = ge->GetLanguageIcon();
  auto it = m_flag_pixmap_cache.find(name);
  if (it != m_flag_pixmap_cache.end())
    return it->second;

  const QIcon icon(QString::fromStdString(QtHost::GetResourcePath(ge->GetLanguageIconName(), true)));
  it = m_flag_pixmap_cache.emplace(name, icon.pixmap(FLAG_PIXMAP_WIDTH, FLAG_PIXMAP_HEIGHT)).first;
  return it->second;
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
          return QtUtils::StringViewToQString(ge->serial);

        case Column_Title:
          return QtUtils::StringViewToQString(ge->title);

        case Column_FileTitle:
          return QtUtils::StringViewToQString(Path::GetFileTitle(ge->path));

        case Column_Developer:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->developer) : QString();

        case Column_Publisher:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->publisher) : QString();

        case Column_Genre:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->genre) : QString();

        case Column_Year:
        {
          if (ge->dbentry && ge->dbentry->release_date != 0)
          {
            return QStringLiteral("%1").arg(
              QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ge->dbentry->release_date), QTimeZone::utc())
                .date()
                .year());
          }
          else
          {
            return QString();
          }
        }

        case Column_Players:
        {
          if (!ge->dbentry || ge->dbentry->min_players == 0)
            return QString();
          else if (ge->dbentry->min_players == ge->dbentry->max_players)
            return QStringLiteral("%1").arg(ge->dbentry->min_players);
          else
            return QStringLiteral("%1-%2").arg(ge->dbentry->min_players).arg(ge->dbentry->max_players);
        }

        case Column_FileSize:
          return (ge->file_size >= 0) ?
                   QStringLiteral("%1 MB").arg(static_cast<double>(ge->file_size) / 1048576.0, 0, 'f', 2) :
                   tr("Unknown");

        case Column_UncompressedSize:
          return QStringLiteral("%1 MB").arg(static_cast<double>(ge->uncompressed_size) / 1048576.0, 0, 'f', 2);

        case Column_Achievements:
          return {};

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
      const int column = index.column();
      if (column == Column_TimePlayed || column == Column_LastPlayed)
        return Qt::DescendingOrder;
      else
        return Qt::AscendingOrder;
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
          return getFlagPixmapForEntry(ge);
        }

        case Column_Compatibility:
        {
          return m_compatibility_pixmaps[static_cast<u32>(ge->dbentry ? ge->dbentry->compatibility :
                                                                        GameDatabase::CompatibilityRating::Unknown)];
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
      const GameDatabase::CompatibilityRating left_compatibility =
        left->dbentry ? left->dbentry->compatibility : GameDatabase::CompatibilityRating::Unknown;
      const GameDatabase::CompatibilityRating right_compatibility =
        right->dbentry ? right->dbentry->compatibility : GameDatabase::CompatibilityRating::Unknown;
      if (left_compatibility == right_compatibility)
        return titlesLessThan(left, right);

      return (static_cast<int>(left_compatibility) < static_cast<int>(right_compatibility));
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
      const int compres =
        StringUtil::CompareNoCase(left->dbentry ? std::string_view(left->dbentry->genre) : std::string_view(),
                                  right->dbentry ? std::string_view(right->dbentry->genre) : std::string_view());
      return (compres == 0) ? titlesLessThan(left, right) : (compres < 0);
    }

    case Column_Developer:
    {
      const int compres =
        StringUtil::CompareNoCase(left->dbentry ? std::string_view(left->dbentry->developer) : std::string_view(),
                                  right->dbentry ? std::string_view(right->dbentry->developer) : std::string_view());
      return (compres == 0) ? titlesLessThan(left, right) : (compres < 0);
    }

    case Column_Publisher:
    {
      const int compres =
        StringUtil::CompareNoCase(left->dbentry ? std::string_view(left->dbentry->publisher) : std::string_view(),
                                  right->dbentry ? std::string_view(right->dbentry->publisher) : std::string_view());
      return (compres == 0) ? titlesLessThan(left, right) : (compres < 0);
    }

    case Column_Year:
    {
      const u64 ldate = left->dbentry ? left->dbentry->release_date : 0;
      const u64 rdate = right->dbentry ? right->dbentry->release_date : 0;
      if (ldate == rdate)
        return titlesLessThan(left, right);

      return (ldate < rdate);
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
      const u8 left_players = left->dbentry ? ((left->dbentry->min_players << 4) + left->dbentry->max_players) : 0;
      const u8 right_players = right->dbentry ? ((right->dbentry->min_players << 4) + right->dbentry->max_players) : 0;
      if (left_players == right_players)
        return titlesLessThan(left, right);

      return (left_players < right_players);
    }

    case Column_Achievements:
    {
      // sort by unlock percentage
      const float unlock_left =
        (left->num_achievements > 0) ?
          (static_cast<float>(std::max(left->unlocked_achievements, left->unlocked_achievements_hc)) /
           static_cast<float>(left->num_achievements)) :
          0;
      const float unlock_right =
        (right->num_achievements > 0) ?
          (static_cast<float>(std::max(right->unlocked_achievements, right->unlocked_achievements_hc)) /
           static_cast<float>(right->num_achievements)) :
          0;
      if (std::abs(unlock_left - unlock_right) < 0.0001f)
      {
        // order by achievement count
        if (left->num_achievements == right->num_achievements)
          return titlesLessThan(left, right);

        return (left->num_achievements < right->num_achievements);
      }

      return (unlock_left < unlock_right);
    }

    default:
      return false;
  }
}

void GameListModel::loadThemeSpecificImages()
{
  for (u32 i = 0; i < static_cast<u32>(GameList::EntryType::Count); i++)
    m_type_pixmaps[i] = QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(i)).pixmap(QSize(24, 24));
}

void GameListModel::loadCommonImages()
{
  loadThemeSpecificImages();

  for (int i = 0; i < static_cast<int>(GameDatabase::CompatibilityRating::Count); i++)
  {
    m_compatibility_pixmaps[i] =
      QtUtils::GetIconForCompatibility(static_cast<GameDatabase::CompatibilityRating>(i)).pixmap(96, 24);
  }

  m_placeholder_image.load(QStringLiteral("%1/images/cover-placeholder.png").arg(QtHost::GetResourcesBasePath()));

  constexpr int ACHIEVEMENT_ICON_SIZE = 16;
  m_no_achievements_pixmap = QIcon(QString::fromStdString(QtHost::GetResourcePath("images/trophy-icon-gray.svg", true)))
                               .pixmap(ACHIEVEMENT_ICON_SIZE);
  m_has_achievements_pixmap = QIcon(QString::fromStdString(QtHost::GetResourcePath("images/trophy-icon.svg", true)))
                                .pixmap(ACHIEVEMENT_ICON_SIZE);
  m_mastered_achievements_pixmap =
    QIcon(QString::fromStdString(QtHost::GetResourcePath("images/trophy-icon-star.svg", true)))
      .pixmap(ACHIEVEMENT_ICON_SIZE);
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
  m_column_display_names[Column_Achievements] = tr("Achievements");
  m_column_display_names[Column_TimePlayed] = tr("Time Played");
  m_column_display_names[Column_LastPlayed] = tr("Last Played");
  m_column_display_names[Column_FileSize] = tr("Size");
  m_column_display_names[Column_UncompressedSize] = tr("Raw Size");
  m_column_display_names[Column_Region] = tr("Region");
  m_column_display_names[Column_Compatibility] = tr("Compatibility");
}
