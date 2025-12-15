// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistwidget.h"
#include "gamelistrefreshthread.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "settingswindow.h"

#include "core/achievements.h"
#include "core/fullscreenui.h"
#include "core/game_list.h"
#include "core/host.h"
#include "core/settings.h"
#include "core/system.h"

#include "util/animated_image.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <QtCore/QDate>
#include <QtCore/QDateTime>
#include <QtCore/QSortFilterProxyModel>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QToolTip>
#include <algorithm>
#include <limits>

#include "moc_gamelistwidget.cpp"

LOG_CHANNEL(GameList);

static constexpr int VIEW_MODE_LIST = 0;
static constexpr int VIEW_MODE_GRID = 1;
static constexpr int VIEW_MODE_NO_GAMES = 2;

static constexpr int GAME_ICON_MIN_SIZE = 16;
static constexpr int GAME_ICON_DEFAULT_SIZE = 16;
static constexpr int GAME_ICON_MAX_SIZE = 80;
static constexpr int GAME_ICON_SIZE_STEP = 4;
static constexpr int GAME_ICON_PADDING = 12;
static constexpr int GAME_ICON_ANIMATION_LOOPS = 5;

static constexpr float MIN_COVER_SCALE = 0.1f;
static constexpr float DEFAULT_COVER_SCALE = 0.45f;
static constexpr float MAX_COVER_SCALE = 2.0f;
static constexpr float COVER_SCALE_STEP = 0.05f;
static constexpr int COVER_ART_SIZE = 512;
static constexpr int COVER_ART_SPACING = 32;
static constexpr int MIN_COVER_CACHE_SIZE = 256;
static constexpr int MIN_COVER_CACHE_ROW_BUFFER = 4;

static constexpr QSize FLAG_PIXMAP_SIZE(30, 20);
static constexpr QSize ACHIEVEMENT_PIXMAP_SIZE(16, 16);
static constexpr QSize COMPATIBILITY_PIXMAP_SIZE(96, 24);

static const char* SUPPORTED_FORMATS_STRING =
  QT_TRANSLATE_NOOP(GameListWidget, ".cue (Cue Sheets)\n"
                                    ".iso/.img (Single Track Image)\n"
                                    ".ecm (Error Code Modeling Image)\n"
                                    ".mds (Media Descriptor Sidecar)\n"
                                    ".chd (Compressed Hunks of Data)\n"
                                    ".pbp (PlayStation Portable, Only Decrypted)");

static constexpr std::array<const char*, GameListModel::Column_Count> s_column_names = {{
  QT_TRANSLATE_NOOP("GameListModel", "Icon"), QT_TRANSLATE_NOOP("GameListModel", "Serial"),
  QT_TRANSLATE_NOOP("GameListModel", "Title"), QT_TRANSLATE_NOOP("GameListModel", "File Title"),
  QT_TRANSLATE_NOOP("GameListModel", "Developer"), QT_TRANSLATE_NOOP("GameListModel", "Publisher"),
  QT_TRANSLATE_NOOP("GameListModel", "Genre"), QT_TRANSLATE_NOOP("GameListModel", "Year"),
  QT_TRANSLATE_NOOP("GameListModel", "Players"), QT_TRANSLATE_NOOP("GameListModel", "Time Played"),
  QT_TRANSLATE_NOOP("GameListModel", "Last Played"), QT_TRANSLATE_NOOP("GameListModel", "Size"),
  QT_TRANSLATE_NOOP("GameListModel", "Data Size"), QT_TRANSLATE_NOOP("GameListModel", "Region"),
  QT_TRANSLATE_NOOP("GameListModel", "Achievements"), QT_TRANSLATE_NOOP("GameListModel", "Compatibility"),
  "Cover", // Do not translate.
}};

static void resizeImage(QImage* image, const QSize& expected_size)
{
  // Get source image in RGB32 format for QPainter.
  const QImage::Format original_format = image->format();
  const QImage::Format expected_format =
    image->hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
  if (original_format != expected_format)
    *image = image->convertToFormat(expected_format);

  if (image->size() == expected_size)
    return;

  if ((static_cast<float>(image->width()) / static_cast<float>(image->height())) >=
      (static_cast<float>(expected_size.width()) / static_cast<float>(expected_size.height())))
  {
    *image = image->scaledToWidth(expected_size.width(), Qt::SmoothTransformation);
  }
  else
  {
    *image = image->scaledToHeight(expected_size.height(), Qt::SmoothTransformation);
  }
}

static void resizeAndPadImage(QImage* image, const QSize& expected_size, bool fill_with_top_left, bool expand_to_fill)
{
  // Get source image in RGB32 format for QPainter.
  // fill_with_top_left is used for the game list background, which cannot be transparent.
  const QImage::Format original_format = image->format();
  const QImage::Format expected_format =
    (image->hasAlphaChannel() && !fill_with_top_left) ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
  if (original_format != expected_format)
    *image = image->convertToFormat(expected_format);

  if (image->size() == expected_size)
    return;

  if (((static_cast<float>(image->width()) / static_cast<float>(image->height())) >=
       (static_cast<float>(expected_size.width()) / static_cast<float>(expected_size.height()))) != expand_to_fill)
  {
    *image = image->scaledToWidth(expected_size.width(), Qt::SmoothTransformation);
  }
  else
  {
    *image = image->scaledToHeight(expected_size.height(), Qt::SmoothTransformation);
  }

  if (image->size() == expected_size)
    return;

  // QPainter works in unscaled coordinates.
  int xoffs = 0;
  int yoffs = 0;
  const int image_width = image->width();
  const int image_height = image->height();
  if ((image_width < expected_size.width()) != expand_to_fill)
  {
    xoffs = static_cast<int>(static_cast<qreal>((expected_size.width() - image_width) / 2) / image->devicePixelRatio());
  }
  if ((image_height < expected_size.height()) != expand_to_fill)
  {
    yoffs =
      static_cast<int>(static_cast<qreal>((expected_size.height() - image_height) / 2) / image->devicePixelRatio());
  }

  QImage padded_image(expected_size, fill_with_top_left ? expected_format : QImage::Format_ARGB32_Premultiplied);
  padded_image.setDevicePixelRatio(image->devicePixelRatio());
  if (fill_with_top_left)
    padded_image.fill(image->pixel(0, 0));
  else
    padded_image.fill(Qt::transparent);

  QPainter painter;
  if (painter.begin(&padded_image))
  {
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(xoffs, yoffs, *image);
    painter.end();
  }

  *image = std::move(padded_image);
}

static void fastResizePixmap(QPixmap& pm, const QSize& expected_size)
{
  if (pm.size() == expected_size)
    return;

  if ((static_cast<float>(pm.width()) / static_cast<float>(pm.height())) >=
      (static_cast<float>(expected_size.width()) / static_cast<float>(expected_size.height())))
  {
    pm = pm.scaledToWidth(expected_size.width(), Qt::FastTransformation);
  }
  else
  {
    pm = pm.scaledToHeight(expected_size.height(), Qt::FastTransformation);
  }
}

static void resizeGameIcon(QPixmap& pm, int icon_size)
{
  const int pm_width = pm.width();
  const int pm_height = pm.height();

  // Manually transforms, we want to truncate this size, not round.
  const qreal scale = (static_cast<qreal>(icon_size) / static_cast<qreal>(pm_width)) * pm.devicePixelRatio();
  const int scaled_pm_width = static_cast<int>(static_cast<qreal>(pm_width) * scale);
  const int scaled_pm_height = static_cast<int>(static_cast<qreal>(pm_height) * scale);

  if (pm_width != scaled_pm_width || pm_height != scaled_pm_height)
    QtUtils::ResizeSharpBilinear(pm, std::max(scaled_pm_width, scaled_pm_height), pm_width);
}

