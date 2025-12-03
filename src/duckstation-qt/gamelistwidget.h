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

class QStyledItemDelegate;

class GameListSortModel;
class GameListRefreshThread;
class GameListWidget;

class GameListModel final : public QAbstractTableModel
{
  Q_OBJECT

  friend GameListWidget;

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
    Column_DataSize,
    Column_Region,
    Column_Achievements,
    Column_Compatibility,
    Column_Cover,

    Column_Count,

    Column_LastVisible = Column_Compatibility,
  };

  static std::optional<Column> getColumnIdForName(std::string_view name);
  static const char* getColumnName(Column col);

  explicit GameListModel(GameListWidget* parent);
  ~GameListModel();

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  std::pair<std::unique_lock<std::recursive_mutex>, const GameList::Entry*>
  getEntryForIndex(const QModelIndex& index) const;

  const QPixmap& getNoAchievementsPixmap() const;
  const QPixmap& getHasAchievementsPixmap() const;
  const QPixmap& getMasteredAchievementsPixmap() const;

  const GameList::Entry* getTakenGameListEntry(u32 index) const;
  bool hasTakenGameList() const;
  void takeGameList();

  void refresh();
  void reloadThemeSpecificImages();

  bool titlesLessThan(const GameList::Entry* left, const GameList::Entry* right) const;
  bool lessThan(const GameList::Entry* left, const GameList::Entry* right, int column) const;
  bool lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column) const;

  bool getShowLocalizedTitles() const;
  void setShowLocalizedTitles(bool enabled);

  bool getShowCoverTitles() const;
  void setShowCoverTitles(bool enabled);

  int getRowHeight() const;

  int getIconSize() const;
  int getIconSizeWithPadding() const;
  void refreshIcons();
  void setIconSize(int size);

  bool getShowGameIcons() const;
  void setShowGameIcons(bool enabled);
  QIcon getIconForGame(const QString& path);

  float getCoverScale() const;
  void setCoverScale(float scale);
  QSize getCoverArtSize() const;
  QSize getCoverArtItemSize() const;
  QSize getDeviceScaledCoverArtSize() const;
  int getCoverArtSpacing() const;
  QFont getCoverCaptionFont() const;
  void refreshCovers();
  void updateCacheSize(int num_rows, int num_columns, QSortFilterProxyModel* const sort_model, int top_left_row);

  qreal getDevicePixelRatio() const;
  void setDevicePixelRatio(qreal dpr);

  const QPixmap* lookupIconPixmapForEntry(const GameList::Entry* ge) const;

  const QPixmap& getCoverForEntry(const GameList::Entry* ge) const;
  void invalidateCoverCacheForPath(const std::string& path);

Q_SIGNALS:
  void coverScaleChanged(float scale);
  void iconSizeChanged(int size);

private:
  struct CoverPixmapCacheEntry
  {
    QPixmap pixmap;
    float scale;
    bool is_loading;
  };

  void rowsChanged(const QList<int>& rows);

  void loadCommonImages();
  void loadSizeDependentPixmaps();
  void updateCoverScale();
  void loadCoverScaleDependentPixmaps();
  void loadOrGenerateCover(const GameList::Entry* ge);
  void invalidateCoverForPath(const std::string& path);
  void coverLoaded(const std::string& path, const QImage& image, float scale);

  static void loadOrGenerateCover(QImage& image, const QImage& placeholder_image, const QSize& size, float scale,
                                  qreal dpr, const std::string& path, const std::string& serial,
                                  const std::string& save_title, const QString& display_title, bool is_custom_title);
  static void createPlaceholderImage(QImage& image, const QImage& placeholder_image, const QSize& size, float scale,
                                     const QString& title);

  const QPixmap& getIconPixmapForEntry(const GameList::Entry* ge) const;
  const QPixmap& getFlagPixmapForEntry(const GameList::Entry* ge) const;

  qreal m_device_pixel_ratio = 1.0;

  std::optional<GameList::EntryList> m_taken_entries;

  float m_cover_scale = 0.0f;
  int m_icon_size = 0;
  bool m_show_localized_titles = false;
  bool m_show_titles_for_covers = false;
  bool m_show_game_icons = false;

  std::array<QPixmap, static_cast<int>(GameList::EntryType::MaxCount)> m_type_pixmaps;
  std::array<QPixmap, static_cast<int>(GameDatabase::CompatibilityRating::Count)> m_compatibility_pixmaps;

  QImage m_placeholder_image;
  QPixmap m_loading_pixmap;

  QPixmap m_no_achievements_pixmap;
  QPixmap m_has_achievements_pixmap;
  QPixmap m_mastered_achievements_pixmap;

  mutable PreferUnorderedStringMap<QPixmap> m_flag_pixmap_cache;

  mutable LRUCache<std::string, QPixmap> m_icon_pixmap_cache;

  mutable LRUCache<std::string, CoverPixmapCacheEntry> m_cover_pixmap_cache;
};

