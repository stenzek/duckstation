#include "gamelistsettingswidget.h"
#include "common/assert.h"
#include "common/minizip_helpers.h"
#include "common/string_util.h"
#include "core/game_list.h"
#include "gamelistsearchdirectoriesmodel.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QAbstractTableModel>
#include <QtCore/QDebug>
#include <QtCore/QSettings>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <algorithm>

static constexpr char REDUMP_DOWNLOAD_URL[] = "http://redump.org/datfile/psx/serial,version,description";

GameListSettingsWidget::GameListSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  m_search_directories_model = new GameListSearchDirectoriesModel(host_interface);
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
  connect(m_ui.rescanAllGames, &QPushButton::clicked, this, &GameListSettingsWidget::onRescanAllGamesClicked);
  connect(m_ui.scanForNewGames, &QPushButton::clicked, this, &GameListSettingsWidget::onScanForNewGamesClicked);
  connect(m_ui.updateRedumpDatabase, &QPushButton::clicked, this,
          &GameListSettingsWidget::onUpdateRedumpDatabaseButtonClicked);
}

GameListSettingsWidget::~GameListSettingsWidget() = default;

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

void GameListSettingsWidget::onRescanAllGamesClicked()
{
  m_host_interface->refreshGameList(true, false);
}

void GameListSettingsWidget::onScanForNewGamesClicked()
{
  m_host_interface->refreshGameList(false, false);
}

void GameListSettingsWidget::onUpdateRedumpDatabaseButtonClicked()
{
  if (QMessageBox::question(this, tr("Download database from redump.org?"),
                            tr("Do you wish to download the disc database from redump.org?\n\nThis will download "
                               "approximately 4 megabytes over your current internet connection.")) != QMessageBox::Yes)
  {
    return;
  }

  if (downloadRedumpDatabase(QString::fromStdString(m_host_interface->getGameList()->GetDatabaseFilename())))
    m_host_interface->refreshGameList(true, true);
}

static bool ExtractRedumpDatabase(const QByteArray& data, const QString& destination_path)
{
  if (data.isEmpty())
    return false;

  unzFile zf = MinizipHelpers::OpenUnzMemoryFile(data.constData(), data.size());
  if (!zf)
  {
    qCritical() << "unzOpen2_64() failed";
    return false;
  }

  // find the first file with a .dat extension (in case there's others)
  if (unzGoToFirstFile(zf) != UNZ_OK)
  {
    qCritical() << "unzGoToFirstFile() failed";
    unzClose(zf);
    return false;
  }

  int dat_size = 0;
  for (;;)
  {
    char zip_filename_buffer[256];
    unz_file_info64 file_info;
    if (unzGetCurrentFileInfo64(zf, &file_info, zip_filename_buffer, sizeof(zip_filename_buffer), nullptr, 0, nullptr,
                                0) != UNZ_OK)
    {
      qCritical() << "unzGetCurrentFileInfo() failed";
      unzClose(zf);
      return false;
    }

    const char* extension = std::strrchr(zip_filename_buffer, '.');
    if (extension && StringUtil::Strcasecmp(extension, ".dat") == 0 && file_info.uncompressed_size > 0)
    {
      dat_size = static_cast<int>(file_info.uncompressed_size);
      qInfo() << "Found redump dat file in zip: " << zip_filename_buffer << "(" << dat_size << " bytes)";
      break;
    }

    if (unzGoToNextFile(zf) != UNZ_OK)
    {
      qCritical() << "dat file not found in downloaded redump zip";
      unzClose(zf);
      return false;
    }
  }

  if (unzOpenCurrentFile(zf) != UNZ_OK)
  {
    qCritical() << "unzOpenCurrentFile() failed";
    unzClose(zf);
    return false;
  }

  QByteArray dat_buffer;
  dat_buffer.resize(dat_size);
  if (unzReadCurrentFile(zf, dat_buffer.data(), dat_size) != dat_size)
  {
    qCritical() << "unzReadCurrentFile() failed";
    unzClose(zf);
    return false;
  }

  unzCloseCurrentFile(zf);
  unzClose(zf);

  QFile dat_output_file(destination_path);
  if (!dat_output_file.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    qCritical() << "QFile::open() failed";
    return false;
  }

  if (static_cast<int>(dat_output_file.write(dat_buffer)) != dat_buffer.size())
  {
    qCritical() << "QFile::write() failed";
    return false;
  }

  dat_output_file.close();
  qInfo() << "Wrote redump dat to " << destination_path;
  return true;
}

bool GameListSettingsWidget::downloadRedumpDatabase(const QString& download_path)
{
  Assert(!download_path.isEmpty());

  QNetworkAccessManager manager;

  QUrl url(QUrl::fromEncoded(QByteArray(REDUMP_DOWNLOAD_URL, sizeof(REDUMP_DOWNLOAD_URL) - 1)));
  QNetworkRequest request(url);

  QNetworkReply* reply = manager.get(request);

  QProgressDialog progress(tr("Downloading %1...").arg(REDUMP_DOWNLOAD_URL), tr("Cancel"), 0, 1);
  progress.setAutoClose(false);

  connect(reply, &QNetworkReply::downloadProgress, [&progress](quint64 received, quint64 total) {
    progress.setRange(0, static_cast<int>(total));
    progress.setValue(static_cast<int>(received));
  });

  connect(&manager, &QNetworkAccessManager::finished, [this, &progress, &download_path](QNetworkReply* reply) {
    if (reply->error() != QNetworkReply::NoError)
    {
      QMessageBox::critical(this, tr("Download failed"), reply->errorString());
      progress.done(-1);
      return;
    }

    progress.setRange(0, 100);
    progress.setValue(100);
    progress.setLabelText(tr("Extracting..."));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    const QByteArray data = reply->readAll();
    if (!ExtractRedumpDatabase(data, download_path))
    {
      QMessageBox::critical(this, tr("Extract failed"), tr("Extracting game database failed."));
      progress.done(-1);
      return;
    }

    progress.done(1);
  });

  const int result = progress.exec();
  if (result == 0)
  {
    // cancelled
    reply->abort();
  }

  reply->deleteLater();
  return (result == 1);
}
