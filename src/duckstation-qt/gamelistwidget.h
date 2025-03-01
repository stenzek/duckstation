// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_emptygamelistwidget.h"
#include "ui_gamelistwidget.h"

#include "core/game_database.h"
#include "core/game_list.h"
#include "core/types.h"

#include "common/heterogeneous_containers.h"
#include "common/lru_cache.h"

#include <QtCore/QAbstractTableModel>
#include <QtGui/QImage>
#include <QtGui/QPixmap>
#include <QtWidgets/QListView>
#include <QtWidgets/QTableView>

#include <algorithm>
#include <array>
#include <optional>

Q_DECLARE_METATYPE(const GameList::Entry*);

class GameListModel;
class GameListSortModel;
class GameListRefreshThread;

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
    Column_Achievements,
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

  ALWAYS_INLINE const QString& getColumnDisplayName(int column) const { return m_column_display_names[column]; }
  ALWAYS_INLINE const QPixmap& getNoAchievementsPixmap() const { return m_no_achievements_pixmap; }
  ALWAYS_INLINE const QPixmap& getHasAchievementsPixmap() const { return m_has_achievements_pixmap; }
  ALWAYS_INLINE const QPixmap& getMasteredAchievementsPixmap() const { return m_mastered_achievements_pixmap; }

  const GameList::Entry* getTakenGameListEntry(u32 index) const;
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

private Q_SLOTS:
  void coverLoaded(const std::string& path, const QImage& image, float scale);
  void rowsChanged(const QList<int>& rows);

private:
  QVariant data(const QModelIndex& index, int role, const GameList::Entry* ge) const;

  void loadCommonImages();
  void loadThemeSpecificImages();
  void setColumnDisplayNames();
  void loadOrGenerateCover(const GameList::Entry* ge);
  void invalidateCoverForPath(const std::string& path);

  const QPixmap& getIconPixmapForEntry(const GameList::Entry* ge) const;
  const QPixmap& getFlagPixmapForEntry(const GameList::Entry* ge) const;
  static void fixIconPixmapSize(QPixmap& pm);

  static QString formatTimespan(time_t timespan);

  std::optional<GameList::EntryList> m_taken_entries;

  float m_cover_scale = 0.0f;
  bool m_show_titles_for_covers = false;
  bool m_show_game_icons = false;

  std::array<QString, Column_Count> m_column_display_names;
  std::array<QPixmap, static_cast<int>(GameList::EntryType::Count)> m_type_pixmaps;
  std::array<QPixmap, static_cast<int>(GameDatabase::CompatibilityRating::Count)> m_compatibility_pixmaps;

  QImage m_placeholder_image;
  QPixmap m_loading_pixmap;

  QPixmap m_no_achievements_pixmap;
  QPixmap m_has_achievements_pixmap;
  QPixmap m_mastered_achievements_pixmap;

  mutable PreferUnorderedStringMap<QPixmap> m_flag_pixmap_cache;

  mutable LRUCache<std::string, QPixmap> m_cover_pixmap_cache;

  mutable LRUCache<std::string, QPixmap> m_memcard_pixmap_cache;
};

class GameListCoverLoader : public QObject
{
  Q_OBJECT

public:
  GameListCoverLoader(const GameList::Entry* ge, const QImage& placeholder_image, int width, int height, float scale);
  ~GameListCoverLoader();

public:
  void loadOrGenerateCover();

Q_SIGNALS:
  void coverLoaded(const std::string& path, const QImage& image, float scale);

private:
  void createPlaceholderImage();

  std::string m_path;
  std::string m_serial;
  std::string m_title;
  QImage m_placeholder_image;
  int m_width;
  int m_height;
  float m_scale;
  float m_dpr;

  QImage m_image;
};

class GameListGridListView : public QListView
{
  Q_OBJECT

public:
  GameListGridListView(QWidget* parent = nullptr);

Q_SIGNALS:
  void zoomOut();
  void zoomIn();

protected:
  void wheelEvent(QWheelEvent* e);
};

class GameListWidget : public QWidget
{
  Q_OBJECT

public:
  GameListWidget(QWidget* parent = nullptr);
  ~GameListWidget();

  ALWAYS_INLINE GameListModel* getModel() const { return m_model; }

  void initialize();
  void resizeTableViewColumnsToFit();
  void setTableViewColumnHidden(int column, bool hidden);

  void refresh(bool invalidate_cache);
  void refreshModel();
  void cancelRefresh();
  void reloadThemeSpecificImages();

  bool isShowingGameList() const;
  bool isShowingGameGrid() const;
  bool isShowingGridCoverTitles() const;
  bool isMergingDiscSets() const;
  bool isShowingGameIcons() const;

  const GameList::Entry* getSelectedEntry() const;

Q_SIGNALS:
  void refreshProgress(const QString& status, int current, int total);
  void refreshComplete();

  void selectionChanged();
  void entryActivated();
  void entryContextMenuRequested(const QPoint& point);

  void addGameDirectoryRequested();
  void layoutChanged();

private Q_SLOTS:
  void onRefreshProgress(const QString& status, int current, int total, float time);
  void onRefreshComplete();

  void onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous);
  void onTableViewItemActivated(const QModelIndex& index);
  void onTableViewContextMenuRequested(const QPoint& point);
  void onTableViewHeaderContextMenuRequested(const QPoint& point);
  void onTableViewHeaderSortIndicatorChanged(int, Qt::SortOrder);
  void onListViewItemActivated(const QModelIndex& index);
  void onListViewContextMenuRequested(const QPoint& point);
  void onCoverScaleChanged();
  void onSearchReturnPressed();

public Q_SLOTS:
  void showGameList();
  void showGameGrid();
  void setShowCoverTitles(bool enabled);
  void setMergeDiscSets(bool enabled);
  void setShowGameIcons(bool enabled);
  void gridZoomIn();
  void gridZoomOut();
  void gridIntScale(int int_scale);
  void refreshGridCovers();
  void focusSearchWidget();

protected:
  void resizeEvent(QResizeEvent* event);

private:
  void loadTableViewColumnVisibilitySettings();
  void saveTableViewColumnVisibilitySettings();
  void saveTableViewColumnVisibilitySettings(int column);
  void loadTableViewColumnSortSettings();
  void saveTableViewColumnSortSettings();
  void listZoom(float delta);
  void updateToolbar();

  Ui::GameListWidget m_ui;

  GameListModel* m_model = nullptr;
  GameListSortModel* m_sort_model = nullptr;
  QTableView* m_table_view = nullptr;
  GameListGridListView* m_list_view = nullptr;

  QWidget* m_empty_widget = nullptr;
  Ui::EmptyGameListWidget m_empty_ui;

  GameListRefreshThread* m_refresh_thread = nullptr;
};
