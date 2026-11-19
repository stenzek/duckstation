// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistsettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"

#include "core/core.h"
#include "core/game_list.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/string_util.h"

#include <QtCore/QAbstractTableModel>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QMimeData>
#include <QtCore/QSettings>
#include <QtCore/QUrl>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <algorithm>

#include "moc_gamelistsettingswidget.cpp"

using namespace Qt::StringLiterals;

static QStringList GetDroppedPaths(const QMimeData* data, bool include_files)
{
  QStringList paths;
  for (const QUrl& url : data->urls())
  {
    if (!url.isLocalFile())
      continue;

    const QFileInfo fi(url.toLocalFile());
    if (fi.isDir() || (include_files && fi.isFile()))
      paths.push_back(QDir::toNativeSeparators(QDir::cleanPath(fi.absoluteFilePath())));
  }

  return paths;
}

GameListSearchDirectoriesModel::GameListSearchDirectoriesModel(QObject* parent) : QAbstractTableModel(parent)
{
}

GameListSearchDirectoriesModel::~GameListSearchDirectoriesModel() = default;

int GameListSearchDirectoriesModel::rowCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

int GameListSearchDirectoriesModel::columnCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : 2;
}

QVariant GameListSearchDirectoriesModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
    return {};

  const Row& row = m_rows[index.row()];
  if (index.column() == 0)
  {
    if (role == Qt::DisplayRole)
      return QString::fromStdString(row.path);
    else if (role == Qt::DecorationRole)
    {
      return QIcon(row.recursive ? u":/icons/monochrome/svg/folder-open-line.svg"_s :
                                   u":/icons/monochrome/svg/folder-line.svg"_s);
    }
  }
  else if (index.column() == 1 && role == Qt::CheckStateRole)
  {
    return row.recursive ? Qt::Checked : Qt::Unchecked;
  }

  return {};
}

QVariant GameListSearchDirectoriesModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return {};

  switch (section)
  {
    case 0:
      return tr("Search Directory");
    case 1:
      return tr("Scan Recursively");
    default:
      return {};
  }
}

Qt::ItemFlags GameListSearchDirectoriesModel::flags(const QModelIndex& index) const
{
  // drops are only accepted on the root, i.e. anywhere in the view
  if (!index.isValid())
    return Qt::ItemIsDropEnabled;

  Qt::ItemFlags flags = QAbstractTableModel::flags(index);
  if (index.isValid() && index.column() == 1)
    flags |= Qt::ItemIsUserCheckable;
  return flags;
}

bool GameListSearchDirectoriesModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
  if (!index.isValid() || index.column() != 1 || role != Qt::CheckStateRole || index.row() < 0 ||
      index.row() >= static_cast<int>(m_rows.size()))
  {
    return false;
  }

  Row& row = m_rows[index.row()];
  const bool recursive = (value.toInt() == Qt::Checked);
  if (recursive == row.recursive)
    return false;

  row.recursive = recursive;
  emit dataChanged(this->index(index.row(), 0), this->index(index.row(), 1), {Qt::DecorationRole, Qt::CheckStateRole});
  save();
  return true;
}

Qt::DropActions GameListSearchDirectoriesModel::supportedDropActions() const
{
  return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
}

QStringList GameListSearchDirectoriesModel::mimeTypes() const
{
  return {u"text/uri-list"_s};
}

bool GameListSearchDirectoriesModel::canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                                     const QModelIndex& parent) const
{
  return !GetDroppedPaths(data, false).isEmpty();
}

bool GameListSearchDirectoriesModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                                  const QModelIndex& parent)
{
  QStringList paths = GetDroppedPaths(data, false);
  if (paths.isEmpty())
    return false;

  emit pathsDropped(paths);
  return true;
}

