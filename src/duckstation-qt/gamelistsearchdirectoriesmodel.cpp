#include "gamelistsearchdirectoriesmodel.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include <QtCore/QUrl>

GameListSearchDirectoriesModel::GameListSearchDirectoriesModel(EmuThread* host_interface)
  : m_host_interface(host_interface)
{
  loadFromSettings();
}

GameListSearchDirectoriesModel::~GameListSearchDirectoriesModel() = default;

int GameListSearchDirectoriesModel::columnCount(const QModelIndex& parent) const
{
  if (parent.isValid())
    return 0;

  return 2;
}

QVariant GameListSearchDirectoriesModel::headerData(int section, Qt::Orientation orientation,
                                                    int role /*= Qt::DisplayRole*/) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return {};

  if (section == 0)
    return tr("Path");
  else
    return tr("Recursive");
}

int GameListSearchDirectoriesModel::rowCount(const QModelIndex& parent) const
{
  if (parent.isValid())
    return 0;

  return static_cast<int>(m_entries.size());
}

QVariant GameListSearchDirectoriesModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  if (!index.isValid())
    return {};

  const int row = index.row();
  const int column = index.column();
  if (row < 0 || row >= static_cast<int>(m_entries.size()))
    return {};

  const Entry& entry = m_entries[row];
  if (role == Qt::CheckStateRole)
  {
    if (column == 1)
      return entry.recursive ? Qt::Checked : Qt::Unchecked;
  }
  else if (role == Qt::DisplayRole)
  {
    if (column == 0)
      return entry.path;
  }

  return {};
}

bool GameListSearchDirectoriesModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
  if (!index.isValid())
    return false;

  const int row = index.row();
  const int column = index.column();
  if (row < 0 || row >= static_cast<int>(m_entries.size()))
    return false;

  if (column != 1 || role == Qt::CheckStateRole)
    return false;

  Entry& entry = m_entries[row];
  entry.recursive = value == Qt::Checked;
  saveToSettings();
  g_main_window->refreshGameList(false);
  return true;
}

void GameListSearchDirectoriesModel::addEntry(const QString& path, bool recursive)
{
  auto existing = std::find_if(m_entries.begin(), m_entries.end(), [path](const Entry& e) { return e.path == path; });
  if (existing != m_entries.end())
  {
    const int row = static_cast<int>(existing - m_entries.begin());
    existing->recursive = recursive;
    dataChanged(index(row, 1), index(row, 1), QVector<int>{Qt::CheckStateRole});
  }
  else
  {
    beginInsertRows(QModelIndex(), static_cast<int>(m_entries.size()), static_cast<int>(m_entries.size()));
    m_entries.push_back({path, recursive});
    endInsertRows();
  }

  saveToSettings();
  g_main_window->refreshGameList(false);
}

void GameListSearchDirectoriesModel::removeEntry(int row)
{
  if (row < 0 || row >= static_cast<int>(m_entries.size()))
    return;

  beginRemoveRows(QModelIndex(), row, row);
  m_entries.erase(m_entries.begin() + row);
  endRemoveRows();

  saveToSettings();
  g_main_window->refreshGameList(false);
}

bool GameListSearchDirectoriesModel::isEntryRecursive(int row) const
{
  return (row < 0 || row >= static_cast<int>(m_entries.size())) ? false : m_entries[row].recursive;
}

void GameListSearchDirectoriesModel::setEntryRecursive(int row, bool recursive)
{
  if (row < 0 || row >= static_cast<int>(m_entries.size()))
    return;

  m_entries[row].recursive = recursive;
  emit dataChanged(index(row, 1), index(row, 1), {Qt::CheckStateRole});

  saveToSettings();
  g_main_window->refreshGameList(false);
}

void GameListSearchDirectoriesModel::openEntryInExplorer(QWidget* parent, int row) const
{
  if (row < 0 || row >= static_cast<int>(m_entries.size()))
    return;

  QtUtils::OpenURL(parent, QUrl::fromLocalFile(m_entries[row].path));
}

void GameListSearchDirectoriesModel::loadFromSettings()
{
  std::vector<std::string> path_list = Host::GetBaseStringListSetting("GameList", "Paths");
  for (std::string& entry : path_list)
    m_entries.push_back({QString::fromStdString(entry), false});

  path_list = Host::GetBaseStringListSetting("GameList", "RecursivePaths");
  for (std::string& entry : path_list)
    m_entries.push_back({QString::fromStdString(entry), true});
}

void GameListSearchDirectoriesModel::saveToSettings()
{
  std::vector<std::string> paths;
  std::vector<std::string> recursive_paths;

  for (const Entry& entry : m_entries)
  {
    if (entry.recursive)
      recursive_paths.push_back(entry.path.toStdString());
    else
      paths.push_back(entry.path.toStdString());
  }

  if (paths.empty())
    Host::DeleteBaseSettingValue("GameList", "Paths");
  else
    Host::SetBaseStringListSettingValue("GameList", "Paths", paths);

  if (recursive_paths.empty())
    Host::DeleteBaseSettingValue("GameList", "RecursivePaths");
  else
    Host::SetBaseStringListSettingValue("GameList", "RecursivePaths", recursive_paths);
}
