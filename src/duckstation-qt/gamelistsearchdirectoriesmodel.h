#pragma once
#include <QtCore/QAbstractTableModel>
#include <QtCore/QString>
#include <vector>

class EmuThread;

class GameListSearchDirectoriesModel : public QAbstractTableModel
{
  Q_OBJECT

public:
  GameListSearchDirectoriesModel(EmuThread* host_interface);
  ~GameListSearchDirectoriesModel();

  int columnCount(const QModelIndex& parent) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  int rowCount(const QModelIndex& parent) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role) override;

  void addEntry(const QString& path, bool recursive);
  void removeEntry(int row);

  bool isEntryRecursive(int row) const;
  void setEntryRecursive(int row, bool recursive);

  void openEntryInExplorer(QWidget* parent, int row) const;
  void loadFromSettings();
  void saveToSettings();

private:
  struct Entry
  {
    QString path;
    bool recursive;
  };

  EmuThread* m_host_interface;
  std::vector<Entry> m_entries;
};