static QString sizeToString(s64 size)
{
  static constexpr s64 one_mb = 1024 * 1024;
  return (size >= 0) ? QStringLiteral("%1 MB").arg((size + (one_mb - 1)) / one_mb) :
                       qApp->translate("GameListModel", "Unknown");
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

GameListModel::GameListModel(GameListWidget* parent)
  : QAbstractTableModel(parent), m_device_pixel_ratio(parent->devicePixelRatio()),
    m_icon_pixmap_cache(MIN_COVER_CACHE_SIZE)
{
  m_cover_scale = Host::GetBaseFloatSettingValue("UI", "GameListCoverArtScale", DEFAULT_COVER_SCALE);
  m_icon_size = Host::GetBaseIntSettingValue("UI", "GameListIconSize", GAME_ICON_DEFAULT_SIZE);
  m_show_localized_titles = GameList::ShouldShowLocalizedTitles();
  m_show_titles_for_covers = Host::GetBaseBoolSettingValue("UI", "GameListShowCoverTitles", true);
  m_show_game_icons = Host::GetBaseBoolSettingValue("UI", "GameListShowGameIcons", true);

  loadCommonImages();
  loadCoverScaleDependentPixmaps();

  if (m_show_game_icons)
    GameList::ReloadMemcardTimestampCache();

  connect(g_emu_thread, &EmuThread::gameListRowsChanged, this, &GameListModel::rowsChanged);
}

GameListModel::~GameListModel() = default;

bool GameListModel::getShowLocalizedTitles() const
{
  return m_show_localized_titles;
}

void GameListModel::setShowLocalizedTitles(bool enabled)
{
  m_show_localized_titles = enabled;
  invalidateColumn(Column_Title);
}

bool GameListModel::getShowCoverTitles() const
{
  return m_show_titles_for_covers;
}

void GameListModel::setShowCoverTitles(bool enabled)
{
  m_show_titles_for_covers = enabled;
  emit dataChanged(index(0, Column_Cover), index(rowCount() - 1, Column_Cover), {Qt::DisplayRole});
}

int GameListModel::getRowHeight() const
{
  return getIconSizeWithPadding();
}

int GameListModel::getIconSize() const
{
  return m_icon_size;
}

int GameListModel::getIconSizeWithPadding() const
{
  return m_icon_size + GAME_ICON_PADDING;
}

void GameListModel::setIconSize(int size)
{
  if (m_icon_size == size)
    return;

  m_icon_size = size;

  Host::SetBaseIntSettingValue("UI", "GameListIconSize", size);
  Host::CommitBaseSettingChanges();

  emit iconSizeChanged(m_icon_size);

  // Might look odd, but this is needed to force the section sizes to invalidate
  // after we change them in the list view in the iconSizeChanged() handler.
  emit headerDataChanged(Qt::Vertical, 0, rowCount() - 1);

  loadSizeDependentPixmaps();
  invalidateColumn(Column_Icon);
}

float GameListModel::getCoverScale() const
{
  return m_cover_scale;
}

void GameListModel::setCoverScale(float scale)
{
  if (m_cover_scale == scale)
    return;

  m_cover_scale = scale;

  // Invalidate all pending loads. This stops the case where the user changes the scale,
  // then quickly changes back before the loads finish, resulting in the image never loading.
  m_cover_pixmap_cache.Apply(
    [](const std::string&, CoverPixmapCacheEntry& entry) { entry.scale = entry.is_loading ? 0.0f : entry.scale; });

  Host::SetBaseFloatSettingValue("UI", "GameListCoverArtScale", scale);
  Host::CommitBaseSettingChanges();
  updateCoverScale();
}

void GameListModel::loadCoverScaleDependentPixmaps()
{
  QImage loading_image;
  if (loading_image.load(QtHost::GetResourceQPath("images/placeholder.png", true)))
  {
    loading_image.setDevicePixelRatio(m_device_pixel_ratio);
    resizeImage(&loading_image, getDeviceScaledCoverArtSize());
  }
  else
  {
    loading_image = QImage(getDeviceScaledCoverArtSize(), QImage::Format_RGB32);
    loading_image.setDevicePixelRatio(m_device_pixel_ratio);
    loading_image.fill(QColor(0, 0, 0, 0));
  }
  m_loading_pixmap = QPixmap::fromImage(loading_image);

  m_placeholder_image = QImage();
  if (m_placeholder_image.load(QtHost::GetResourceQPath("images/cover-placeholder.png", true)))
  {
    m_placeholder_image.setDevicePixelRatio(m_device_pixel_ratio);
    resizeImage(&m_placeholder_image, getDeviceScaledCoverArtSize());
  }
  else
  {
    m_placeholder_image = QImage(getDeviceScaledCoverArtSize(), QImage::Format_RGB32);
    m_placeholder_image.setDevicePixelRatio(m_device_pixel_ratio);
    m_placeholder_image.fill(QColor(0, 0, 0, 0));
  }
}

void GameListModel::updateCoverScale()
{
  loadCoverScaleDependentPixmaps();
  emit coverScaleChanged(m_cover_scale);
  emit dataChanged(index(0, Column_Cover), index(rowCount() - 1, Column_Cover),
                   {Qt::DecorationRole, Qt::FontRole, Qt::SizeHintRole});
}

void GameListModel::updateCacheSize(int num_rows, int num_columns, QSortFilterProxyModel* const sort_model,
                                    int top_left_row)
{
  // Add additional buffer zone to the rows, since Qt will grab them early when scrolling.
  const int num_items = (num_rows + MIN_COVER_CACHE_ROW_BUFFER) * num_columns;

  // This is a bit conversative, since it doesn't consider padding, but better to be over than under.
  const int cache_size = std::max(num_items, MIN_COVER_CACHE_SIZE);
  if (static_cast<u32>(cache_size) < m_cover_pixmap_cache.GetSize())
  {
    // If we're removing items, make sure we try to keep those currently in view in the cache.
    // Otherwise we'll have flicker of the loading pixmap as they get evicted and reloaded.
    const auto lock = GameList::GetLock();
    for (int row = 0; row < cache_size; row++)
    {
      const QModelIndex real_index = sort_model->mapToSource(sort_model->index(top_left_row + row, Column_Cover));
      if (!real_index.isValid())
        continue;

      const GameList::Entry* ge = GameList::GetEntryByIndex(static_cast<u32>(real_index.row()));
      if (ge)
        m_cover_pixmap_cache.Lookup(ge->path);
    }
  }

  m_cover_pixmap_cache.SetMaxCapacity(static_cast<u32>(cache_size));
}

qreal GameListModel::getDevicePixelRatio() const
{
  return m_device_pixel_ratio;
}

void GameListModel::setDevicePixelRatio(qreal dpr)
{
  if (m_device_pixel_ratio == dpr)
    return;

  m_device_pixel_ratio = dpr;
  m_placeholder_image.setDevicePixelRatio(dpr);
  m_loading_pixmap.setDevicePixelRatio(dpr);
  m_flag_pixmap_cache.clear();
  loadCommonImages();
  loadCoverScaleDependentPixmaps();
  invalidateColumn(Column_Icon);
  invalidateColumn(Column_Cover);
}

void GameListModel::reloadThemeSpecificImages()
{
  loadSizeDependentPixmaps();
  refresh();
}

void GameListModel::loadOrGenerateCover(const GameList::Entry* ge)
{
  QtAsyncTask::create(this, [path = ge->path, serial = ge->serial, save_title = std::string(ge->GetSaveTitle()),
                             display_title = QtUtils::StringViewToQString(ge->GetDisplayTitle(m_show_localized_titles)),
                             placeholder_image = m_placeholder_image, list = this, size = getDeviceScaledCoverArtSize(),
                             scale = m_cover_scale, dpr = m_device_pixel_ratio,
                             is_custom_title = ge->has_custom_title]() mutable {
    QImage image;
    loadOrGenerateCover(image, placeholder_image, size, scale, dpr, path, serial, save_title, display_title,
                        is_custom_title);
    return [path = std::move(path), image = std::move(image), list, scale]() { list->coverLoaded(path, image, scale); };
  });
}

void GameListModel::createPlaceholderImage(QImage& image, const QImage& placeholder_image, const QSize& size,
                                           float scale, const QString& title)
{
  image = placeholder_image.copy();
  if (image.isNull())
    return;

  resizeImage(&image, size);

  QPainter painter;
  if (painter.begin(&image))
  {
    QFont font;
    font.setPixelSize(std::max(static_cast<int>(64.0f * scale), 1));
    font.setFamilies(QtHost::GetRobotoFontFamilies());
    painter.setFont(font);

    const int margin = static_cast<int>(30.0f * scale);
    const QSize unscaled_size = QtUtils::GetDeviceIndependentSize(image.size(), image.devicePixelRatio());
    const QRect text_rc(margin, margin, static_cast<int>(static_cast<float>(unscaled_size.width() - margin - margin)),
                        static_cast<int>(static_cast<float>(unscaled_size.height() - margin - margin)));

    // draw shadow first
    painter.setPen(QColor(0, 0, 0, 160)); // semi-transparent black
    painter.drawText(text_rc.translated(1, 1), Qt::AlignCenter | Qt::TextWordWrap, title);

    painter.setPen(Qt::white);
    painter.drawText(text_rc, Qt::AlignCenter | Qt::TextWordWrap, title);

    painter.end();
  }
}

void GameListModel::loadOrGenerateCover(QImage& image, const QImage& placeholder_image, const QSize& size, float scale,
                                        qreal dpr, const std::string& path, const std::string& serial,
                                        const std::string& save_title, const QString& display_title,
                                        bool is_custom_title)
{
  const std::string cover_path = GameList::GetCoverImagePath(path, serial, save_title, is_custom_title);
  if (!cover_path.empty())
  {
    image.load(QString::fromStdString(cover_path));
    if (!image.isNull())
    {
      image.setDevicePixelRatio(dpr);
      resizeImage(&image, size);
    }
  }

  if (image.isNull())
    createPlaceholderImage(image, placeholder_image, size, scale, display_title);
}

void GameListModel::coverLoaded(const std::string& path, const QImage& image, float scale)
{
  // old request before cover scale change?
  CoverPixmapCacheEntry* pmp;
  if (m_cover_scale != scale || image.isNull() || !(pmp = m_cover_pixmap_cache.Lookup(path)))
    return;

  pmp->pixmap = QPixmap::fromImage(image);
  pmp->scale = scale;
  pmp->is_loading = false;

  invalidateColumnForPath(path, Column_Cover, false);
}

void GameListModel::rowsChanged(const QList<int>& rows)
{
  QList<int> roles_changed{Qt::DisplayRole, Qt::ToolTipRole};
  if (m_show_game_icons)
    roles_changed.append(Qt::DecorationRole);

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
      emit dataChanged(index(rows[start], 0), index(rows[idx], Column_Count - 1), roles_changed);
      start = ++idx;
    }
  }
}

QList<int> GameListModel::getRolesToInvalidate(int column)
{
  QList<int> ret;

  if (column == Column_Icon || column == Column_Cover || column == Column_Region)
    ret = {Qt::DecorationRole};
  else
    ret = {Qt::DisplayRole, Qt::ToolTipRole};

  return ret;
}

void GameListModel::invalidateColumn(int column, bool invalidate_cache /* = true */)
{
  if (invalidate_cache)
  {
    if (column == Column_Icon)
    {
      m_icon_pixmap_cache.Clear();
    }
    else if (column == Column_Cover)
    {
      m_cover_pixmap_cache.Clear();
      if (QtHost::IsFullscreenUIStarted())
        Host::RunOnCPUThread([]() { FullscreenUI::InvalidateCoverCache(); });
    }
    else if (column == Column_Title)
    {
      // if we're changing the title, the generated cover could change
      invalidateColumn(Column_Cover, invalidate_cache);
    }
  }

  emit dataChanged(index(0, column), index(rowCount() - 1, column), getRolesToInvalidate(column));
}

