#include "gamelistsettingswidget.h"
#include "common/assert.h"
#include "common/string_util.h"
#include "core/game_list.h"
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
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <algorithm>
#include <unzip.h>

static constexpr char REDUMP_DOWNLOAD_URL[] = "http://redump.org/datfile/psx/serial,version,description";

class GameListSearchDirectoriesModel : public QAbstractTableModel
{
public:
  GameListSearchDirectoriesModel(QtHostInterface* host_interface) : m_host_interface(host_interface)
  {
    loadFromSettings();
  }

  ~GameListSearchDirectoriesModel() = default;

  int columnCount(const QModelIndex& parent) const override
  {
    if (parent.isValid())
      return 0;

    return 2;
  }

  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
  {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
      return {};

    if (section == 0)
      return tr("Path");
    else
      return tr("Recursive");
  }

  int rowCount(const QModelIndex& parent) const override
  {
    if (parent.isValid())
      return 0;

    return static_cast<int>(m_entries.size());
  }

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
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

  bool setData(const QModelIndex& index, const QVariant& value, int role) override
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
    m_host_interface->refreshGameList(false);
    return true;
  }

  void addEntry(const QString& path, bool recursive)
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
    m_host_interface->refreshGameList(false);
  }

  void removeEntry(int row)
  {
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
      return;

    beginRemoveRows(QModelIndex(), row, row);
    m_entries.erase(m_entries.begin() + row);
    endRemoveRows();

    saveToSettings();
    m_host_interface->refreshGameList(false);
  }

  bool isEntryRecursive(int row) const
  {
    return (row < 0 || row >= static_cast<int>(m_entries.size())) ? false : m_entries[row].recursive;
  }

  void setEntryRecursive(int row, bool recursive)
  {
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
      return;

    m_entries[row].recursive = recursive;
    emit dataChanged(index(row, 1), index(row, 1), {Qt::CheckStateRole});

    saveToSettings();
    m_host_interface->refreshGameList(false);
  }

  void loadFromSettings()
  {
    std::vector<std::string> path_list = m_host_interface->GetStringList("GameList", "Paths");
    for (std::string& entry : path_list)
      m_entries.push_back({QString::fromStdString(entry), false});

    path_list = m_host_interface->GetStringList("GameList", "RecursivePaths");
    for (std::string& entry : path_list)
      m_entries.push_back({QString::fromStdString(entry), true});
  }

  void saveToSettings()
  {
    QStringList paths;
    QStringList recursive_paths;

    for (const Entry& entry : m_entries)
    {
      if (entry.recursive)
        recursive_paths.push_back(entry.path);
      else
        paths.push_back(entry.path);
    }

    if (paths.empty())
      m_host_interface->removeSettingValue("GameList", "Paths");
    else
      m_host_interface->putSettingValue(QStringLiteral("GameList"), QStringLiteral("Paths"), paths);

    if (recursive_paths.empty())
      m_host_interface->removeSettingValue("GameList", "RecursivePaths");
    else
      m_host_interface->putSettingValue(QStringLiteral("GameList"), QStringLiteral("RecursivePaths"), recursive_paths);
  }

private:
  struct Entry
  {
    QString path;
    bool recursive;
  };

  QtHostInterface* m_host_interface;
  std::vector<Entry> m_entries;
};

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

  connect(m_ui.searchDirectoryList, &QTableView::clicked, this, &GameListSettingsWidget::onDirectoryListItemClicked);
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

  struct MemoryFileInfo
  {
    const QByteArray& data;
    int position;
  };

  MemoryFileInfo fi{data, 0};

#define FI static_cast<MemoryFileInfo*>(stream)

  zlib_filefunc64_def funcs = {
    [](voidpf opaque, const void* filename, int mode) -> voidpf { return opaque; }, // open
    [](voidpf opaque, voidpf stream, void* buf, uLong size) -> uLong {              // read
      const int remaining = FI->data.size() - FI->position;
      const int to_read = std::min(remaining, static_cast<int>(size));
      if (to_read > 0)
      {
        std::memcpy(buf, FI->data.constData() + FI->position, to_read);
        FI->position += to_read;
      }

      return static_cast<uLong>(to_read);
    },
    [](voidpf opaque, voidpf stream, const void* buf, uLong size) -> uLong { return 0; },         // write
    [](voidpf opaque, voidpf stream) -> ZPOS64_T { return static_cast<ZPOS64_T>(FI->position); }, // tell
    [](voidpf opaque, voidpf stream, ZPOS64_T offset, int origin) -> long {                       // seek
      int new_position = FI->position;
      if (origin == SEEK_SET)
        new_position = static_cast<int>(offset);
      else if (origin == SEEK_CUR)
        new_position += static_cast<int>(offset);
      else
        new_position = FI->data.size();
      if (new_position < 0 || new_position > FI->data.size())
        return -1;

      FI->position = new_position;
      return 0;
    },
    [](voidpf opaque, voidpf stream) -> int { return 0; }, // close
    [](voidpf opaque, voidpf stream) -> int { return 0; }, // testerror
    static_cast<voidpf>(&fi)};

#undef FI

  unzFile zf = unzOpen2_64("", &funcs);
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
