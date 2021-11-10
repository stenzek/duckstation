#include "gamelistmodel.h"
#include "common/file_system.h"
#include "common/string_util.h"
#include "core/system.h"
#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPainter>

static constexpr std::array<const char*, GameListModel::Column_Count> s_column_names = {
  {"Type", "Code", "Title", "File Title", "Developer", "Publisher", "Genre", "Year", "Players", "Size", "Region",
   "Compatibility", "Cover"}};

static constexpr int COVER_ART_WIDTH = 512;
static constexpr int COVER_ART_HEIGHT = 512;
static constexpr int COVER_ART_SPACING = 32;

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

static QPixmap createPlaceholderImage(int width, int height, float scale, const std::string& title)
{
  const float dpr = qApp->devicePixelRatio();
  QPixmap pm(QStringLiteral(":/icons/cover-placeholder.png"));
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

GameListModel::GameListModel(GameList* game_list, QObject* parent /* = nullptr */)
  : QAbstractTableModel(parent), m_game_list(game_list)
{
  loadCommonImages();
  setColumnDisplayNames();
}
GameListModel::~GameListModel() = default;

void GameListModel::setCoverScale(float scale)
{
  if (m_cover_scale == scale)
    return;

  m_cover_pixmap_cache.clear();
  m_cover_scale = scale;
}

void GameListModel::refreshCovers()
{
  m_cover_pixmap_cache.clear();
  refresh();
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

  return static_cast<int>(m_game_list->GetEntryCount());
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
          const std::string_view file_title(FileSystem::GetFileTitleFromPath(ge.path));
          return QString::fromUtf8(file_title.data(), static_cast<int>(file_title.length()));
        }

        case Column_Developer:
          return QString::fromStdString(ge.developer);

        case Column_Publisher:
          return QString::fromStdString(ge.publisher);

        case Column_Genre:
          return QString::fromStdString(ge.genre);

        case Column_Year:
        {
          if (ge.release_date != 0)
          {
            return QStringLiteral("%1").arg(
              QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ge.release_date), Qt::UTC).date().year());
          }
          else
          {
            return QString();
          }
        }

        case Column_Players:
        {
          if (ge.min_players == ge.max_players)
            return QStringLiteral("%1").arg(ge.min_players);
          else
            return QStringLiteral("%1-%2").arg(ge.min_players).arg(ge.max_players);
        }

        case Column_Size:
          return QString("%1 MB").arg(static_cast<double>(ge.total_size) / 1048576.0, 0, 'f', 2);

        case Column_Cover:
        {
          if (m_show_titles_for_covers)
            return QString::fromStdString(ge.title);
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
          return static_cast<int>(ge.type);

        case Column_Code:
          return QString::fromStdString(ge.code);

        case Column_Title:
        case Column_Cover:
          return QString::fromStdString(ge.title);

        case Column_FileTitle:
        {
          const std::string_view file_title(FileSystem::GetFileTitleFromPath(ge.path));
          return QString::fromUtf8(file_title.data(), static_cast<int>(file_title.length()));
        }

        case Column_Developer:
          return QString::fromStdString(ge.developer);

        case Column_Publisher:
          return QString::fromStdString(ge.publisher);

        case Column_Genre:
          return QString::fromStdString(ge.genre);

        case Column_Year:
          return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ge.release_date), Qt::UTC).date().year();

        case Column_Players:
          return static_cast<int>(ge.max_players);

        case Column_Region:
          return static_cast<int>(ge.region);

        case Column_Compatibility:
          return static_cast<int>(ge.compatibility_rating);

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
              return ((ge.settings.GetUserSettingsCount() > 0) ? m_type_disc_with_settings_pixmap : m_type_disc_pixmap);
            case GameListEntryType::Playlist:
              return m_type_playlist_pixmap;
            case GameListEntryType::PSF:
              return m_type_psf_pixmap;
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
            case DiscRegion::Other:
              return m_region_other_pixmap;
            case DiscRegion::PAL:
            default:
              return m_region_eu_pixmap;
          }
        }

        case Column_Compatibility:
        {
          return m_compatibiliy_pixmaps[static_cast<int>(
            (ge.compatibility_rating >= GameListCompatibilityRating::Count) ? GameListCompatibilityRating::Unknown :
                                                                              ge.compatibility_rating)];
        }

        case Column_Cover:
        {
          auto it = m_cover_pixmap_cache.find(ge.path);
          if (it != m_cover_pixmap_cache.end())
            return it->second;

          QPixmap image;
          std::string path = m_game_list->GetCoverImagePathForEntry(&ge);
          if (!path.empty())
          {
            const float dpr = qApp->devicePixelRatio();
            image = QPixmap(QString::fromStdString(path));
            if (!image.isNull())
            {
              image.setDevicePixelRatio(dpr);
              resizeAndPadPixmap(&image, getCoverArtWidth(), getCoverArtHeight(), dpr);
            }
          }

          if (image.isNull())
            image = createPlaceholderImage(getCoverArtWidth(), getCoverArtHeight(), m_cover_scale, ge.title);

          m_cover_pixmap_cache.emplace(ge.path, image);
          return image;
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
  if (left_row < 0 || left_row >= static_cast<int>(m_game_list->GetEntryCount()) || right_row < 0 ||
      right_row >= static_cast<int>(m_game_list->GetEntryCount()))
  {
    return false;
  }

  const GameListEntry& left = m_game_list->GetEntries().at(left_row);
  const GameListEntry& right = m_game_list->GetEntries().at(right_row);
  return (StringUtil::Strcasecmp(left.title.c_str(), right.title.c_str()) < 0);
}