void GameListModel::invalidateColumnForPath(const std::string& path, int column, bool invalidate_cache /* = true */)
{
  // if we're changing the title, the cover could change
  if (column == Column_Title)
    invalidateColumnForPath(path, Column_Cover, invalidate_cache);

  if (invalidate_cache)
  {
    if (column == Column_Icon)
    {
      m_icon_pixmap_cache.Remove(path);
    }
    else if (column == Column_Cover)
    {
      m_cover_pixmap_cache.Remove(path);
      if (QtHost::IsFullscreenUIStarted())
        Host::RunOnCPUThread([path]() mutable { FullscreenUI::InvalidateCoverCache(std::move(path)); });
    }
  }

  const auto remove_entry = [this, &column](const GameList::Entry* ge, int row) {
    const QModelIndex mi(index(row, column));
    emit dataChanged(mi, mi, getRolesToInvalidate(column));
  };

  if (hasTakenGameList())
  {
    for (u32 i = 0; i < static_cast<u32>(m_taken_entries->size()); i++)
    {
      if (path == m_taken_entries.value()[i].path)
      {
        remove_entry(&m_taken_entries.value()[i], static_cast<int>(i));
        return;
      }
    }
  }
  else
  {
    // This isn't ideal, but not sure how else we can get the row, when it might change while scanning...
    auto lock = GameList::GetLock();
    const size_t count = GameList::GetEntryCount();
    for (size_t i = 0; i < count; i++)
    {
      const GameList::Entry* const entry = GameList::GetEntryByIndex(static_cast<u32>(i));
      if (entry->path == path)
      {
        remove_entry(entry, static_cast<int>(i));
        return;
      }
    }
  }
}

const QPixmap& GameListModel::getCoverForEntry(const GameList::Entry* ge) const
{
  CoverPixmapCacheEntry* pm = m_cover_pixmap_cache.Lookup(ge->path);
  if (pm && pm->scale == m_cover_scale)
    return pm->pixmap;

  // We insert the placeholder into the cache, so that we don't repeatedly queue loading jobs for this game.
  const_cast<GameListModel*>(this)->loadOrGenerateCover(ge);
  if (pm && !pm->is_loading)
  {
    // Use a fast resize so we don't block the main thread, it'll get fixed up soon.
    // But don't try to resize loading pixmaps.
    if (pm->is_loading)
      pm->pixmap = m_loading_pixmap;
    else
      fastResizePixmap(pm->pixmap, getDeviceScaledCoverArtSize());

    pm->scale = m_cover_scale;
  }
  else
  {
    pm = m_cover_pixmap_cache.Insert(ge->path, CoverPixmapCacheEntry{m_loading_pixmap, m_cover_scale, true});
  }

  return pm->pixmap;
}

const QPixmap* GameListModel::lookupIconPixmapForEntry(const GameList::Entry* ge) const
{
  // We only do this for discs/disc sets for now.
  if (m_show_game_icons && !ge->serial.empty() && ge->IsDiscOrDiscSet())
  {
    QPixmap* item = m_icon_pixmap_cache.Lookup(ge->serial);
    if (item)
    {
      if (!item->isNull())
        return item;
    }
    else
    {
      // Assumes game list lock is held.
      const std::string path = GameList::GetGameIconPath(ge);
      QPixmap pm;
      if (!path.empty() && pm.load(QString::fromStdString(path)))
      {
        pm.setDevicePixelRatio(m_device_pixel_ratio);
        resizeGameIcon(pm, m_icon_size);
        return m_icon_pixmap_cache.Insert(ge->serial, std::move(pm));
      }

      // Stop it trying again in the future.
      m_icon_pixmap_cache.Insert(ge->serial, {});
    }
  }

  return nullptr;
}

const QPixmap& GameListModel::getIconPixmapForEntry(const GameList::Entry* ge) const
{
  if (const QPixmap* pm = lookupIconPixmapForEntry(ge))
    return *pm;

  // If we don't have a pixmap, we return the type pixmap.
  return m_type_pixmaps[static_cast<u32>(ge->type)];
}

const QPixmap& GameListModel::getFlagPixmapForEntry(const GameList::Entry* ge) const
{
  const std::string_view name = ge->GetLanguageIcon();
  auto it = m_flag_pixmap_cache.find(name);
  if (it != m_flag_pixmap_cache.end())
    return it->second;

  const QIcon icon(QtHost::GetResourceQPath(ge->GetLanguageIconName(), true));
  it = m_flag_pixmap_cache.emplace(name, icon.pixmap(FLAG_PIXMAP_SIZE, m_device_pixel_ratio)).first;
  return it->second;
}

bool GameListModel::getShowGameIcons() const
{
  return m_show_game_icons;
}

void GameListModel::setShowGameIcons(bool enabled)
{
  m_show_game_icons = enabled;

  if (enabled)
    GameList::ReloadMemcardTimestampCache();

  invalidateColumn(Column_Icon);
}

QIcon GameListModel::getIconForGame(const QString& path)
{
  QIcon ret;

  if (!m_show_game_icons || path.isEmpty())
    return ret;

  const auto lock = GameList::GetLock();
  const GameList::Entry* entry = GameList::GetEntryForPath(path.toStdString());
  if (!entry || entry->serial.empty() || !entry->IsDiscOrDiscSet())
    return ret;

  // Only use the cache if we're not using larger icons. Otherwise they'll get double scaled.
  // Provides a small performance boost when using default size icons.
  if (m_icon_size == GAME_ICON_DEFAULT_SIZE)
  {
    if (const QPixmap* pm = m_icon_pixmap_cache.Lookup(entry->serial))
    {
      // If we already have the icon cached, return it.
      ret = QIcon(*pm);
      return ret;
    }
  }

  const std::string icon_path = GameList::GetGameIconPath(entry);
  if (!icon_path.empty())
    ret = QIcon(QString::fromStdString(icon_path));

  return ret;
}

QSize GameListModel::getCoverArtSize() const
{
  const int size = std::max(static_cast<int>(static_cast<float>(COVER_ART_SIZE) * m_cover_scale), 1);
  return QSize(size, size);
}

QSize GameListModel::getCoverArtItemSize() const
{
  const int width = std::max(static_cast<int>(static_cast<float>(COVER_ART_SIZE) * m_cover_scale), 1);
  int height = width;

  if (m_show_titles_for_covers)
  {
    // Add some spacing underneath the cover for the caption.
    const QFontMetrics fm(getCoverCaptionFont());
    height += 4 + fm.height();
  }

  return QSize(width, height);
}

QSize GameListModel::getDeviceScaledCoverArtSize() const
{
  const int width = std::max(static_cast<int>(static_cast<float>(COVER_ART_SIZE) * m_cover_scale), 1);
  return QSize(static_cast<int>(static_cast<float>(width) * m_device_pixel_ratio),
               static_cast<int>(static_cast<float>(width) * m_device_pixel_ratio));
}

int GameListModel::getCoverArtSpacing() const
{
  return std::max(static_cast<int>(static_cast<float>(COVER_ART_SPACING) * m_cover_scale), 1);
}

QFont GameListModel::getCoverCaptionFont() const
{
  QFont font;
  font.setPixelSize(std::max(static_cast<int>(30.0f * m_cover_scale), 1));
  font.setFamilies(QtHost::GetRobotoFontFamilies());
  return font;
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

std::pair<std::unique_lock<std::recursive_mutex>, const GameList::Entry*>
GameListModel::getEntryForIndex(const QModelIndex& index) const
{
  std::pair<std::unique_lock<std::recursive_mutex>, const GameList::Entry*> ret;

  if (index.isValid()) [[likely]]
  {
    const int row = index.row();
    DebugAssert(row >= 0);

    if (m_taken_entries.has_value()) [[unlikely]]
    {
      ret.second = (static_cast<u32>(row) < m_taken_entries->size()) ? &m_taken_entries.value()[row] : nullptr;
    }
    else
    {
      ret.first = GameList::GetLock();
      ret.second = GameList::GetEntryByIndex(static_cast<u32>(row));
    }
  }
  else
  {
    ret.second = nullptr;
  }

  return ret;
}

QVariant GameListModel::data(const QModelIndex& index, int role) const
{
  const auto& [lock, ge] = getEntryForIndex(index);
  if (!ge) [[unlikely]]
    return {};

  switch (role)
  {
    case Qt::DisplayRole:
    {
      switch (index.column())
      {
        case Column_Serial:
          return QtUtils::StringViewToQString(ge->serial);

        case Column_Title:
          return QtUtils::StringViewToQString(ge->GetDisplayTitle(m_show_localized_titles));

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
          if (!ge->dbentry || ge->dbentry->release_date == 0)
            return {};

          return QString::number(
            QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ge->dbentry->release_date), QTimeZone::utc())
              .date()
              .year());
        }

        case Column_Players:
        {
          if (!ge->dbentry || ge->dbentry->min_players == 0)
            return {};
          else if (ge->dbentry->min_players == ge->dbentry->max_players)
            return QStringLiteral("%1").arg(ge->dbentry->min_players);
          else
            return QStringLiteral("%1-%2").arg(ge->dbentry->min_players).arg(ge->dbentry->max_players);
        }

        case Column_FileSize:
          return sizeToString(ge->file_size);

        case Column_DataSize:
          return sizeToString(ge->uncompressed_size);

        case Column_Achievements:
          return {};

        case Column_TimePlayed:
        {
          if (ge->total_played_time == 0)
            return {};
          else
            return QtUtils::StringViewToQString(GameList::FormatTimespan(ge->total_played_time, true));
        }

        case Column_LastPlayed:
          return QtUtils::StringViewToQString(GameList::FormatTimestamp(ge->last_played_time));

        default:
          return {};
      }
    }

    case Qt::TextAlignmentRole:
    {
      switch (index.column())
      {
        case Column_FileSize:
        case Column_DataSize:
          return (Qt::AlignRight | Qt::AlignVCenter).toInt();

        case Column_Serial:
        case Column_Year:
        case Column_Players:
          return (Qt::AlignCenter | Qt::AlignVCenter).toInt();

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

    case Qt::SizeHintRole:
    {
      switch (index.column())
      {
        case Column_Icon:
        {
          const int sz = getIconSizeWithPadding();
          return QSize(sz, sz);
        }

        case Column_Cover:
          return getCoverArtItemSize();

        default:
          return {};
      }
    }

    case Qt::DecorationRole:
    {
      switch (index.column())
      {
        case Column_Icon:
          return getIconPixmapForEntry(ge);

        case Column_Region:
          return getFlagPixmapForEntry(ge);

        case Column_Compatibility:
          return m_compatibility_pixmaps[static_cast<u32>(ge->dbentry ? ge->dbentry->compatibility :
                                                                        GameDatabase::CompatibilityRating::Unknown)];

        default:
          return {};
      }
    }

    case Qt::ToolTipRole:
    {
      switch (index.column())
      {
        case Column_Serial:
          return QtUtils::StringViewToQString(ge->serial);

        case Column_Title:
        {
          if (!ge->has_custom_title && ge->dbentry && !ge->dbentry->localized_title.empty())
            return QString::fromStdString(fmt::format("{}\n{}", ge->dbentry->localized_title, ge->dbentry->title));
          else
            return QtUtils::StringViewToQString(ge->GetDisplayTitle(m_show_localized_titles));
        }

        case Column_FileTitle:
          return QtUtils::StringViewToQString(Path::GetFileTitle(ge->path));

        case Column_Developer:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->developer) : QString();

        case Column_Publisher:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->publisher) : QString();

        case Column_Genre:
          return ge->dbentry ? QtUtils::StringViewToQString(ge->dbentry->genre) : QString();

        case Column_TimePlayed:
        {
          if (ge->total_played_time == 0)
            return {};
          else
            return QtUtils::StringViewToQString(GameList::FormatTimespan(ge->total_played_time, false));
        }

        case Column_LastPlayed:
        {
          if (ge->last_played_time == 0)
            return {};
          else
            return QtHost::FormatNumber(Host::NumberFormatType::LongDateTime, static_cast<s64>(ge->last_played_time));
        }

        case Column_Achievements:
        {
          if (ge->num_achievements == 0)
            return tr("No Achievements");

          QString tooltip = tr("%1/%2 achievements unlocked").arg(ge->unlocked_achievements).arg(ge->num_achievements);
          if (ge->unlocked_achievements_hc > 0)
          {
            tooltip = QStringLiteral("%1\n%2").arg(tooltip).arg(
              tr("%1 unlocked in hardcore mode").arg(ge->unlocked_achievements_hc));
          }

          return tooltip;
        }

        default:
          return {};
      }
    }
  }

  return {};
}