void GameListSearchDirectoriesModel::reload()
{
  std::vector<std::string> paths = Core::GetBaseStringListSetting(GameList::CONFIG_SECTION_NAME, "Paths");
  std::vector<std::string> recursive_paths =
    Core::GetBaseStringListSetting(GameList::CONFIG_SECTION_NAME, "RecursivePaths");

  beginResetModel();
  m_rows.clear();
  m_rows.reserve(paths.size() + recursive_paths.size());
  for (std::string& path : paths)
    m_rows.push_back({std::move(path), false});
  for (std::string& path : recursive_paths)
    m_rows.push_back({std::move(path), true});
  std::sort(m_rows.begin(), m_rows.end(), [](const Row& lhs, const Row& rhs) {
    return QString::localeAwareCompare(QString::fromStdString(lhs.path), QString::fromStdString(rhs.path)) < 0;
  });
  endResetModel();
}

void GameListSearchDirectoriesModel::addPath(std::string path, bool recursive)
{
  const auto existing =
    std::find_if(m_rows.begin(), m_rows.end(), [&path](const Row& row) { return row.path == path; });
  if (existing != m_rows.end())
  {
    const int row = static_cast<int>(std::distance(m_rows.begin(), existing));
    if (existing->recursive != recursive)
    {
      existing->recursive = recursive;
      emit dataChanged(index(row, 0), index(row, 1), {Qt::DecorationRole, Qt::CheckStateRole});
    }
  }
  else
  {
    const auto insert_position =
      std::lower_bound(m_rows.begin(), m_rows.end(), path, [](const Row& row, const std::string& value) {
        return QString::localeAwareCompare(QString::fromStdString(row.path), QString::fromStdString(value)) < 0;
      });
    const int row = static_cast<int>(std::distance(m_rows.begin(), insert_position));
    beginInsertRows({}, row, row);
    m_rows.insert(insert_position, {std::move(path), recursive});
    endInsertRows();
  }

  save();
}

void GameListSearchDirectoriesModel::removePath(const QModelIndex& index)
{
  if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
    return;

  beginRemoveRows({}, index.row(), index.row());
  m_rows.erase(m_rows.begin() + index.row());
  endRemoveRows();
  save();
}

void GameListSearchDirectoriesModel::removePath(const std::string& path)
{
  const auto existing = std::ranges::find_if(m_rows, [&path](const Row& row) { return row.path == path; });
  if (existing != m_rows.end())
  {
    const int row = static_cast<int>(std::distance(m_rows.begin(), existing));
    beginRemoveRows({}, row, row);
    m_rows.erase(existing);
    endRemoveRows();
    save();
  }
}

const std::string& GameListSearchDirectoriesModel::pathForIndex(const QModelIndex& index) const
{
  DebugAssert(index.isValid() && index.row() >= 0 && index.row() < static_cast<int>(m_rows.size()));
  return m_rows[index.row()].path;
}

void GameListSearchDirectoriesModel::save()
{
  std::vector<std::string> paths;
  std::vector<std::string> recursive_paths;
  paths.reserve(m_rows.size());
  recursive_paths.reserve(m_rows.size());
  for (const Row& row : m_rows)
    (row.recursive ? recursive_paths : paths).push_back(row.path);

  Core::SetBaseStringListSettingValue(GameList::CONFIG_SECTION_NAME, "Paths", paths);
  Core::SetBaseStringListSettingValue(GameList::CONFIG_SECTION_NAME, "RecursivePaths", recursive_paths);
  Host::CommitBaseSettingChanges();
  emit settingsChanged();
}

GameListExcludedPathsModel::GameListExcludedPathsModel(QObject* parent) : QStringListModel(parent)
{
}

GameListExcludedPathsModel::~GameListExcludedPathsModel() = default;

QVariant GameListExcludedPathsModel::data(const QModelIndex& index, int role) const
{
  if (role == Qt::DecorationRole && index.isValid())
    return QIcon(u":/icons/monochrome/svg/file-forbid-line.svg"_s);

  return QStringListModel::data(index, role);
}

Qt::ItemFlags GameListExcludedPathsModel::flags(const QModelIndex& index) const
{
  // drops are only accepted on the root, i.e. anywhere in the view
  if (!index.isValid())
    return Qt::ItemIsDropEnabled;

  return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemNeverHasChildren;
}