bool GameListModel::lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column) const
{
  if (!left_index.isValid() || !right_index.isValid())
    return false;

  const int left_row = left_index.row();
  const int right_row = right_index.row();
  if (left_row < 0 || left_row >= static_cast<int>(m_game_list->GetEntryCount()) || right_row < 0 ||
      right_row >= static_cast<int>(m_game_list->GetEntryCount()))
  {
    return false;
  }

  const GameListEntry& left = m_game_list->GetEntries()[left_row];
  const GameListEntry& right = m_game_list->GetEntries()[right_row];
  switch (column)
  {
    case Column_Type:
    {
      if (left.type == right.type)
        return titlesLessThan(left_row, right_row);

      return (static_cast<int>(left.type) < static_cast<int>(right.type));
    }

    case Column_Code:
    {
      if (left.code == right.code)
        return titlesLessThan(left_row, right_row);
      return (StringUtil::Strcasecmp(left.code.c_str(), right.code.c_str()) < 0);
    }

    case Column_Title:
    {
      return titlesLessThan(left_row, right_row);
    }

    case Column_FileTitle:
    {
      const std::string_view file_title_left(FileSystem::GetFileTitleFromPath(left.path));
      const std::string_view file_title_right(FileSystem::GetFileTitleFromPath(right.path));
      if (file_title_left == file_title_right)
        return titlesLessThan(left_row, right_row);

      const std::size_t smallest = std::min(file_title_left.size(), file_title_right.size());
      return (StringUtil::Strncasecmp(file_title_left.data(), file_title_right.data(), smallest) < 0);
    }

    case Column_Region:
    {
      if (left.region == right.region)
        return titlesLessThan(left_row, right_row);
      return (static_cast<int>(left.region) < static_cast<int>(right.region));
    }

    case Column_Compatibility:
    {
      if (left.compatibility_rating == right.compatibility_rating)
        return titlesLessThan(left_row, right_row);

      return (static_cast<int>(left.compatibility_rating) < static_cast<int>(right.compatibility_rating));
    }

    case Column_Size:
    {
      if (left.total_size == right.total_size)
        return titlesLessThan(left_row, right_row);

      return (left.total_size < right.total_size);
    }

    case Column_Genre:
    {
      if (left.genre == right.genre)
        return titlesLessThan(left_row, right_row);
      return (StringUtil::Strcasecmp(left.genre.c_str(), right.genre.c_str()) < 0);
    }

    case Column_Developer:
    {
      if (left.developer == right.developer)
        return titlesLessThan(left_row, right_row);
      return (StringUtil::Strcasecmp(left.developer.c_str(), right.developer.c_str()) < 0);
    }

    case Column_Publisher:
    {
      if (left.publisher == right.publisher)
        return titlesLessThan(left_row, right_row);
      return (StringUtil::Strcasecmp(left.publisher.c_str(), right.publisher.c_str()) < 0);
    }

    case Column_Year:
    {
      if (left.release_date == right.release_date)
        return titlesLessThan(left_row, right_row);

      return (left.release_date < right.release_date);
    }

    case Column_Players:
    {
      u8 left_players = (left.min_players << 4) + left.max_players;
      u8 right_players = (right.min_players << 4) + right.max_players;
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
  // TODO: Use svg instead of png
  m_type_disc_pixmap = QIcon(QStringLiteral(":/icons/media-optical-24.png")).pixmap(QSize(24, 24));
  m_type_disc_with_settings_pixmap = QIcon(QStringLiteral(":/icons/media-optical-gear-24.png")).pixmap(QSize(24, 24));
  m_type_exe_pixmap = QIcon(QStringLiteral(":/icons/applications-system-24.png")).pixmap(QSize(24, 24));
  m_type_playlist_pixmap = QIcon(QStringLiteral(":/icons/address-book-new-22.png")).pixmap(QSize(22, 22));
  m_type_psf_pixmap = QIcon(QStringLiteral(":/icons/multimedia-player.png")).pixmap(QSize(22, 22));
  m_region_eu_pixmap = QIcon(QStringLiteral(":/icons/flag-eu.png")).pixmap(QSize(42, 30));
  m_region_jp_pixmap = QIcon(QStringLiteral(":/icons/flag-jp.png")).pixmap(QSize(42, 30));
  m_region_us_pixmap = QIcon(QStringLiteral(":/icons/flag-uc.png")).pixmap(QSize(42, 30));
  m_region_other_pixmap = QIcon(QStringLiteral(":/icons/flag-other.png")).pixmap(QSize(42, 30));

  for (int i = 0; i < static_cast<int>(GameListCompatibilityRating::Count); i++)
    m_compatibiliy_pixmaps[i].load(QStringLiteral(":/icons/star-%1.png").arg(i));
}

void GameListModel::setColumnDisplayNames()
{
  m_column_display_names[Column_Type] = tr("Type");
  m_column_display_names[Column_Code] = tr("Code");
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
