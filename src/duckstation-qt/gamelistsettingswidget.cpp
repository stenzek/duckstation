#include "gamelistsettingswidget.h"
#include "common/assert.h"
#include "common/file_system.h"
#include "common/string_util.h"
#include "frontend-common/game_list.h"
#include "gamelistsearchdirectoriesmodel.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include <QtCore/QAbstractTableModel>
#include <QtCore/QDebug>
#include <QtCore/QSettings>
#include <QtCore/QUrl>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <algorithm>

GameListSettingsWidget::GameListSettingsWidget(SettingsDialog* dialog, QWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);

  m_search_directories_model = new GameListSearchDirectoriesModel(g_emu_thread);
  m_ui.searchDirectoryList->setModel(m_search_directories_model);
  m_ui.searchDirectoryList->setSelectionMode(QAbstractItemView::SingleSelection);
  m_ui.searchDirectoryList->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_ui.searchDirectoryList->setAlternatingRowColors(true);
  m_ui.searchDirectoryList->setShowGrid(false);
  m_ui.searchDirectoryList->horizontalHeader()->setHighlightSections(false);
  m_ui.searchDirectoryList->verticalHeader()->hide();
  m_ui.searchDirectoryList->setCurrentIndex({});
  m_ui.searchDirectoryList->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);

  connect(m_ui.searchDirectoryList, &QTableView::clicked, this, &GameListSettingsWidget::onDirectoryListItemClicked);
  connect(m_ui.searchDirectoryList, &QTableView::customContextMenuRequested, this,
          &GameListSettingsWidget::onDirectoryListContextMenuRequested);
  connect(m_ui.addSearchDirectoryButton, &QPushButton::clicked, this,
          &GameListSettingsWidget::onAddSearchDirectoryButtonClicked);
  connect(m_ui.removeSearchDirectoryButton, &QPushButton::clicked, this,
          &GameListSettingsWidget::onRemoveSearchDirectoryButtonClicked);
  connect(m_ui.addExcludedPath, &QPushButton::clicked, this, &GameListSettingsWidget::onAddExcludedPathButtonClicked);
  connect(m_ui.removeExcludedPath, &QPushButton::clicked, this,
          &GameListSettingsWidget::onRemoveExcludedPathButtonClicked);
  connect(m_ui.rescanAllGames, &QPushButton::clicked, this, &GameListSettingsWidget::onRescanAllGamesClicked);
  connect(m_ui.scanForNewGames, &QPushButton::clicked, this, &GameListSettingsWidget::onScanForNewGamesClicked);

  refreshExclusionList();
}

GameListSettingsWidget::~GameListSettingsWidget() = default;

bool GameListSettingsWidget::addExcludedPath(const std::string& path)
{
  if (!Host::AddValueToBaseStringListSetting("GameList", "ExcludedPaths", path.c_str()))
    return false;

  m_ui.excludedPaths->addItem(QString::fromStdString(path));
  g_main_window->refreshGameList(false);
  return true;
}

void GameListSettingsWidget::refreshExclusionList()
{
  m_ui.excludedPaths->clear();

  const std::vector<std::string> paths(Host::GetBaseStringListSetting("GameList", "ExcludedPaths"));
  for (const std::string& path : paths)
    m_ui.excludedPaths->addItem(QString::fromStdString(path));
}

void GameListSettingsWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);

  QtUtils::ResizeColumnsForTableView(m_ui.searchDirectoryList, {-1, 100});
}

void GameListSettingsWidget::onDirectoryListItemClicked(const QModelIndex& index)
{
  if (!index.isValid())
    return;

  const int row = index.row();
  const int column = index.column();
  if (column != 1)
    return;

  m_search_directories_model->setEntryRecursive(row, !m_search_directories_model->isEntryRecursive(row));
}

void GameListSettingsWidget::onDirectoryListContextMenuRequested(const QPoint& point)
{
  QModelIndexList selection = m_ui.searchDirectoryList->selectionModel()->selectedIndexes();
  if (selection.size() < 1)
    return;

  const int row = selection[0].row();

  QMenu menu;
  menu.addAction(tr("Remove"), [this, row]() { m_search_directories_model->removeEntry(row); });
  menu.addSeparator();
  menu.addAction(tr("Open Directory..."),
                 [this, row]() { m_search_directories_model->openEntryInExplorer(this, row); });
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
  m_search_directories_model->addEntry(dir, recursive);
}

void GameListSettingsWidget::onAddSearchDirectoryButtonClicked()
{
  addSearchDirectory(this);
}

void GameListSettingsWidget::onRemoveSearchDirectoryButtonClicked()
{
  QModelIndexList selection = m_ui.searchDirectoryList->selectionModel()->selectedIndexes();
  if (selection.size() < 1)
    return;

  const int row = selection[0].row();
  m_search_directories_model->removeEntry(row);
}

void GameListSettingsWidget::onAddExcludedPathButtonClicked()
{
  QString path =
    QDir::toNativeSeparators(QFileDialog::getOpenFileName(QtUtils::GetRootWidget(this), tr("Select Path")));
  if (path.isEmpty())
    return;

  addExcludedPath(path.toStdString());
}

void GameListSettingsWidget::onRemoveExcludedPathButtonClicked()
{
  const int row = m_ui.excludedPaths->currentRow();
  QListWidgetItem* item = (row >= 0) ? m_ui.excludedPaths->takeItem(row) : 0;
  if (!item)
    return;

  Host::RemoveValueFromBaseStringListSetting("GameList", "ExcludedPaths", item->text().toUtf8().constData());
  delete item;

  g_main_window->refreshGameList(false);
}

void GameListSettingsWidget::onRescanAllGamesClicked()
{
  g_main_window->refreshGameList(true);
}

void GameListSettingsWidget::onScanForNewGamesClicked()
{
  g_main_window->refreshGameList(false);
}