QVariant GameListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  QVariant ret;
  if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < Column_Count)
    ret = qApp->translate("GameListModel", s_column_names[static_cast<u32>(section)]);

  return ret;
}

const QPixmap& GameListModel::getNoAchievementsPixmap() const
{
  return m_no_achievements_pixmap;
}

const QPixmap& GameListModel::getHasAchievementsPixmap() const
{
  return m_has_achievements_pixmap;
}

const QPixmap& GameListModel::getMasteredAchievementsPixmap() const
{
  return m_mastered_achievements_pixmap;
}

const GameList::Entry* GameListModel::getTakenGameListEntry(u32 index) const
{
  return (m_taken_entries.has_value() && index < m_taken_entries->size()) ? &m_taken_entries.value()[index] : nullptr;
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
  m_icon_pixmap_cache.Clear();

  endResetModel();
}

bool GameListModel::titlesLessThan(const GameList::Entry* left, const GameList::Entry* right) const
{
  const s32 res = StringUtil::CompareNoCase(left->GetSortTitle(), right->GetSortTitle());
  if (res != 0)
    return (res < 0);

  // Fallback to path compare if titles are the same.
  return (left->path < right->path);
}

bool GameListModel::lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column) const
{
  if (!left_index.isValid() || !right_index.isValid())
    return false;

  const size_t left_row = static_cast<u32>(left_index.row());
  const size_t right_row = static_cast<u32>(right_index.row());

  if (m_taken_entries.has_value()) [[unlikely]]
  {
    const GameList::Entry* left = (left_row < m_taken_entries->size()) ? &m_taken_entries.value()[left_row] : nullptr;
    const GameList::Entry* right =
      (right_row < m_taken_entries->size()) ? &m_taken_entries.value()[right_row] : nullptr;
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

    case Column_DataSize:
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

void GameListModel::loadSizeDependentPixmaps()
{
  const QSize icon_size = QSize(m_icon_size, m_icon_size);
  for (u32 i = 0; i < static_cast<u32>(GameList::EntryType::MaxCount); i++)
  {
    m_type_pixmaps[i] =
      QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(i)).pixmap(icon_size, m_device_pixel_ratio);
  }
}

void GameListModel::loadCommonImages()
{
  loadSizeDependentPixmaps();

  for (u32 i = 0; i < static_cast<u32>(GameDatabase::CompatibilityRating::Count); i++)
  {
    m_compatibility_pixmaps[i] = QtUtils::GetIconForCompatibility(static_cast<GameDatabase::CompatibilityRating>(i))
                                   .pixmap(COMPATIBILITY_PIXMAP_SIZE, m_device_pixel_ratio);
  }

  m_no_achievements_pixmap = QIcon(QtHost::GetResourceQPath("images/trophy-icon-gray.svg", true))
                               .pixmap(ACHIEVEMENT_PIXMAP_SIZE, m_device_pixel_ratio);
  m_has_achievements_pixmap = QIcon(QtHost::GetResourceQPath("images/trophy-icon.svg", true))
                                .pixmap(ACHIEVEMENT_PIXMAP_SIZE, m_device_pixel_ratio);
  m_mastered_achievements_pixmap = QIcon(QtHost::GetResourceQPath("images/trophy-icon-star.svg", true))
                                     .pixmap(ACHIEVEMENT_PIXMAP_SIZE, m_device_pixel_ratio);
}

class GameListSortModel final : public QSortFilterProxyModel
{
public:
  explicit GameListSortModel(GameListModel* parent) : QSortFilterProxyModel(parent), m_model(parent)
  {
    m_merge_disc_sets = Host::GetBaseBoolSettingValue("UI", "GameListMergeDiscSets", true);
  }

  bool isMergingDiscSets() const { return m_merge_disc_sets; }

  void setMergeDiscSets(bool enabled)
  {
    beginFilterChange();
    m_merge_disc_sets = enabled;
    endFilterChange(Direction::Rows);
  }

  void setFilterType(GameList::EntryType type)
  {
    beginFilterChange();
    m_filter_type = type;
    endFilterChange(Direction::Rows);
  }

  void setFilterRegion(DiscRegion region)
  {
    beginFilterChange();
    m_filter_region = region;
    endFilterChange(Direction::Rows);
  }

  void setFilterName(std::string name)
  {
    beginFilterChange();
    m_filter_name = std::move(name);
    std::transform(m_filter_name.begin(), m_filter_name.end(), m_filter_name.begin(), StringUtil::ToLower);
    endFilterChange(Direction::Rows);
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

    if (m_filter_type != GameList::EntryType::MaxCount && entry->type != m_filter_type)
      return false;

    if (m_filter_region != DiscRegion::Count && entry->region != m_filter_region)
      return false;

    if (!m_filter_name.empty())
    {
      if (!((!entry->IsDiscSet() && StringUtil::ContainsNoCase(entry->path, m_filter_name)) ||
            StringUtil::ContainsNoCase(entry->serial, m_filter_name) ||
            StringUtil::ContainsNoCase(entry->GetDisplayTitle(true), m_filter_name) ||
            StringUtil::ContainsNoCase(entry->GetDisplayTitle(false), m_filter_name)))
      {
        return false;
      }
    }

    return QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent);
  }

  bool lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const override
  {
    return m_model->lessThan(source_left, source_right, source_left.column());
  }

private:
  GameListModel* m_model;
  GameList::EntryType m_filter_type = GameList::EntryType::MaxCount;
  DiscRegion m_filter_region = DiscRegion::Count;
  std::string m_filter_name;
  bool m_merge_disc_sets = true;
};

namespace {

class GameListCenterIconStyleDelegate final : public QStyledItemDelegate
{
public:
  explicit GameListCenterIconStyleDelegate(QWidget* parent) : QStyledItemDelegate(parent) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    // https://stackoverflow.com/questions/32216568/how-to-set-icon-center-in-qtableview
    Q_ASSERT(index.isValid());

    const QRect& r = option.rect;
    const QPixmap pix = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
    const QSize pix_size = QtUtils::GetDeviceIndependentSize(pix.size(), pix.devicePixelRatio());

    // draw pixmap at center of item
    const QPoint p((r.width() - pix_size.width()) / 2, (r.height() - pix_size.height()) / 2);
    painter->drawPixmap(r.topLeft() + p, pix);
  }
};

class GameListAchievementsStyleDelegate final : public QStyledItemDelegate
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
    const QSize icon_size = QtUtils::GetDeviceIndependentSize(icon.size(), icon.devicePixelRatio());
    painter->drawPixmap(r.topLeft() + QPoint(5, (r.height() - icon_size.height() + 2) / 2), icon);
    r.setLeft(r.left() + 12 + icon_size.width());

    const QPalette& palette = static_cast<QWidget*>(parent())->palette();
    const QColor& text_color =
      palette.color((option.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);

    painter->save();

    if (num_achievements > 0)
    {
      const QFontMetrics fm(painter->fontMetrics());

      // display hardcore in parenthesis only if there are actually hc unlocks
      const bool display_hardcore = (num_unlocked_hardcore > 0);
      const bool display_hardcore_only =
        (display_hardcore && (num_unlocked == 0 || num_unlocked == num_unlocked_hardcore));
      const QString first = QStringLiteral("%1").arg(display_hardcore_only ? num_unlocked_hardcore : num_unlocked);
      const QString total = QStringLiteral("/%3").arg(num_achievements);

      const QColor hc_color = QColor(44, 151, 250);

      painter->setPen(display_hardcore_only ? hc_color : text_color);
      painter->drawText(r, Qt::AlignVCenter, first);
      r.setLeft(r.left() + fm.size(Qt::TextSingleLine, first).width());

      if (display_hardcore && !display_hardcore_only)
      {
        const QString hc = QStringLiteral("(%2)").arg(num_unlocked_hardcore);
        painter->setPen(hc_color);
        painter->drawText(r, Qt::AlignVCenter, hc);
        r.setLeft(r.left() + fm.size(Qt::TextSingleLine, hc).width());
      }

      painter->setPen(text_color);
      painter->drawText(r, Qt::AlignVCenter, total);
    }
    else
    {
      painter->setPen(text_color);
      painter->drawText(r, Qt::AlignVCenter, QStringLiteral("N/A"));
    }

    painter->restore();
  }

private:
  GameListModel* m_model;
  GameListSortModel* m_sort_model;
};

