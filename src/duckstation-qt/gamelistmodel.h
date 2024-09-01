// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core/game_database.h"
#include "core/game_list.h"
#include "core/types.h"

#include "common/heterogeneous_containers.h"
#include "common/lru_cache.h"

#include <QtCore/QAbstractTableModel>
#include <QtGui/QPixmap>
#include <algorithm>
#include <array>
#include <optional>

class GameListModel final : public QAbstractTableModel
{
  Q_OBJECT

public:
  enum Column : int
  {
    Column_Icon,
    Column_Serial,
    Column_Title,
    Column_FileTitle,
    Column_Developer,
    Column_Publisher,
    Column_Genre,
    Column_Year,
    Column_Players,
    Column_TimePlayed,
    Column_LastPlayed,
    Column_FileSize,
    Column_UncompressedSize,
    Column_Region,
    Column_Compatibility,
    Column_Cover,

    Column_Count
  };

  static std::optional<Column> getColumnIdForName(std::string_view name);
  static const char* getColumnName(Column col);

  GameListModel(float cover_scale, bool show_cover_titles, bool show_game_icons, QObject* parent = nullptr);
  ~GameListModel();

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  ALWAYS_INLINE const QString& getColumnDisplayName(int column) { return m_column_display_names[column]; }

  bool hasTakenGameList() const;
  void takeGameList();

  void refresh();
  void reloadThemeSpecificImages();

  bool titlesLessThan(const GameList::Entry* left, const GameList::Entry* right) const;
  bool lessThan(const GameList::Entry* left, const GameList::Entry* right, int column) const;

  bool lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column) const;

  bool getShowCoverTitles() const { return m_show_titles_for_covers; }
  void setShowCoverTitles(bool enabled) { m_show_titles_for_covers = enabled; }

  bool getShowGameIcons() const { return m_show_game_icons; }
  void setShowGameIcons(bool enabled);
  QIcon getIconForGame(const QString& path);

  float getCoverScale() const { return m_cover_scale; }
  void setCoverScale(float scale);
  int getCoverArtWidth() const;
  int getCoverArtHeight() const;
  int getCoverArtSpacing() const;
  void refreshCovers();
  void updateCacheSize(int width, int height);

Q_SIGNALS:
  void coverScaleChanged();

private:
  /// The purpose of this cache is to stop us trying to constantly extract memory card icons, when we know a game
  /// doesn't have any saves yet. It caches the serial:memcard_timestamp pair, and only tries extraction when the
  /// timestamp of the memory card has changed.
#pragma pack(push, 1)
  struct MemcardTimestampCacheEntry
  {
    enum : u32
    {
      MAX_SERIAL_LENGTH = 32,
    };

    char serial[MAX_SERIAL_LENGTH];
    s64 memcard_timestamp;
  };
#pragma pack(pop)

  QVariant data(const QModelIndex& index, int role, const GameList::Entry* ge) const;

  void loadCommonImages();
  void loadThemeSpecificImages();
  void setColumnDisplayNames();
  void loadOrGenerateCover(const GameList::Entry* ge);
  void invalidateCoverForPath(const std::string& path);

  const QPixmap& getIconPixmapForEntry(const GameList::Entry* ge) const;
  static void fixIconPixmapSize(QPixmap& pm);

  static QString formatTimespan(time_t timespan);

  std::optional<GameList::EntryList> m_taken_entries;

  float m_cover_scale = 0.0f;
  bool m_show_titles_for_covers = false;
  bool m_show_game_icons = false;

  std::array<QString, Column_Count> m_column_display_names;
  std::array<QPixmap, static_cast<int>(GameList::EntryType::Count)> m_type_pixmaps;
  std::array<QPixmap, static_cast<int>(DiscRegion::Count)> m_region_pixmaps;
  std::array<QPixmap, static_cast<int>(GameDatabase::CompatibilityRating::Count)> m_compatibility_pixmaps;

  QPixmap m_placeholder_pixmap;
  QPixmap m_loading_pixmap;

  mutable LRUCache<std::string, QPixmap> m_cover_pixmap_cache;

  mutable LRUCache<std::string, QPixmap> m_memcard_pixmap_cache;
};
