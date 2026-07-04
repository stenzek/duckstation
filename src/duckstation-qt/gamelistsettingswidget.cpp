// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistsettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/core.h"
#include "core/game_list.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/string_util.h"

#include <QtCore/QAbstractTableModel>
#include <QtCore/QDebug>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QSettings>
#include <QtCore/QUrl>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <algorithm>

#include "moc_gamelistsettingswidget.cpp"

using namespace Qt::StringLiterals;

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

void GameListSearchDirectoriesModel::reload()
{
  std::vector<std::string> paths = Core::GetBaseStringListSetting("GameList", "Paths");
  std::vector<std::string> recursive_paths = Core::GetBaseStringListSetting("GameList", "RecursivePaths");

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

  Core::SetBaseStringListSettingValue("GameList", "Paths", paths);
  Core::SetBaseStringListSettingValue("GameList", "RecursivePaths", recursive_paths);
  Host::CommitBaseSettingChanges();
  emit settingsChanged();
}

GameListSettingsWidget::GameListSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);

  m_directory_model = new GameListSearchDirectoriesModel(this);
  m_ui.searchDirectoryList->setModel(m_directory_model);
  QtUtils::SetColumnWidthsForTreeView(m_ui.searchDirectoryList, {-1, 120});

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
  connect(m_ui.excludedPaths, &QListWidget::itemSelectionChanged, this,
          &GameListSettingsWidget::onExcludedPathsSelectionChanged);
  connect(m_ui.rescanAllGames, &QPushButton::clicked, this, &GameListSettingsWidget::onRescanAllGamesClicked);
  connect(m_ui.scanForNewGames, &QPushButton::clicked, this, &GameListSettingsWidget::onScanForNewGamesClicked);

  refreshDirectoryList();
  refreshExclusionList();
}

GameListSettingsWidget::~GameListSettingsWidget() = default;

bool GameListSettingsWidget::addExcludedPath(const QString& path)
{
  if (!Core::AddValueToBaseStringListSetting("GameList", "ExcludedPaths", path.toStdString().c_str()))
    return false;

  Host::CommitBaseSettingChanges();
  m_ui.excludedPaths->addItem(path);
  g_main_window->refreshGameList(false);
  return true;
}

void GameListSettingsWidget::refreshExclusionList()
{
  m_ui.excludedPaths->clear();

  const std::vector<std::string> paths(Core::GetBaseStringListSetting("GameList", "ExcludedPaths"));
  for (const std::string& path : paths)
  {
    QListWidgetItem* const it = new QListWidgetItem(QString::fromStdString(path));
    it->setIcon(QIcon(u":/icons/monochrome/svg/file-forbid-line.svg"_s));
    m_ui.excludedPaths->addItem(it);
  }

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

void GameListSettingsWidget::addSearchDirectory(QWidget* parent_widget)
{
  QString dir =
    QDir::toNativeSeparators(QFileDialog::getExistingDirectory(parent_widget, tr("Select Search Directory")));

  if (dir.isEmpty())
    return;

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
  QString path = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select Path")));
  if (path.isEmpty())
    return;

  addExcludedPath(path);
}

void GameListSettingsWidget::onAddExcludedFolderButtonClicked()
{
  QString path = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select Directory")));
  if (path.isEmpty())
    return;

  addExcludedPath(path);
}

void GameListSettingsWidget::onRemoveExcludedPathButtonClicked()
{
  const int row = m_ui.excludedPaths->currentRow();
  QListWidgetItem* item = (row >= 0) ? m_ui.excludedPaths->takeItem(row) : nullptr;
  if (!item)
    return;

  if (Core::RemoveValueFromBaseStringListSetting("GameList", "ExcludedPaths", item->text().toUtf8().constData()))
    Host::CommitBaseSettingChanges();
  delete item;

  g_main_window->refreshGameList(false);
}

void GameListSettingsWidget::onExcludedPathsSelectionChanged()
{
  m_ui.removeExcludedPath->setEnabled(!m_ui.excludedPaths->selectedItems().isEmpty());
}

void GameListSettingsWidget::onRescanAllGamesClicked()
{
  g_main_window->refreshGameList(true);
}

void GameListSettingsWidget::onScanForNewGamesClicked()
{
  g_main_window->refreshGameList(false);
}