class GameListAnimatedIconDelegate final : public QStyledItemDelegate
{
public:
  GameListAnimatedIconDelegate(QObject* parent, GameListModel* model) : QStyledItemDelegate(parent), m_model(model)
  {
    connect(&m_animation_timer, &QTimer::timeout, this, &GameListAnimatedIconDelegate::nextAnimationFrame);
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    const int column = index.column();
    if (column != GameListModel::Column_Icon || m_frame_pixmaps.empty())
    {
      if (QAbstractItemDelegate* const delegate =
            static_cast<GameListListView*>(parent())->itemDelegateForColumn(index.column()))
      {
        delegate->paint(painter, option, index);
      }
      else
      {
        QStyledItemDelegate::paint(painter, option, index);
      }

      return;
    }

    const QRect& r = option.rect;
    const QPixmap pix = m_frame_pixmaps[m_current_frame];
    const QSize pix_size = QtUtils::GetDeviceIndependentSize(pix.size(), pix.devicePixelRatio());

    // draw pixmap at center of item
    const QPoint p((r.width() - pix_size.width()) / 2, (r.height() - pix_size.height()) / 2);
    painter->drawPixmap(r.topLeft() + p, pix);
  }

  bool setEntry(const GameList::Entry* entry, int source_row)
  {
    DebugAssert(source_row >= 0);

    const std::string icon_path = GameList::GetGameIconPath(entry);
    if (icon_path.empty())
    {
      clearEntry();
      return false;
    }

    AnimatedImage image;
    Error error;
    if (!image.LoadFromFile(icon_path.c_str(), &error))
    {
      ERROR_LOG("Failed to load animated icon '{}': {}", Path::GetFileName(icon_path), error.GetDescription());
      clearEntry();
      return false;
    }

    // don't use animated delegate if there's only one frame
    if (image.GetFrames() <= 1)
    {
      clearEntry();
      return false;
    }

    m_frame_pixmaps.clear();
    m_frame_pixmaps.reserve(image.GetFrames());
    for (u32 i = 0; i < image.GetFrames(); i++)
    {
      QPixmap pm = QPixmap::fromImage(QImage(reinterpret_cast<uchar*>(image.GetPixels(i)), image.GetWidth(),
                                             image.GetHeight(), QImage::Format::Format_RGBA8888));
      pm.setDevicePixelRatio(m_model->getDevicePixelRatio());
      resizeGameIcon(pm, m_model->getIconSize());
      m_frame_pixmaps.push_back(std::move(pm));
    }

    m_current_frame = 0;
    m_loops_remaining = GAME_ICON_ANIMATION_LOOPS;
    m_source_row = source_row;

    const AnimatedImage::FrameDelay& delay = image.GetFrameDelay(0);
    m_animation_timer.start(std::max((1000 * delay.numerator) / delay.denominator, 100));
    return true;
  }

  void clearEntry()
  {
    m_source_row = -1;
    m_loops_remaining = 0;
    m_current_frame = 0;
    m_frame_pixmaps.clear();
    m_animation_timer.stop();
  }

  void nextAnimationFrame()
  {
    m_current_frame = (m_current_frame + 1) % static_cast<u32>(m_frame_pixmaps.size());
    if (m_current_frame == 0)
    {
      m_loops_remaining--;
      if (m_loops_remaining == 0)
        m_animation_timer.stop();
    }

    const QModelIndex mi = m_model->index(m_source_row, GameListModel::Column_Icon);
    emit m_model->dataChanged(mi, mi, {Qt::DecorationRole});
  }

  void pauseAnimation()
  {
    if (!m_frame_pixmaps.empty() && m_loops_remaining > 0)
      m_animation_timer.stop();
  }

  void resumeAnimation()
  {
    if (!m_frame_pixmaps.empty() && m_loops_remaining > 0)
      m_animation_timer.start();
  }

private:
  GameListModel* m_model;

  std::vector<QPixmap> m_frame_pixmaps;
  u32 m_current_frame = 0;
  int m_loops_remaining = 0;
  int m_source_row = -1;

  QTimer m_animation_timer;
};

class GameListCoverDelegate : public QStyledItemDelegate
{
public:
  explicit GameListCoverDelegate(GameListGridView* const widget, GameListModel* const model,
                                 GameListSortModel* const sort_model)
    : QStyledItemDelegate(widget), m_model(model), m_sort_model(sort_model)
  {
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    Q_ASSERT(index.isValid());

    const auto& [lock, ge] = m_model->getEntryForIndex(m_sort_model->mapToSource(index));
    if (!ge) [[unlikely]]
      return;

    const QPixmap& pix = m_model->getCoverForEntry(ge);
    const QSize pix_size = QtUtils::GetDeviceIndependentSize(pix.size(), pix.devicePixelRatio());
    const QSize cover_size = m_model->getCoverArtSize();

    // draw pixmap at center of item
    const QPoint p((cover_size.width() - pix_size.width()) / 2, (cover_size.height() - pix_size.height()) / 2);
    painter->drawPixmap(option.rect.topLeft() + p, pix);

    const bool show_caption = m_model->getShowCoverTitles();
    const bool show_selected = (option.state & QStyle::State_HasFocus && option.state & QStyle::State_Selected);
    if (!show_caption && !show_selected)
      return;

    painter->save();

    const QPalette::ColorGroup cg = (option.state & QStyle::State_Enabled) ?
                                      ((option.state & QStyle::State_Active) ? QPalette::Normal : QPalette::Inactive) :
                                      QPalette::Disabled;
    // draw title below cover if enabled
    if (show_caption)
    {
      // WE don't use HighlightedText here because on the Classic Windows theme, it makes the text unreadable.
      painter->setPen(option.palette.color(cg, QPalette::Text));

      const QString title = QtUtils::StringViewToQString(ge->GetDisplayTitle(m_model->getShowLocalizedTitles()));
      const QFont font = m_model->getCoverCaptionFont();
      const QFontMetrics fm(font);
      const int text_height = fm.size(Qt::TextSingleLine, title).height();
      const int text_width = option.rect.width();
      const QString elided_text = fm.elidedText(title, Qt::ElideRight, text_width);
      const QRect text_rect(option.rect.left(), option.rect.bottom() - text_height - 2, text_width, text_height);
      painter->setFont(font);
      painter->drawText(text_rect, Qt::AlignHCenter | Qt::AlignVCenter, elided_text);
    }

    // draw highlight and border if selected
    if (option.state & QStyle::State_Selected)
    {
      QStyleOptionFocusRect fo;
      fo.QStyleOption::operator=(option);
      fo.rect = option.rect;
      fo.state |= QStyle::State_KeyboardFocusChange;
      fo.state |= QStyle::State_Item;
      fo.backgroundColor = option.palette.color(cg, QPalette::Highlight);

      const GameListGridView* const widget = static_cast<GameListGridView*>(parent());
      widget->style()->drawPrimitive(QStyle::PE_FrameFocusRect, &fo, painter, widget);

      painter->setRenderHint(QPainter::Antialiasing, false);
      painter->setPen(QPen(QtHost::IsDarkApplicationTheme() ? QColor(180, 180, 180) : QColor(0, 0, 0), 2));
      painter->setBrush(Qt::NoBrush);

      // Draw border manually instead of with drawRect to avoid joins at corners.
      // Using the top-left pixel causes rendering issues at high-dpi.
      const QRect border_rect = option.rect.adjusted(1, 1, 0, 0);
      const std::array<QPoint, 4 * 2> line_points = {{
        border_rect.topLeft(),
        border_rect.topRight(),
        border_rect.topRight(),
        border_rect.bottomRight(),
        border_rect.bottomRight(),
        border_rect.bottomLeft(),
        border_rect.bottomLeft(),
        border_rect.topLeft(),
      }};
      painter->drawLines(line_points.data(), static_cast<int>(line_points.size()) / 2);
    }

    painter->restore();
  }

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    return m_model->getCoverArtItemSize();
  }

private:
  GameListModel* const m_model;
  GameListSortModel* const m_sort_model;
};

} // namespace

