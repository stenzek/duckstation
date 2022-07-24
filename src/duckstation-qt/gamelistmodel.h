#pragma once
#include "common/heterogeneous_containers.h"
#include "common/lru_cache.h"
#include "core/game_database.h"
#include "core/types.h"
#include "frontend-common/game_list.h"
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
    Column_Type,
    Column_Serial,
    Column_Title,
    Column_FileTitle,
    Column_Developer,
    Column_Publisher,
    Column_Genre,
    Column_Year,
    Column_Players,
    Column_Size,
    Column_Region,
    Column_Compatibility,
    Column_Cover,

    Column_Count
  };

  static std::optional<Column> getColumnIdForName(std::string_view name);
  static const char* getColumnName(Column col);

  GameListModel(QObject* parent = nullptr);
  ~GameListModel();

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  ALWAYS_INLINE const QString& getColumnDisplayName(int column) { return m_column_display_names[column]; }

  void refresh();

  bool titlesLessThan(int left_row, int right_row) const;

  bool lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column) const;

  bool getShowCoverTitles() const { return m_show_titles_for_covers; }
  void setShowCoverTitles(bool enabled) { m_show_titles_for_covers = enabled; }

  float getCoverScale() const { return m_cover_scale; }
  void setCoverScale(float scale);
  int getCoverArtWidth() const;
  int getCoverArtHeight() const;
  int getCoverArtSpacing() const;
  void refreshCovers();
  void updateCacheSize(int width, int height);
  void reloadCommonImages();

private:
  void loadCommonImages();
  void setColumnDisplayNames();
  void loadOrGenerateCover(const GameList::Entry* ge);
  void invalidateCoverForPath(const std::string& path);

  float m_cover_scale = 0.0f;
  bool m_show_titles_for_covers = false;

  std::array<QString, Column_Count> m_column_display_names;
  std::array<QPixmap, static_cast<int>(GameList::EntryType::Count)> m_type_pixmaps;
  std::array<QPixmap, static_cast<int>(DiscRegion::Count)> m_region_pixmaps;
  std::array<QPixmap, static_cast<int>(GameDatabase::CompatibilityRating::Count)> m_compatibility_pixmaps;

  QPixmap m_placeholder_pixmap;
  QPixmap m_loading_pixmap;

  mutable LRUCache<std::string, QPixmap> m_cover_pixmap_cache;
};