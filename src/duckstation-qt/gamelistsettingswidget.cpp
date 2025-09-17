// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamelistsettingswidget.h"
#include "core/game_list.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

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
#include <QtWidgets/QMessageBox>
#include <algorithm>

#include "moc_gamelistsettingswidget.cpp"

GameListSettingsWidget::GameListSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);

  m_ui.searchDirectoryList->setSelectionMode(QAbstractItemView::SingleSelection);
  m_ui.searchDirectoryList->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_ui.searchDirectoryList->setAlternatingRowColors(true);
  m_ui.searchDirectoryList->setShowGrid(false);
  m_ui.searchDirectoryList->horizontalHeader()->setHighlightSections(false);
  m_ui.searchDirectoryList->verticalHeader()->hide();
  m_ui.searchDirectoryList->setCurrentIndex({});
  m_ui.searchDirectoryList->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);
  QtUtils::SetColumnWidthsForTableView(m_ui.searchDirectoryList, {-1, 120});

  connect(m_ui.searchDirectoryList, &QTableWidget::customContextMenuRequested, this,
          &GameListSettingsWidget::onDirectoryListContextMenuRequested);
  connect(m_ui.addSearchDirectoryButton, &QPushButton::clicked, this,
          &GameListSettingsWidget::onAddSearchDirectoryButtonClicked);
  connect(m_ui.removeSearchDirectoryButton, &QPushButton::clicked, this,
          &GameListSettingsWidget::onRemoveSearchDirectoryButtonClicked);
  connect(m_ui.searchDirectoryList, &QTableWidget::itemSelectionChanged, this,
          &GameListSettingsWidget::onSearchDirectoriesSelectionChanged);
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
  if (!Host::AddValueToBaseStringListSetting("GameList", "ExcludedPaths", path.toStdString().c_str()))
    return false;

  Host::CommitBaseSettingChanges();
  m_ui.excludedPaths->addItem(path);
  g_main_window->refreshGameList(false);
  return true;
}

void GameListSettingsWidget::refreshExclusionList()
{
  m_ui.excludedPaths->clear();

  const std::vector<std::string> paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
  for (const std::string& path : paths)
    m_ui.excludedPaths->addItem(QString::fromStdString(path));

  m_ui.removeExcludedPath->setEnabled(false);
}

void GameListSettingsWidget::addPathToTable(const std::string& path, bool recursive)
{
  const int row = m_ui.searchDirectoryList->rowCount();
  m_ui.searchDirectoryList->insertRow(row);

  QTableWidgetItem* item = new QTableWidgetItem();
  item->setText(QString::fromStdString(path));
  item->setFlags(item->flags() & ~(Qt::ItemIsEditable));
  m_ui.searchDirectoryList->setItem(row, 0, item);

  QCheckBox* cb = new QCheckBox(m_ui.searchDirectoryList);
  m_ui.searchDirectoryList->setCellWidget(row, 1, cb);
  cb->setChecked(recursive);

  connect(cb, &QCheckBox::checkStateChanged, this, [item](Qt::CheckState state) {
    const std::string path(item->text().toStdString());
    if (state == Qt::Checked)
    {
      Host::RemoveValueFromBaseStringListSetting("GameList", "Paths", path.c_str());
      Host::AddValueToBaseStringListSetting("GameList", "RecursivePaths", path.c_str());
    }
    else
    {
      Host::RemoveValueFromBaseStringListSetting("GameList", "RecursivePaths", path.c_str());
      Host::AddValueToBaseStringListSetting("GameList", "Paths", path.c_str());
    }
    Host::CommitBaseSettingChanges();
    g_main_window->refreshGameList(false);
  });
}

void GameListSettingsWidget::refreshDirectoryList()
{
  QSignalBlocker sb(m_ui.searchDirectoryList);
  while (m_ui.searchDirectoryList->rowCount() > 0)
    m_ui.searchDirectoryList->removeRow(0);

  std::vector<std::string> path_list = Host::GetBaseStringListSetting("GameList", "Paths");
  for (const std::string& entry : path_list)
    addPathToTable(entry, false);

  path_list = Host::GetBaseStringListSetting("GameList", "RecursivePaths");
  for (const std::string& entry : path_list)
    addPathToTable(entry, true);

  m_ui.searchDirectoryList->sortByColumn(0, Qt::AscendingOrder);
  m_ui.removeSearchDirectoryButton->setEnabled(false);
}

void GameListSettingsWidget::addSearchDirectory(const QString& path, bool recursive)
{
  const std::string spath(path.toStdString());
  Host::RemoveValueFromBaseStringListSetting("GameList", recursive ? "Paths" : "RecursivePaths", spath.c_str());
  Host::AddValueToBaseStringListSetting("GameList", recursive ? "RecursivePaths" : "Paths", spath.c_str());
  Host::CommitBaseSettingChanges();
  refreshDirectoryList();
  g_main_window->refreshGameList(false);
}

void GameListSettingsWidget::removeSearchDirectory(const QString& path)
{
  const std::string spath(path.toStdString());
  if (!Host::RemoveValueFromBaseStringListSetting("GameList", "Paths", spath.c_str()) &&
      !Host::RemoveValueFromBaseStringListSetting("GameList", "RecursivePaths", spath.c_str()))
  {
    return;
  }

  Host::CommitBaseSettingChanges();
  refreshDirectoryList();
  g_main_window->refreshGameList(false);
}

void GameListSettingsWidget::onDirectoryListContextMenuRequested(const QPoint& point)
{
  QModelIndexList selection = m_ui.searchDirectoryList->selectionModel()->selectedIndexes();
  if (selection.size() < 1)
    return;

  const int row = selection[0].row();

  QMenu menu;
  menu.addAction(tr("Remove"), [this]() { onRemoveSearchDirectoryButtonClicked(); });
  menu.addSeparator();
  menu.addAction(tr("Open Directory..."), [this, row]() {
    QtUtils::OpenURL(this, QUrl::fromLocalFile(m_ui.searchDirectoryList->item(row, 0)->text()));
  });
  menu.exec(m_ui.searchDirectoryList->mapToGlobal(point));
}

void GameListSettingsWidget::addSearchDirectory(QWidget* parent_widget)
{
  QString dir =
    QDir::toNativeSeparators(QFileDialog::getExistingDirectory(parent_widget, tr("Select Search Directory")));

  if (dir.isEmpty())
    return;

  QMessageBox::StandardButton selection =
    QMessageBox::question(this, tr("Scan Recursively?"),
                          tr("Would you like to scan the directory \"%1\" recursively?\n\nScanning recursively takes "
                             "more time, but will identify files in subdirectories.")
                            .arg(dir),
                          QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (selection == QMessageBox::Cancel)
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
  const int row = m_ui.searchDirectoryList->currentRow();
  QTableWidgetItem* item = (row >= 0) ? m_ui.searchDirectoryList->takeItem(row, 0) : nullptr;
  if (!item)
    return;

  removeSearchDirectory(item->text());
  delete item;
}

void GameListSettingsWidget::onSearchDirectoriesSelectionChanged()
{
  m_ui.removeSearchDirectoryButton->setEnabled(m_ui.searchDirectoryList->selectionModel()->hasSelection());
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

  if (Host::RemoveValueFromBaseStringListSetting("GameList", "ExcludedPaths", item->text().toUtf8().constData()))
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