GameListWidget::GameListWidget(QWidget* parent, QAction* action_view_list, QAction* action_view_grid,
                               QAction* action_merge_disc_sets, QAction* action_show_list_icons,
                               QAction* action_animate_list_icons, QAction* action_prefer_achievement_game_icons,
                               QAction* action_show_grid_titles, QAction* action_show_localized_titles)
  : QWidget(parent)
{
  m_model = new GameListModel(this);
  connect(m_model, &GameListModel::coverScaleChanged, this, &GameListWidget::onScaleChanged);
  connect(m_model, &GameListModel::iconSizeChanged, this, &GameListWidget::onIconSizeChanged);

  m_sort_model = new GameListSortModel(m_model);
  m_sort_model->setSourceModel(m_model);

  m_ui.setupUi(this);
  for (u32 type = 0; type < static_cast<u32>(GameList::EntryType::MaxCount); type++)
  {
    m_ui.filterType->addItem(
      QtUtils::GetIconForEntryType(static_cast<GameList::EntryType>(type)),
      QString::fromUtf8(GameList::GetEntryTypeDisplayName(static_cast<GameList::EntryType>(type))));
  }
  for (u32 region = 0; region < static_cast<u32>(DiscRegion::Count); region++)
  {
    m_ui.filterRegion->addItem(QtUtils::GetIconForRegion(static_cast<DiscRegion>(region)),
                               QString::fromUtf8(Settings::GetDiscRegionName(static_cast<DiscRegion>(region))));
  }

  m_list_view = new GameListListView(m_model, m_sort_model, m_ui.stack);
  m_list_view->setAnimateGameIcons(Host::GetBaseBoolSettingValue("UI", "GameListAnimateGameIcons", false));
  m_ui.stack->insertWidget(0, m_list_view);

  m_grid_view = new GameListGridView(m_model, m_sort_model, m_ui.stack);
  m_grid_view->setItemDelegate(new GameListCoverDelegate(m_grid_view, m_model, m_sort_model));
  m_ui.stack->insertWidget(1, m_grid_view);

  m_empty_widget = new QWidget(m_ui.stack);
  m_empty_ui.setupUi(m_empty_widget);
  m_empty_ui.supportedFormats->setText(qApp->translate("GameListWidget", SUPPORTED_FORMATS_STRING));
  m_ui.stack->insertWidget(2, m_empty_widget);

  m_ui.viewGameList->setDefaultAction(action_view_list);
  m_ui.viewGameGrid->setDefaultAction(action_view_grid);
  m_ui.mergeDiscSets->setDefaultAction(action_merge_disc_sets);
  m_ui.showGameIcons->setDefaultAction(action_show_list_icons);
  m_ui.showGridTitles->setDefaultAction(action_show_grid_titles);
  m_ui.showLocalizedTitles->setDefaultAction(action_show_localized_titles);

  connect(m_ui.scale, &QSlider::sliderPressed, this, &GameListWidget::showScaleToolTip);
  connect(m_ui.scale, &QSlider::sliderReleased, this, &QToolTip::hideText);
  connect(m_ui.scale, &QSlider::valueChanged, this, &GameListWidget::onScaleSliderChanged);
  connect(m_ui.filterType, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_sort_model->setFilterType((index == 0) ? GameList::EntryType::MaxCount :
                                               static_cast<GameList::EntryType>(index - 1));
  });
  connect(m_ui.filterRegion, &QComboBox::currentIndexChanged, this, [this](int index) {
    m_sort_model->setFilterRegion((index == 0) ? DiscRegion::Count : static_cast<DiscRegion>(index - 1));
  });
  connect(m_ui.searchText, &QLineEdit::textChanged, this,
          [this](const QString& text) { m_sort_model->setFilterName(text.toStdString()); });
  connect(m_ui.searchText, &QLineEdit::returnPressed, this, &GameListWidget::onSearchReturnPressed);

  connect(m_list_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);
  connect(m_list_view, &QTableView::activated, this, &GameListWidget::onListViewItemActivated);
  connect(m_list_view, &QTableView::customContextMenuRequested, this, &GameListWidget::onListViewContextMenuRequested);

  connect(m_grid_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &GameListWidget::onSelectionModelCurrentChanged);
  connect(m_grid_view, &QListView::activated, this, &GameListWidget::onGridViewItemActivated);
  connect(m_grid_view, &QListView::customContextMenuRequested, this, &GameListWidget::onGridViewContextMenuRequested);

  connect(m_empty_ui.addGameDirectory, &QPushButton::clicked, this, [this]() { emit addGameDirectoryRequested(); });
  connect(m_empty_ui.scanForNewGames, &QPushButton::clicked, this, [this]() { refresh(false); });

  connect(g_main_window, &MainWindow::themeChanged, this, &GameListWidget::onThemeChanged);

  const bool grid_view = Host::GetBaseBoolSettingValue("UI", "GameListGridView", false);
  if (grid_view)
    action_view_grid->setChecked(true);
  else
    action_view_list->setChecked(true);
  action_merge_disc_sets->setChecked(m_sort_model->isMergingDiscSets());
  action_show_localized_titles->setChecked(m_model->getShowLocalizedTitles());
  action_show_list_icons->setChecked(m_model->getShowGameIcons());
  action_animate_list_icons->setChecked(m_list_view->isAnimatingGameIcons());
  action_prefer_achievement_game_icons->setChecked(GameList::PreferAchievementGameBadgesForIcons());
  action_show_grid_titles->setChecked(m_model->getShowCoverTitles());
  onIconSizeChanged(m_model->getIconSize());

  setViewMode(grid_view ? VIEW_MODE_GRID : VIEW_MODE_LIST);
  updateBackground(true);
}

GameListWidget::~GameListWidget() = default;

bool GameListWidget::isShowingGameList() const
{
  return (m_ui.stack->currentIndex() == VIEW_MODE_LIST);
}

bool GameListWidget::isShowingGameGrid() const
{
  return (m_ui.stack->currentIndex() == VIEW_MODE_GRID);
}

void GameListWidget::zoomOut()
{
  if (isShowingGameList())
    m_list_view->adjustIconSize(-GAME_ICON_SIZE_STEP);
  else if (isShowingGameGrid())
    m_grid_view->adjustZoom(-COVER_SCALE_STEP);
}

void GameListWidget::zoomIn()
{
  if (isShowingGameList())
    m_list_view->adjustIconSize(GAME_ICON_SIZE_STEP);
  else if (isShowingGameGrid())
    m_grid_view->adjustZoom(COVER_SCALE_STEP);
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
  m_refresh_last_entry_count = std::numeric_limits<int>::max(); // force reset on first progress update
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

void GameListWidget::onThemeChanged()
{
  m_model->reloadThemeSpecificImages();

  // Resize columns, since the text size can change with themes.
  m_list_view->updateFixedColumnWidths();

  // Hacks for background.
  updateBackground(false);
}

void GameListWidget::setBackgroundPath(const std::string_view path)
{
  if (!path.empty())
  {
    Host::SetBaseStringSettingValue("UI", "GameListBackgroundPath",
                                    Path::MakeRelative(path, EmuFolders::DataRoot).c_str());
  }
  else
  {
    Host::DeleteBaseSettingValue("UI", "GameListBackgroundPath");
  }

  Host::CommitBaseSettingChanges();
  updateBackground(true);
}

void GameListWidget::updateBackground(bool reload_image)
{
  const bool had_image = !m_background_image.isNull();
  if (reload_image)
  {
    m_background_image = QImage();

    if (std::string path = Host::GetBaseStringSettingValue("UI", "GameListBackgroundPath"); !path.empty())
    {
      if (!Path::IsAbsolute(path))
        path = Path::Combine(EmuFolders::DataRoot, path);

      if (m_background_image.load(path.c_str()))
        m_background_image.setDevicePixelRatio(devicePixelRatio());
    }
  }

  if (m_background_image.isNull())
  {
    if (had_image)
    {
      m_ui.stack->setPalette(qApp->palette(m_ui.stack));
      m_ui.stack->setAutoFillBackground(false);
      m_list_view->setAlternatingRowColors(true);
      m_list_view->setStyleSheet(QString());
      m_grid_view->setStyleSheet(QString());
    }

    return;
  }

  QImage scaled_image = m_background_image;
  resizeAndPadImage(&scaled_image,
                    QtUtils::ApplyDevicePixelRatioToSize(m_ui.stack->size(), m_model->getDevicePixelRatio()), true,
                    true);

  QPalette new_palette = qApp->palette(m_ui.stack);
  new_palette.setBrush(QPalette::Window, QPixmap::fromImage(scaled_image));
  new_palette.setBrush(QPalette::Base, Qt::transparent);
  m_ui.stack->setPalette(new_palette);
  m_ui.stack->setAutoFillBackground(true);
  m_list_view->setAlternatingRowColors(false);

  if (QtHost::HasGlobalStylesheet())
  {
    // Stylesheets override palette, so we need to set background: transparent on the grid and list view.
    const QString style_sheet = QStringLiteral("QAbstractScrollArea { background-color: transparent; }");
    m_list_view->setStyleSheet(style_sheet);
    m_grid_view->setStyleSheet(style_sheet);
  }
}

bool GameListWidget::hasBackground() const
{
  return !m_background_image.isNull();
}

void GameListWidget::onRefreshProgress(const QString& status, int current, int total, int entry_count, float time)
{
  // Avoid spamming the UI on very short refresh (e.g. game exit).
  static constexpr float SHORT_REFRESH_TIME = 0.5f;
  if (!m_model->hasTakenGameList())
  {
    if (entry_count > m_refresh_last_entry_count)
    {
      m_model->beginInsertRows(QModelIndex(), m_refresh_last_entry_count, entry_count - 1);
      m_model->endInsertRows();
    }
    else
    {
      m_model->beginResetModel();
      m_model->endResetModel();
    }

    m_refresh_last_entry_count = entry_count;
  }

  // switch away from the placeholder while we scan, in case we find anything
  if (m_ui.stack->currentIndex() == VIEW_MODE_NO_GAMES)
    setViewMode(Host::GetBaseBoolSettingValue("UI", "GameListGridView", false) ? VIEW_MODE_GRID : VIEW_MODE_LIST);

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
    setViewMode(VIEW_MODE_NO_GAMES);
}

void GameListWidget::onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous)
{
  const QModelIndex source_index = m_sort_model->mapToSource(current);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
  {
    m_list_view->clearAnimatedGameIconDelegate();
    return;
  }

  // selection model hasn't updated yet, so this has to be queued... ugh.
  QMetaObject::invokeMethod(m_list_view, &GameListListView::updateAnimatedGameIconDelegate, Qt::QueuedConnection);

  emit selectionChanged();
}

void GameListWidget::onListViewItemActivated(const QModelIndex& index)
{
  const QModelIndex source_index = m_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
    return;

  if (qApp->keyboardModifiers().testFlag(Qt::AltModifier))
  {
    const auto lock = GameList::GetLock();
    const GameList::Entry* entry = GameList::GetEntryByIndex(static_cast<u32>(source_index.row()));
    if (entry)
      SettingsWindow::openGamePropertiesDialog(entry);
  }
  else
  {
    emit entryActivated();
  }
}

void GameListWidget::onListViewContextMenuRequested(const QPoint& point)
{
  emit entryContextMenuRequested(m_list_view->mapToGlobal(point));
}

void GameListWidget::onGridViewItemActivated(const QModelIndex& index)
{
  const QModelIndex source_index = m_sort_model->mapToSource(index);
  if (!source_index.isValid() || source_index.row() >= static_cast<int>(GameList::GetEntryCount()))
    return;

  emit entryActivated();
}