class GameListListView final : public QTableView
{
  Q_OBJECT

public:
  GameListListView(GameListModel* model, GameListSortModel* sort_model, QWidget* parent);
  ~GameListListView() override;

  QFontMetrics fontMetricsForHorizontalHeader() const;
  void setFixedColumnWidth(const QFontMetrics& fm, int column, int str_width);
  void setAndSaveColumnHidden(int column, bool hidden);
  void updateFixedColumnWidths();

  void adjustIconSize(int delta);

  bool isAnimatingGameIcons() const;
  void setAnimateGameIcons(bool enabled);
  void updateAnimatedGameIconDelegate();
  void clearAnimatedGameIconDelegate();

protected:
  void wheelEvent(QWheelEvent* e) override;

private:
  void loadColumnVisibilitySettings();
  void loadColumnSortSettings();

  void onHeaderContextMenuRequested(const QPoint& point);
  void saveColumnSortSettings();

  GameListModel* m_model = nullptr;
  GameListSortModel* m_sort_model = nullptr;

  QStyledItemDelegate* m_animated_game_icon_delegate = nullptr;
  int m_animated_icon_row = -1;
};

class GameListGridView final : public QListView
{
  Q_OBJECT

public:
  GameListGridView(GameListModel* model, GameListSortModel* sort_model, QWidget* parent);
  ~GameListGridView() override;

  int horizontalOffset() const override;
  int verticalOffset() const override;

  void adjustZoom(float delta);

  void updateLayout();

protected:
  void wheelEvent(QWheelEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;

private:
  GameListModel* m_model = nullptr;
  GameListSortModel* m_sort_model = nullptr;
  int m_horizontal_offset = 0;
  int m_vertical_offset = 0;
};

class GameListWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit GameListWidget(QWidget* parent, QAction* action_view_list, QAction* action_view_grid,
                          QAction* action_merge_disc_sets, QAction* action_show_list_icons,
                          QAction* action_animate_list_icons, QAction* action_prefer_achievement_game_icons,
                          QAction* action_show_grid_titles, QAction* action_show_localized_titles);
  ~GameListWidget();

  ALWAYS_INLINE GameListModel* getModel() const { return m_model; }
  ALWAYS_INLINE GameListListView* getListView() const { return m_list_view; }
  ALWAYS_INLINE GameListGridView* getGridView() const { return m_grid_view; }

  void refresh(bool invalidate_cache);
  void cancelRefresh();
  void setBackgroundPath(const std::string_view path);
  bool hasBackground() const;

  bool isShowingGameList() const;
  bool isShowingGameGrid() const;

  void zoomOut();
  void zoomIn();

  const GameList::Entry* getSelectedEntry() const;

  void showGameList();
  void showGameGrid();
  void setMergeDiscSets(bool enabled);
  void setShowLocalizedTitles(bool enabled);
  void setShowGameIcons(bool enabled);
  void setAnimateGameIcons(bool enabled);
  void setPreferAchievementGameIcons(bool enabled);
  void downloadAllGameIcons();
  void setShowCoverTitles(bool enabled);
  void refreshGridCovers();
  void focusSearchWidget();

Q_SIGNALS:
  void refreshProgress(const QString& status, int current, int total);
  void refreshComplete();

  void selectionChanged();
  void entryActivated();
  void entryContextMenuRequested(const QPoint& point);

  void addGameDirectoryRequested();

protected:
  bool event(QEvent* e) override;

private:
  void setViewMode(int stack_index);
  void updateBackground(bool reload_image);

  void onRefreshProgress(const QString& status, int current, int total, int entry_count, float time);
  void onRefreshComplete();

  void onThemeChanged();

  void showScaleToolTip();
  void onScaleSliderChanged(int value);
  void onScaleChanged();
  void onIconSizeChanged(int size);

  void onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous);
  void onListViewItemActivated(const QModelIndex& index);
  void onListViewContextMenuRequested(const QPoint& point);
  void onGridViewItemActivated(const QModelIndex& index);
  void onGridViewContextMenuRequested(const QPoint& point);
  void onSearchReturnPressed();

  Ui::GameListWidget m_ui;

  GameListModel* m_model = nullptr;
  GameListSortModel* m_sort_model = nullptr;
  GameListListView* m_list_view = nullptr;
  GameListGridView* m_grid_view = nullptr;

  QWidget* m_empty_widget = nullptr;
  Ui::EmptyGameListWidget m_empty_ui;

  QImage m_background_image;

  GameListRefreshThread* m_refresh_thread = nullptr;
  int m_refresh_last_entry_count = 0;
};