Qt::DropActions GameListExcludedPathsModel::supportedDropActions() const
{
  return Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
}

QStringList GameListExcludedPathsModel::mimeTypes() const
{
  return {u"text/uri-list"_s};
}

bool GameListExcludedPathsModel::canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                                 const QModelIndex& parent) const
{
  return !GetDroppedPaths(data, true).isEmpty();
}

bool GameListExcludedPathsModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                              const QModelIndex& parent)
{
  QStringList paths = GetDroppedPaths(data, true);
  if (paths.isEmpty())
    return false;

  emit pathsDropped(paths);
  return true;
}

GameListSettingsWidget::GameListSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);

  m_directory_model = new GameListSearchDirectoriesModel(this);
  m_ui.searchDirectoryList->setModel(m_directory_model);
  QtUtils::SetColumnWidthsForTreeView(m_ui.searchDirectoryList, {-1, 120});
  m_excluded_paths_model = new GameListExcludedPathsModel(this);
  m_ui.excludedPaths->setModel(m_excluded_paths_model);

  connect(m_ui.searchDirectoryList->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          &GameListSettingsWidget::onDirectoryListSelectionChanged);
  connect(m_ui.searchDirectoryList, &QTreeView::customContextMenuRequested, this,
          &GameListSettingsWidget::onDirectoryListContextMenuRequested);
  connect(m_directory_model, &GameListSearchDirectoriesModel::settingsChanged, this,
          []() { g_main_window->refreshGameList(false); });
  connect(m_ui.addSearchDirectoryButton, &QPushButton::clicked, this,
          &GameListSettingsWidget::onAddSearchDirectoryButtonClicked);
  connect(m_ui.removeSearchDirectoryButton, &QPushButton::clicked, this,
          &GameListSettingsWidget::onRemoveSearchDirectoryButtonClicked);
  connect(m_ui.addExcludedFile, &QPushButton::clicked, this, &GameListSettingsWidget::onAddExcludedFileButtonClicked);
  connect(m_ui.addExcludedFolder, &QPushButton::clicked, this,
          &GameListSettingsWidget::onAddExcludedFolderButtonClicked);
  connect(m_ui.removeExcludedPath, &QPushButton::clicked, this,
          &GameListSettingsWidget::onRemoveExcludedPathButtonClicked);
  connect(m_ui.excludedPaths->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          &GameListSettingsWidget::onExcludedPathsSelectionChanged);
  connect(m_ui.excludedPaths, &QListView::customContextMenuRequested, this,
          &GameListSettingsWidget::onExcludedPathsContextMenuRequested);
  connect(m_ui.rescanAllGames, &QPushButton::clicked, this, &GameListSettingsWidget::onRescanAllGamesClicked);
  connect(m_ui.scanForNewGames, &QPushButton::clicked, this, &GameListSettingsWidget::onScanForNewGamesClicked);

  // queued, so we're not showing message boxes from inside the drop handler
  connect(
    m_directory_model, &GameListSearchDirectoriesModel::pathsDropped, this,
    [this](const QStringList& paths) {
      for (const QString& path : paths)
        promptAndAddSearchDirectory(path);
    },
    Qt::QueuedConnection);
  connect(
    m_excluded_paths_model, &GameListExcludedPathsModel::pathsDropped, this,
    [this](const QStringList& paths) {
      for (const QString& path : paths)
        addExcludedPath(path);
    },
    Qt::QueuedConnection);

  // used to draw placeholder text when the lists are empty
  m_ui.searchDirectoryList->viewport()->installEventFilter(this);
  m_ui.excludedPaths->viewport()->installEventFilter(this);

  dialog->registerWidgetHelp(
    m_ui.searchDirectoryList, tr("Search Directories"), QString(),
    tr("Directories which will be scanned for games. Enable Scan Recursively to also search subdirectories, which "
       "takes longer but finds games stored in nested folders. Folders can also be dragged here from your file "
       "manager."));
  dialog->registerWidgetHelp(
    m_ui.excludedPaths, tr("Excluded Paths"), QString(),
    tr("Files and directories which will be skipped when scanning for games, for example images you do not want to "
       "appear in the game list. Files and folders can also be dragged here from your file manager."));
  dialog->registerWidgetHelp(m_ui.scanForNewGames, tr("Scan For New Games"), QString(),
                             tr("Scans the search directories for files which are not already in the game list. "
                                "Previously-scanned games are loaded from the cache, so this is usually quick."));
  dialog->registerWidgetHelp(
    m_ui.rescanAllGames, tr("Rescan All Games"), QString(),
    tr("Discards the game list cache and scans every file in the search directories again. Use this if game details "
       "such as titles or serials are incorrect. May take a while for large collections."));

  refreshDirectoryList();
  refreshExclusionList();
}