void GameListWidget::onGridViewContextMenuRequested(const QPoint& point)
{
  emit entryContextMenuRequested(m_grid_view->mapToGlobal(point));
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
    isShowingGameGrid() ? static_cast<QAbstractItemView*>(m_grid_view) : static_cast<QAbstractItemView*>(m_list_view);
  target->selectionModel()->select(m_sort_model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  target->setFocus(Qt::ShortcutFocusReason);
}

void GameListWidget::showGameList()
{
  // keep showing the placeholder widget if we have no games
  if (isShowingGameList() || m_model->rowCount() == 0)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListGridView", false);
  Host::CommitBaseSettingChanges();

  setViewMode(VIEW_MODE_LIST);
}

void GameListWidget::showGameGrid()
{
  // keep showing the placeholder widget if we have no games
  if (isShowingGameGrid() || m_model->rowCount() == 0)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListGridView", true);
  Host::CommitBaseSettingChanges();

  setViewMode(VIEW_MODE_GRID);
}

void GameListWidget::setMergeDiscSets(bool enabled)
{
  if (m_sort_model->isMergingDiscSets() == enabled)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListMergeDiscSets", enabled);
  Host::CommitBaseSettingChanges();
  m_sort_model->setMergeDiscSets(enabled);
}

void GameListWidget::setShowLocalizedTitles(bool enabled)
{
  if (m_model->getShowLocalizedTitles() == enabled)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListShowLocalizedTitles", enabled);
  Host::CommitBaseSettingChanges();
  m_model->setShowLocalizedTitles(enabled);
}

void GameListWidget::setShowGameIcons(bool enabled)
{
  if (m_model->getShowGameIcons() == enabled)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListShowGameIcons", enabled);
  Host::CommitBaseSettingChanges();
  m_model->setShowGameIcons(enabled);
  if (isShowingGameList() && m_list_view->isAnimatingGameIcons())
  {
    if (enabled)
      m_list_view->updateAnimatedGameIconDelegate();
    else
      m_list_view->clearAnimatedGameIconDelegate();
  }
}

void GameListWidget::setAnimateGameIcons(bool enabled)
{
  if (m_list_view->isAnimatingGameIcons() == enabled)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListAnimateGameIcons", enabled);
  Host::CommitBaseSettingChanges();
  m_list_view->setAnimateGameIcons(enabled);
  if (isShowingGameList())
    m_list_view->updateAnimatedGameIconDelegate();
}

void GameListWidget::setPreferAchievementGameIcons(bool enabled)
{
  Host::SetBaseBoolSettingValue("UI", "GameListPreferAchievementGameBadgesForIcons", enabled);
  Host::CommitBaseSettingChanges();

  if (!enabled)
    GameList::ReloadMemcardTimestampCache();

  m_model->invalidateColumn(GameListModel::Column_Icon);
}

void GameListWidget::setShowCoverTitles(bool enabled)
{
  if (m_model->getShowCoverTitles() == enabled)
    return;

  Host::SetBaseBoolSettingValue("UI", "GameListShowCoverTitles", enabled);
  Host::CommitBaseSettingChanges();
  m_model->setShowCoverTitles(enabled);
  m_grid_view->updateLayout();
}

void GameListWidget::setViewMode(int stack_index)
{
  const int prev_stack_index = m_ui.stack->currentIndex();
  m_ui.stack->setCurrentIndex(stack_index);
  setFocusProxy(m_ui.stack->currentWidget());

  // this is pretty yuck, because it's a "manual" toolbar we can't just disable the parent
  const bool has_games = (stack_index != VIEW_MODE_NO_GAMES);
  m_ui.viewGameList->setEnabled(has_games);
  m_ui.viewGameGrid->setEnabled(has_games);
  m_ui.mergeDiscSets->setEnabled(has_games);
  m_ui.showLocalizedTitles->setEnabled(has_games);
  m_ui.showGameIcons->setEnabled(has_games);
  m_ui.showGridTitles->setEnabled(has_games);
  m_ui.scale->setEnabled(has_games);
  m_ui.filterType->setEnabled(has_games);
  m_ui.filterRegion->setEnabled(has_games);
  m_ui.searchText->setEnabled(has_games);

  const bool is_grid_view = isShowingGameGrid();
  m_ui.showGameIcons->setVisible(!is_grid_view);
  m_ui.showGridTitles->setVisible(is_grid_view);

  QSignalBlocker sb(m_ui.scale);
  if (is_grid_view)
  {
    m_ui.scale->setMinimum(static_cast<int>(MIN_COVER_SCALE * 100.0f));
    m_ui.scale->setMaximum(static_cast<int>(MAX_COVER_SCALE * 100.0f));
    m_ui.scale->setValue(static_cast<int>(m_model->getCoverScale() * 100.0f));
  }
  else
  {
    m_ui.scale->setMinimum(GAME_ICON_MIN_SIZE / GAME_ICON_SIZE_STEP);
    m_ui.scale->setMaximum(GAME_ICON_MAX_SIZE / GAME_ICON_SIZE_STEP);
    m_ui.scale->setValue(m_model->getIconSize() / GAME_ICON_SIZE_STEP);
  }

  // pause animation when list is not visible
  if (stack_index == VIEW_MODE_LIST)
    m_list_view->updateAnimatedGameIconDelegate();
  else if (prev_stack_index == VIEW_MODE_LIST)
    m_list_view->clearAnimatedGameIconDelegate();
}

void GameListWidget::showScaleToolTip()
{
  const int value = m_ui.scale->value();
  if (isShowingGameGrid())
    QToolTip::showText(QCursor::pos(), tr("Cover scale: %1%").arg(value));
  else if (isShowingGameList())
    QToolTip::showText(QCursor::pos(), tr("Icon size: %1%").arg((value * 100) / GAME_ICON_SIZE_STEP));
}

void GameListWidget::onScaleSliderChanged(int value)
{
  if (isShowingGameGrid())
  {
    m_model->setCoverScale(static_cast<float>(value) / 100.0f);
  }
  else if (isShowingGameList())
  {
    m_model->setIconSize(value * GAME_ICON_SIZE_STEP);
    m_list_view->updateAnimatedGameIconDelegate();
  }

  if (m_ui.scale->isSliderDown())
    showScaleToolTip();
}

void GameListWidget::onScaleChanged()
{
  int value = m_ui.scale->value();
  if (isShowingGameGrid())
    value = static_cast<int>(m_model->getCoverScale() * 100.0f);
  else if (isShowingGameList())
    value = m_model->getIconSize() / GAME_ICON_SIZE_STEP;

  QSignalBlocker sb(m_ui.scale);
  m_ui.scale->setValue(value);
}

void GameListWidget::onIconSizeChanged(int size)
{
  m_list_view->setFixedColumnWidth(m_list_view->fontMetricsForHorizontalHeader(), GameListModel::Column_Icon,
                                   m_model->getIconSizeWithPadding());
  m_list_view->verticalHeader()->setDefaultSectionSize(m_model->getRowHeight());
  onScaleChanged();
}

bool GameListWidget::event(QEvent* e)
{
  const QEvent::Type type = e->type();
  if (type == QEvent::Resize)
    updateBackground(false);
  else if (type == QEvent::DevicePixelRatioChange)
    m_model->setDevicePixelRatio(devicePixelRatio());

  return QWidget::event(e);
}

const GameList::Entry* GameListWidget::getSelectedEntry() const
{
  if (isShowingGameList())
  {
    const QItemSelectionModel* selection_model = m_list_view->selectionModel();
    if (!selection_model->hasSelection())
      return nullptr;

    const QModelIndexList selected_rows = selection_model->selectedRows();
    if (selected_rows.empty())
      return nullptr;

    const QModelIndex source_index = m_sort_model->mapToSource(selected_rows[0]);
    if (!source_index.isValid())
      return nullptr;

    return GameList::GetEntryByIndex(static_cast<u32>(source_index.row()));
  }
  else
  {
    const QItemSelectionModel* selection_model = m_grid_view->selectionModel();
    if (!selection_model->hasSelection())
      return nullptr;

    const QModelIndex source_index = m_sort_model->mapToSource(selection_model->currentIndex());
    if (!source_index.isValid())
      return nullptr;

    return GameList::GetEntryByIndex(static_cast<u32>(source_index.row()));
  }
}

GameListListView::GameListListView(GameListModel* model, GameListSortModel* sort_model, QWidget* parent)
  : QTableView(parent), m_model(model), m_sort_model(sort_model)
{
  setModel(sort_model);
  setSortingEnabled(true);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setContextMenuPolicy(Qt::CustomContextMenu);
  setAlternatingRowColors(true);
  setShowGrid(false);

  QHeaderView* const horizontal_header = horizontalHeader();
  horizontal_header->setHighlightSections(false);
  horizontal_header->setContextMenuPolicy(Qt::CustomContextMenu);
  updateFixedColumnWidths();

  horizontal_header->setSectionResizeMode(GameListModel::Column_Title, QHeaderView::Stretch);
  horizontal_header->setSectionResizeMode(GameListModel::Column_FileTitle, QHeaderView::Stretch);

  verticalHeader()->hide();

  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);

  GameListCenterIconStyleDelegate* const center_icon_delegate = new GameListCenterIconStyleDelegate(this);
  setItemDelegateForColumn(GameListModel::Column_Icon, center_icon_delegate);
  setItemDelegateForColumn(GameListModel::Column_Region, center_icon_delegate);
  setItemDelegateForColumn(GameListModel::Column_Achievements,
                           new GameListAchievementsStyleDelegate(this, model, sort_model));

  loadColumnVisibilitySettings();
  loadColumnSortSettings();

  connect(horizontal_header, &QHeaderView::sortIndicatorChanged, this, &GameListListView::saveColumnSortSettings);
  connect(horizontal_header, &QHeaderView::customContextMenuRequested, this,
          &GameListListView::onHeaderContextMenuRequested);
}

GameListListView::~GameListListView() = default;

void GameListListView::wheelEvent(QWheelEvent* e)
{
  if (e->modifiers() & Qt::ControlModifier)
  {
    const int dy = e->angleDelta().y();
    if (dy != 0)
    {
      adjustIconSize((dy < 0) ? -GAME_ICON_SIZE_STEP : GAME_ICON_SIZE_STEP);
      return;
    }
  }

  QTableView::wheelEvent(e);
}

QFontMetrics GameListListView::fontMetricsForHorizontalHeader() const
{
  // https://github.com/qt/qtbase/blob/9cc32c2490813b81ce36fc97f959078bf5c2fbf5/src/widgets/itemviews/qheaderview.cpp#L3148
  QFont font = horizontalHeader()->font();
  font.setBold(true);
  return QFontMetrics(font);
}

void GameListListView::setFixedColumnWidth(const QFontMetrics& fm, int column, int str_width)
{
  horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);

  const int margin = style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr, this);
  const int header_width = fm.size(0, m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString()).width() +
                           style()->pixelMetric(QStyle::PM_HeaderMarkSize, nullptr, this) + // sort indicator
                           3 * margin; // left/right margins + space between text and sort indicator
  setColumnWidth(column, std::max(header_width, str_width));
}

