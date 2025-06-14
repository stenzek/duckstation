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

  explicit GameListModel(QObject* parent);
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
  int getCoverArtSize() const;
  int getCoverArtSpacing() const;
  void refreshCovers();
  void updateCacheSize(int num_rows, int num_columns);

Q_SIGNALS:
  void coverScaleChanged(float scale);

private Q_SLOTS:
  void rowsChanged(const QList<int>& rows);

private:
  QVariant data(const QModelIndex& index, int role, const GameList::Entry* ge) const;

  void loadCommonImages();
  void loadThemeSpecificImages();
  void setColumnDisplayNames();
  void updateCoverScale();
  void loadOrGenerateCover(const GameList::Entry* ge);
  void invalidateCoverForPath(const std::string& path);
  void coverLoaded(const std::string& path, const QImage& image, float scale);

  static void loadOrGenerateCover(QImage& image, const QImage& placeholder_image, int width, int height, float scale,
                                  float dpr, const std::string& path, const std::string& serial,
                                  const std::string& title);
  static void createPlaceholderImage(QImage& image, const QImage& placeholder_image, int width, int height, float scale,
                                     const std::string& title);

  const QPixmap& getIconPixmapForEntry(const GameList::Entry* ge) const;
  const QPixmap& getFlagPixmapForEntry(const GameList::Entry* ge) const;
  static void fixIconPixmapSize(QPixmap& pm);

  std::optional<GameList::EntryList> m_taken_entries;

  float m_cover_scale = 0.0f;
  bool m_show_titles_for_covers = false;
  bool m_show_game_icons = false;

  std::array<QString, Column_Count> m_column_display_names;
  std::array<QPixmap, static_cast<int>(GameList::EntryType::MaxCount)> m_type_pixmaps;
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

class GameListListView final : public QTableView
{
  Q_OBJECT

public:
  GameListListView(GameListModel* model, GameListSortModel* sort_model, QWidget* parent);
  ~GameListListView() override;

  void setAndSaveColumnHidden(int column, bool hidden);

  void resizeColumnsToFit();

protected:
  void resizeEvent(QResizeEvent* e) override;

private Q_SLOTS:
  void onHeaderSortIndicatorChanged(int, Qt::SortOrder);
  void onHeaderContextMenuRequested(const QPoint& point);

private:
  void loadColumnVisibilitySettings();
  void loadColumnSortSettings();
  void saveColumnSortSettings();

  GameListModel* m_model = nullptr;
  GameListSortModel* m_sort_model = nullptr;
};

class GameListGridView final : public QListView
{
  Q_OBJECT

public:
  GameListGridView(GameListModel* model, GameListSortModel* sort_model, QWidget* parent);
  ~GameListGridView() override;

  void updateLayout();
  int horizontalOffset() const override;
  int verticalOffset() const override;

public Q_SLOTS:
  void zoomOut();
  void zoomIn();
  void setZoomPct(int int_scale);

protected:
  void wheelEvent(QWheelEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;

private Q_SLOTS:
  void onCoverScaleChanged(float scale);

private:
  void adjustZoom(float delta);

  GameListModel* m_model = nullptr;
  int m_horizontal_offset = 0;
  int m_vertical_offset = 0;
};

class GameListWidget final : public QWidget
{
  Q_OBJECT

public:
  GameListWidget(QWidget* parent = nullptr);
  ~GameListWidget();

  ALWAYS_INLINE GameListModel* getModel() const { return m_model; }
  ALWAYS_INLINE GameListListView* getListView() const { return m_list_view; }
  ALWAYS_INLINE GameListGridView* getGridView() const { return m_grid_view; }

  void initialize();
  void resizeListViewColumnsToFit();

  void refresh(bool invalidate_cache);
  void refreshModel();
  void cancelRefresh();
  void reloadThemeSpecificImages();
  void updateBackground(bool reload_image);

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

  void onCoverScaleChanged(float scale);

  void onSelectionModelCurrentChanged(const QModelIndex& current, const QModelIndex& previous);
  void onListViewItemActivated(const QModelIndex& index);
  void onListViewContextMenuRequested(const QPoint& point);
  void onGridViewItemActivated(const QModelIndex& index);
  void onGridViewContextMenuRequested(const QPoint& point);
  void onSearchReturnPressed();

public Q_SLOTS:
  void showGameList();
  void showGameGrid();
  void setShowCoverTitles(bool enabled);
  void setMergeDiscSets(bool enabled);
  void setShowGameIcons(bool enabled);
  void refreshGridCovers();
  void focusSearchWidget();

protected:
  void resizeEvent(QResizeEvent* event);

private:
  void updateToolbar();

  Ui::GameListWidget m_ui;

  GameListModel* m_model = nullptr;
  GameListSortModel* m_sort_model = nullptr;
  GameListListView* m_list_view = nullptr;
  GameListGridView* m_grid_view = nullptr;

  QWidget* m_empty_widget = nullptr;
  Ui::EmptyGameListWidget m_empty_ui;

  GameListRefreshThread* m_refresh_thread = nullptr;

  QImage m_background_image;
};