GameListSettingsWidget::~GameListSettingsWidget() = default;

bool GameListSettingsWidget::addExcludedPath(const QString& path)
{
  if (!Core::AddValueToBaseStringListSetting(GameList::CONFIG_SECTION_NAME, "ExcludedPaths",
                                             path.toStdString().c_str()))
    return false;

  Host::CommitBaseSettingChanges();

  const int row = m_excluded_paths_model->rowCount();
  m_excluded_paths_model->insertRow(row);
  m_excluded_paths_model->setData(m_excluded_paths_model->index(row), path);

  g_main_window->refreshGameList(false);
  return true;
}

void GameListSettingsWidget::refreshExclusionList()
{
  QStringList paths;
  for (const std::string& path : Core::GetBaseStringListSetting(GameList::CONFIG_SECTION_NAME, "ExcludedPaths"))
    paths.push_back(QString::fromStdString(path));
  m_excluded_paths_model->setStringList(paths);

  m_ui.removeExcludedPath->setEnabled(false);
}

void GameListSettingsWidget::refreshDirectoryList()
{
  m_directory_model->reload();
  m_ui.removeSearchDirectoryButton->setEnabled(false);
}

void GameListSettingsWidget::addSearchDirectory(const QString& path, bool recursive)
{
  m_directory_model->addPath(path.toStdString(), recursive);
}

void GameListSettingsWidget::onDirectoryListSelectionChanged()
{
  m_ui.removeSearchDirectoryButton->setEnabled(m_ui.searchDirectoryList->selectionModel()->hasSelection());
}

void GameListSettingsWidget::onDirectoryListContextMenuRequested(const QPoint& point)
{
  const QModelIndex index = m_ui.searchDirectoryList->currentIndex();
  if (!index.isValid())
    return;

  const QString path = QString::fromStdString(m_directory_model->pathForIndex(index));

  QMenu* const menu = QtUtils::NewPopupMenu(this);
  menu->addAction(QIcon(u":/icons/monochrome/svg/folder-reduce-line.svg"_s), tr("Remove"), this,
                  [this, path]() { m_directory_model->removePath(path.toStdString()); });
  menu->addSeparator();
  menu->addAction(QIcon(u":/icons/monochrome/svg/folder-open-line.svg"_s), tr("Open Directory..."),
                  [this, path]() { QtUtils::OpenURL(this, QUrl::fromLocalFile(path)); });
  menu->popup(m_ui.searchDirectoryList->mapToGlobal(point));
}

bool GameListSettingsWidget::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Paint)
  {
    if (object == m_ui.searchDirectoryList->viewport())
    {
      if (QtUtils::DrawItemViewPlaceholderText(
            m_ui.searchDirectoryList,
            tr("No search directories. Add a folder, or drag one here, to choose where your games are stored.")))
      {
        return true;
      }
    }
    else if (object == m_ui.excludedPaths->viewport())
    {
      if (QtUtils::DrawItemViewPlaceholderText(
            m_ui.excludedPaths, tr("No excluded paths. Files or folders added here will be skipped when scanning.")))
      {
        return true;
      }
    }
  }

  return QWidget::eventFilter(object, event);
}