void GameListListView::updateFixedColumnWidths()
{
  const QFontMetrics fm = fontMetricsForHorizontalHeader();
  // See QCommonStylePrivate::viewItemSize()
  const int margins = 1 + 2 * (style()->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, this) + 1);
  const auto width_for = [&](const QString& text) { return fm.size(0, text).width() + margins; };

  setFixedColumnWidth(fm, GameListModel::Column_Serial, width_for(QStringLiteral("SWWW-00000")));
  setFixedColumnWidth(fm, GameListModel::Column_Year,
                      std::max(width_for(QStringLiteral("1999")), width_for(QStringLiteral("2000"))));
  setFixedColumnWidth(fm, GameListModel::Column_Players, width_for(QStringLiteral("1-8")));

  // Played time is a little trickier, since some locales might have longer words for "hours" and "minutes".
  setFixedColumnWidth(fm, GameListModel::Column_TimePlayed,
                      std::max({width_for(qApp->translate("GameList", "%n seconds", "", 59)),
                                width_for(qApp->translate("GameList", "%n minutes", "", 59)),
                                width_for(qApp->translate("GameList", "%n hours", "", 1000))}));

  // And this is a monstrosity.
  setFixedColumnWidth(
    fm, GameListModel::Column_LastPlayed,
    std::max({width_for(qApp->translate("GameList", "Today")), width_for(qApp->translate("GameList", "Yesterday")),
              width_for(qApp->translate("GameList", "Never")),
              width_for(QtHost::FormatNumber(Host::NumberFormatType::ShortDate,
                                             static_cast<s64>(QDateTime::currentSecsSinceEpoch())))}));

  // Assume 8 is the widest digit.
  const int size_width = std::max(width_for(QStringLiteral("%1 MB").arg(8888)), width_for(tr("Unknown")));
  setFixedColumnWidth(fm, GameListModel::Column_FileSize, size_width);
  setFixedColumnWidth(fm, GameListModel::Column_DataSize, size_width);

  setFixedColumnWidth(fm, GameListModel::Column_Icon, m_model->getIconSizeWithPadding());
  setFixedColumnWidth(fm, GameListModel::Column_Region, FLAG_PIXMAP_SIZE.width());
  setFixedColumnWidth(fm, GameListModel::Column_Achievements, 100);
  setFixedColumnWidth(fm, GameListModel::Column_Compatibility, COMPATIBILITY_PIXMAP_SIZE.width());
  setColumnWidth(GameListModel::Column_Developer, 200);
  setColumnWidth(GameListModel::Column_Publisher, 200);
  setColumnWidth(GameListModel::Column_Genre, 200);
}

static TinyString getColumnVisibilitySettingsKeyName(int column)
{
  return TinyString::from_format("Show{}", GameListModel::getColumnName(static_cast<GameListModel::Column>(column)));
}

void GameListListView::loadColumnVisibilitySettings()
{
  static constexpr std::array<bool, GameListModel::Column_Count> DEFAULT_VISIBILITY = {{
    true,  // type
    true,  // serial
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
    setColumnHidden(column, !visible);
  }
}

void GameListListView::loadColumnSortSettings()
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
  if (QHeaderView* hv = horizontalHeader())
    hv->setSortIndicator(sort_column, sort_order);
}

void GameListListView::saveColumnSortSettings()
{
  const int sort_column = horizontalHeader()->sortIndicatorSection();
  const bool sort_descending = (horizontalHeader()->sortIndicatorOrder() == Qt::DescendingOrder);

  if (sort_column >= 0 && sort_column < GameListModel::Column_Count)
  {
    Host::SetBaseStringSettingValue("GameListTableView", "SortColumn",
                                    GameListModel::getColumnName(static_cast<GameListModel::Column>(sort_column)));
  }

  Host::SetBaseBoolSettingValue("GameListTableView", "SortDescending", sort_descending);
  Host::CommitBaseSettingChanges();
}

void GameListListView::setAndSaveColumnHidden(int column, bool hidden)
{
  DebugAssert(column < GameListModel::Column_Count);
  if (isColumnHidden(column) == hidden)
    return;

  setColumnHidden(column, hidden);
  Host::SetBaseBoolSettingValue("GameListTableView", getColumnVisibilitySettingsKeyName(column), !hidden);
  Host::CommitBaseSettingChanges();
}

void GameListListView::onHeaderContextMenuRequested(const QPoint& point)
{
  QMenu* const menu = QtUtils::NewPopupMenu(this);

  for (int column = 0; column < GameListModel::Column_Count; column++)
  {
    if (column == GameListModel::Column_Cover)
      continue;

    QAction* const action = menu->addAction(m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString(),
                                            [this, column](bool enabled) { setAndSaveColumnHidden(column, !enabled); });
    action->setCheckable(true);
    action->setChecked(!isColumnHidden(column));
  }

  menu->popup(mapToGlobal(point));
}

void GameListListView::adjustIconSize(int delta)
{
  const int new_size = std::clamp(m_model->getIconSize() + delta, GAME_ICON_MIN_SIZE, GAME_ICON_MAX_SIZE);
  m_model->setIconSize(new_size);
  updateAnimatedGameIconDelegate();
}

bool GameListListView::isAnimatingGameIcons() const
{
  return (m_animated_game_icon_delegate != nullptr);
}

void GameListListView::setAnimateGameIcons(bool enabled)
{
  if (!enabled)
  {
    clearAnimatedGameIconDelegate();
    delete m_animated_game_icon_delegate;
    m_animated_game_icon_delegate = nullptr;
    return;
  }

  if (m_animated_game_icon_delegate)
    return;

  m_animated_game_icon_delegate = new GameListAnimatedIconDelegate(this, m_model);
}

void GameListListView::updateAnimatedGameIconDelegate()
{
  if (!m_animated_game_icon_delegate || !m_model->getShowGameIcons())
    return;

  const QModelIndexList selected = selectionModel()->selectedIndexes();
  if (selected.isEmpty())
  {
    clearAnimatedGameIconDelegate();
    return;
  }

  // clear previous
  const int visible_row = selected.first().row();
  if (m_animated_icon_row >= 0)
  {
    setItemDelegateForRow(m_animated_icon_row, nullptr);
    m_animated_icon_row = -1;
  }

  const auto lock = GameList::GetLock();
  const QModelIndex source_index = m_sort_model->mapToSource(selected.first());
  const GameList::Entry* entry = m_model->hasTakenGameList() ?
                                   m_model->getTakenGameListEntry(static_cast<u32>(source_index.row())) :
                                   GameList::GetEntryByIndex(static_cast<u32>(source_index.row()));

  // don't try to load an animated icon if there is no icon
  if (!entry || !m_model->lookupIconPixmapForEntry(entry))
    return;

  if (static_cast<GameListAnimatedIconDelegate*>(m_animated_game_icon_delegate)->setEntry(entry, source_index.row()))
  {
    m_animated_icon_row = visible_row;
    setItemDelegateForRow(visible_row, m_animated_game_icon_delegate);
  }
}

void GameListListView::clearAnimatedGameIconDelegate()
{
  if (m_animated_icon_row < 0)
    return;

  static_cast<GameListAnimatedIconDelegate*>(m_animated_game_icon_delegate)->clearEntry();
  setItemDelegateForRow(m_animated_icon_row, nullptr);
  m_animated_icon_row = -1;
}

GameListGridView::GameListGridView(GameListModel* model, GameListSortModel* sort_model, QWidget* parent)
  : QListView(parent), m_model(model), m_sort_model(sort_model)
{
  setModel(sort_model);
  setModelColumn(GameListModel::Column_Cover);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setViewMode(QListView::IconMode);
  setResizeMode(QListView::Adjust);
  setUniformItemSizes(true);
  setItemAlignment(Qt::AlignHCenter);
  setContextMenuPolicy(Qt::CustomContextMenu);
  setFrameStyle(QFrame::NoFrame);
  setVerticalScrollMode(QAbstractItemView::ScrollMode::ScrollPerPixel);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  verticalScrollBar()->setSingleStep(15);
  connect(m_model, &GameListModel::coverScaleChanged, this, &GameListGridView::updateLayout);
  updateLayout();
}

GameListGridView::~GameListGridView() = default;

void GameListGridView::wheelEvent(QWheelEvent* e)
{
  if (e->modifiers() & Qt::ControlModifier)
  {
    const int dy = e->angleDelta().y();
    if (dy != 0)
    {
      if (dy < 0)
        adjustZoom(-COVER_SCALE_STEP);
      else
        adjustZoom(COVER_SCALE_STEP);

      return;
    }
  }

  QListView::wheelEvent(e);
}

void GameListGridView::resizeEvent(QResizeEvent* e)
{
  QListView::resizeEvent(e);
  updateLayout();
}

void GameListGridView::adjustZoom(float delta)
{
  const float new_scale = std::clamp(m_model->getCoverScale() + delta, MIN_COVER_SCALE, MAX_COVER_SCALE);
  m_model->setCoverScale(new_scale);
}

void GameListGridView::updateLayout()
{
  const QSize item_size = m_model->getCoverArtItemSize();
  const int item_spacing = m_model->getCoverArtSpacing();
  const int item_margin = style()->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, this) * 2;
  const int item_width = item_size.width() + item_margin + item_spacing;
  const int item_height = item_size.height() + item_margin + item_spacing;

  // the -1 here seems to be necessary otherwise we calculate too many columns..
  // can't see where in qlistview.cpp it's coming from though.
  const int available_width = viewport()->width();
  const int num_columns = (available_width - 1) / item_width;
  const int num_rows = (viewport()->height() + (item_height - 1)) / item_height;
  const int margin = (available_width - (num_columns * item_width)) / 2;

  setGridSize(QSize(item_width, item_height));

  const int top_left_row = (verticalScrollBar()->value() / item_height) * num_columns;
  m_model->updateCacheSize(num_rows, num_columns, m_sort_model, top_left_row);

  m_horizontal_offset = margin;
  m_vertical_offset = item_spacing;
}

int GameListGridView::horizontalOffset() const
{
  return QListView::horizontalOffset() - m_horizontal_offset;
}

int GameListGridView::verticalOffset() const
{
  return QListView::verticalOffset() - m_vertical_offset;
}
