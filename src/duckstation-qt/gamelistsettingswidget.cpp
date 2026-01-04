// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
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
#include <QtCore/QSettings>
#include <QtCore/QUrl>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <algorithm>

#include "moc_gamelistsettingswidget.cpp"

GameListSettingsWidget::GameListSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);

  QtUtils::SetColumnWidthsForTreeView(m_ui.searchDirectoryList, {-1, 120});

  connect(m_ui.searchDirectoryList, &QTreeWidget::itemSelectionChanged, this,
          &GameListSettingsWidget::onDirectoryListSelectionChanged);
  connect(m_ui.searchDirectoryList, &QTreeWidget::itemChanged, this,
          &GameListSettingsWidget::onDirectoryListItemChanged);
  connect(m_ui.searchDirectoryList, &QTreeWidget::customContextMenuRequested, this,
          &GameListSettingsWidget::onDirectoryListContextMenuRequested);
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
    m_ui.excludedPaths->addItem(QString::fromStdString(path));

  m_ui.removeExcludedPath->setEnabled(false);
}

void GameListSettingsWidget::addPathToTable(const std::string& path, bool recursive)
{
  QTreeWidgetItem* const item = new QTreeWidgetItem();
  item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
  item->setText(0, QString::fromStdString(path));
  item->setCheckState(1, recursive ? Qt::Checked : Qt::Unchecked);
  m_ui.searchDirectoryList->addTopLevelItem(item);
}

void GameListSettingsWidget::refreshDirectoryList()
{
  QSignalBlocker sb(m_ui.searchDirectoryList);
  m_ui.searchDirectoryList->clear();

  std::vector<std::string> path_list = Core::GetBaseStringListSetting("GameList", "Paths");
  for (const std::string& entry : path_list)
    addPathToTable(entry, false);

  path_list = Core::GetBaseStringListSetting("GameList", "RecursivePaths");
  for (const std::string& entry : path_list)
    addPathToTable(entry, true);

  m_ui.searchDirectoryList->sortByColumn(0, Qt::AscendingOrder);
  m_ui.removeSearchDirectoryButton->setEnabled(false);
}

void GameListSettingsWidget::addSearchDirectory(const QString& path, bool recursive)
{
  const std::string spath(path.toStdString());
  Core::RemoveValueFromBaseStringListSetting("GameList", recursive ? "Paths" : "RecursivePaths", spath.c_str());
  Core::AddValueToBaseStringListSetting("GameList", recursive ? "RecursivePaths" : "Paths", spath.c_str());
  Host::CommitBaseSettingChanges();
  refreshDirectoryList();
  g_main_window->refreshGameList(false);
}

void GameListSettingsWidget::removeSearchDirectory(const QString& path)
{
  const std::string spath(path.toStdString());
  if (!Core::RemoveValueFromBaseStringListSetting("GameList", "Paths", spath.c_str()) &&
      !Core::RemoveValueFromBaseStringListSetting("GameList", "RecursivePaths", spath.c_str()))
  {
    return;
  }

  Host::CommitBaseSettingChanges();
  refreshDirectoryList();
  g_main_window->refreshGameList(false);
}

void GameListSettingsWidget::onDirectoryListSelectionChanged()
{
  m_ui.removeSearchDirectoryButton->setEnabled(m_ui.searchDirectoryList->selectionModel()->hasSelection());
}

void GameListSettingsWidget::onDirectoryListItemChanged(QTreeWidgetItem* item, int column)
{
  if (column != 1)
    return;

  const std::string path = item->text(0).toStdString();

  if (item->checkState(1) == Qt::Checked)
  {
    Core::RemoveValueFromBaseStringListSetting("GameList", "Paths", path.c_str());
    Core::AddValueToBaseStringListSetting("GameList", "RecursivePaths", path.c_str());
  }
  else
  {
    Core::RemoveValueFromBaseStringListSetting("GameList", "RecursivePaths", path.c_str());
    Core::AddValueToBaseStringListSetting("GameList", "Paths", path.c_str());
  }
  Host::CommitBaseSettingChanges();
  g_main_window->refreshGameList(false);
}

void GameListSettingsWidget::onDirectoryListContextMenuRequested(const QPoint& point)
{
  QModelIndexList selection = m_ui.searchDirectoryList->selectionModel()->selectedIndexes();
  if (selection.size() < 1)
    return;

  const int row = selection[0].row();

  QMenu* const menu = QtUtils::NewPopupMenu(this);
  menu->addAction(QIcon::fromTheme("folder-reduce-line"), tr("Remove"), this,
                  &GameListSettingsWidget::onRemoveSearchDirectoryButtonClicked);
  menu->addSeparator();
  menu->addAction(QIcon::fromTheme("folder-open-line"), tr("Open Directory..."), [this, row]() {
    const QTreeWidgetItem* const item = m_ui.searchDirectoryList->topLevelItem(row);
    if (item)
      QtUtils::OpenURL(this, QUrl::fromLocalFile(item->text(0)));
  });
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
  const QTreeWidgetItem* const item = m_ui.searchDirectoryList->takeTopLevelItem(index.row());
  if (!item)
    return;

  removeSearchDirectory(item->text(0));
  delete item;
}

void GameListSettingsWidget::onAddExcludedFileButtonClicked()
{
  QString path =
    QDir::toNativeSeparators(QFileDialog::getOpenFileName(QtUtils::GetRootWidget(this), tr("Select Path")));
  if (path.isEmpty())
    return;

  addExcludedPath(path);
}

void GameListSettingsWidget::onAddExcludedFolderButtonClicked()
{
  QString path =
    QDir::toNativeSeparators(QFileDialog::getExistingDirectory(QtUtils::GetRootWidget(this), tr("Select Directory")));
  if (path.isEmpty())
    return;

  addExcludedPath(path);
}

void GameListSettingsWidget::onRemoveExcludedPathButtonClicked()
{
  const int row = m_ui.excludedPaths->currentRow();
  QListWidgetItem* item = (row >= 0) ? m_ui.excludedPaths->takeItem(row) : 0;
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