void GameListSettingsWidget::addSearchDirectory(QWidget* parent_widget)
{
  const QString dir = QDir::toNativeSeparators(QDir::cleanPath(
    QDir::fromNativeSeparators(QFileDialog::getExistingDirectory(parent_widget, tr("Select Search Directory")))));

  if (dir.isEmpty())
    return;

  promptAndAddSearchDirectory(dir);
}

void GameListSettingsWidget::promptAndAddSearchDirectory(const QString& dir)
{
  QMessageBox::StandardButton selection = QtUtils::MessageBoxQuestion(
    this, tr("Scan Recursively?"),
    tr("Would you like to scan the directory \"%1\" recursively?\n\nScanning recursively takes "
       "more time, but will identify files in subdirectories.")
      .arg(dir),
    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (selection != QMessageBox::Yes && selection != QMessageBox::No)
    return;

  const bool recursive = (selection == QMessageBox::Yes);
  addSearchDirectory(dir, recursive);
}

void GameListSettingsWidget::onAddSearchDirectoryButtonClicked()
{
  addSearchDirectory(this);
}

void GameListSettingsWidget::onRemoveSearchDirectoryButtonClicked()
{
  const QModelIndex index = m_ui.searchDirectoryList->currentIndex();
  if (!index.isValid())
    return;

  m_directory_model->removePath(index);
}

void GameListSettingsWidget::onAddExcludedFileButtonClicked()
{
  const QString path = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select Path")));
  if (path.isEmpty())
    return;

  addExcludedPath(path);
}

void GameListSettingsWidget::onAddExcludedFolderButtonClicked()
{
  const QString path = QDir::toNativeSeparators(
    QDir::cleanPath(QDir::fromNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select Directory")))));
  if (path.isEmpty())
    return;

  addExcludedPath(path);
}

void GameListSettingsWidget::onRemoveExcludedPathButtonClicked()
{
  QModelIndexList indices = m_ui.excludedPaths->selectionModel()->selectedRows();
  if (indices.isEmpty())
    return;

  // remove from the bottom up, so the row numbers stay valid
  std::ranges::sort(indices, [](const QModelIndex& lhs, const QModelIndex& rhs) { return lhs.row() > rhs.row(); });

  bool changed = false;
  for (const QModelIndex& index : indices)
  {
    changed |= Core::RemoveValueFromBaseStringListSetting(GameList::CONFIG_SECTION_NAME, "ExcludedPaths",
                                                          index.data().toString().toUtf8().constData());
    m_excluded_paths_model->removeRow(index.row());
  }

  if (changed)
    Host::CommitBaseSettingChanges();

  g_main_window->refreshGameList(false);
}

void GameListSettingsWidget::onExcludedPathsSelectionChanged()
{
  m_ui.removeExcludedPath->setEnabled(m_ui.excludedPaths->selectionModel()->hasSelection());
}

void GameListSettingsWidget::onExcludedPathsContextMenuRequested(const QPoint& point)
{
  QMenu* const menu = QtUtils::NewPopupMenu(this);
  if (m_ui.excludedPaths->selectionModel()->hasSelection())
  {
    menu->addAction(QIcon(u":/icons/monochrome/svg/folder-reduce-line.svg"_s), tr("Remove"), this,
                    &GameListSettingsWidget::onRemoveExcludedPathButtonClicked);
    menu->addSeparator();
  }

  menu->addAction(QIcon(u":/icons/monochrome/svg/file-add-line.svg"_s), tr("Add File..."), this,
                  &GameListSettingsWidget::onAddExcludedFileButtonClicked);
  menu->addAction(QIcon(u":/icons/monochrome/svg/folder-add-line.svg"_s), tr("Add Folder..."), this,
                  &GameListSettingsWidget::onAddExcludedFolderButtonClicked);
  menu->popup(m_ui.excludedPaths->mapToGlobal(point));
}

void GameListSettingsWidget::onRescanAllGamesClicked()
{
  g_main_window->refreshGameList(true);
}

void GameListSettingsWidget::onScanForNewGamesClicked()
{
  g_main_window->refreshGameList(false);
}
