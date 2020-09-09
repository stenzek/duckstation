#pragma once
#include "core/types.h"
#include "frontend-common/game_list.h"
#include <QtCore/QAbstractTableModel>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <array>
#include <optional>

class GameListModel final : public QAbstractTableModel
{
  Q_OBJECT

public:
  enum Column : int
  {
    Column_Type,
    Column_Code,
    Column_Title,
    Column_FileTitle,
    Column_Size,
    Column_Region,
    Column_Compatibility,

    Column_Count
  };

  static std::optional<Column> getColumnIdForName(std::string_view name);
  static const char* getColumnName(Column col);

  GameListModel(GameList* game_list, QObject* parent = nullptr);
  ~GameListModel();

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  ALWAYS_INLINE const QString& getColumnDisplayName(int column) { return m_column_display_names[column]; }

  void refresh();

  bool titlesLessThan(int left_row, int right_row, bool ascending) const;

  bool lessThan(const QModelIndex& left_index, const QModelIndex& right_index, int column, bool ascending) const;

private:
  void loadCommonImages();
  void setColumnDisplayNames();

  GameList* m_game_list;

  std::array<QString, Column_Count> m_column_display_names;

  QPixmap m_type_disc_pixmap;
  QPixmap m_type_exe_pixmap;
  QPixmap m_type_playlist_pixmap;

  QIcon m_region_jp_pixmap;
  QIcon m_region_eu_pixmap;
  QIcon m_region_us_pixmap;

  std::array<QPixmap, static_cast<int>(GameListCompatibilityRating::Count)> m_compatibiliy_pixmaps;
};