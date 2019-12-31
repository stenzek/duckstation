#include "gamelistsettingswidget.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QAbstractTableModel>
#include <QtCore/QSettings>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMessageBox>
#include <algorithm>

class GameListSearchDirectoriesModel : public QAbstractTableModel
{
public:
  GameListSearchDirectoriesModel(QSettings& settings) : m_settings(settings) {}

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

  void addEntry(const QString& path, bool recursive)
  {
    if (std::find_if(m_entries.begin(), m_entries.end(), [path](const Entry& e) { return e.path == path; }) !=
        m_entries.end())
    {
      return;
    }

    beginInsertRows(QModelIndex(), static_cast<int>(m_entries.size()), static_cast<int>(m_entries.size() + 1));
    m_entries.push_back({path, recursive});
    endInsertRows();
  }

  void removeEntry(int row)
  {
    if (row < 0 || row >= static_cast<int>(m_entries.size()))
      return;

    beginRemoveRows(QModelIndex(), row, row);
    m_entries.erase(m_entries.begin() + row);
    endRemoveRows();
  }

  void loadFromSettings()
  {
    QStringList path_list = m_settings.value(QStringLiteral("GameList/Paths")).toStringList();
    for (QString& entry : path_list)
      m_entries.push_back({std::move(entry), false});

    path_list = m_settings.value(QStringLiteral("GameList/RecursivePaths")).toStringList();
    for (QString& entry : path_list)
      m_entries.push_back({std::move(entry), true});
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
      m_settings.remove(QStringLiteral("GameList/Paths"));
    else
      m_settings.setValue(QStringLiteral("GameList/Paths"), paths);

    if (recursive_paths.empty())
      m_settings.remove(QStringLiteral("GameList/RecursivePaths"));
    else
      m_settings.setValue(QStringLiteral("GameList/RecursivePaths"), recursive_paths);
  }

private:
  struct Entry
  {
    QString path;
    bool recursive;
  };

  QSettings& m_settings;
  std::vector<Entry> m_entries;
};

GameListSettingsWidget::GameListSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  QSettings& qsettings = host_interface->getQSettings();

  m_search_directories_model = new GameListSearchDirectoriesModel(qsettings);
  m_search_directories_model->loadFromSettings();
  m_ui.redumpDatabasePath->setText(qsettings.value("GameList/RedumpDatabasePath").toString());
  m_ui.searchDirectoryList->setModel(m_search_directories_model);
  m_ui.searchDirectoryList->setSelectionMode(QAbstractItemView::SingleSelection);
  m_ui.searchDirectoryList->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_ui.searchDirectoryList->setAlternatingRowColors(true);
  m_ui.searchDirectoryList->setShowGrid(false);
  m_ui.searchDirectoryList->verticalHeader()->hide();
  m_ui.searchDirectoryList->setCurrentIndex({});

  connect(m_ui.addSearchDirectoryButton, &QToolButton::pressed, this,
          &GameListSettingsWidget::onAddSearchDirectoryButtonPressed);
  connect(m_ui.removeSearchDirectoryButton, &QToolButton::pressed, this,
          &GameListSettingsWidget::onRemoveSearchDirectoryButtonPressed);
  connect(m_ui.refreshGameListButton, &QToolButton::pressed, this,
          &GameListSettingsWidget::onRefreshGameListButtonPressed);
  connect(m_ui.browseRedumpPath, &QToolButton::pressed, this, &GameListSettingsWidget::onBrowseRedumpPathButtonPressed);
  connect(m_ui.downloadRedumpDatabase, &QToolButton::pressed, this,
          &GameListSettingsWidget::onDownloadRedumpDatabaseButtonPressed);
}

GameListSettingsWidget::~GameListSettingsWidget() = default;

void GameListSettingsWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);

  QtUtils::ResizeColumnsForTableView(m_ui.searchDirectoryList, {-1, 100});
}

void GameListSettingsWidget::onAddSearchDirectoryButtonPressed()
{
  QString dir = QFileDialog::getExistingDirectory(this, tr("Select Search Directory"));
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
  m_search_directories_model->saveToSettings();
  m_host_interface->refreshGameList(false);
}

void GameListSettingsWidget::onRemoveSearchDirectoryButtonPressed()
{
  QModelIndexList selection = m_ui.searchDirectoryList->selectionModel()->selectedIndexes();
  if (selection.size() < 1)
    return;

  const int row = selection[0].row();
  m_search_directories_model->removeEntry(row);
  m_search_directories_model->saveToSettings();
  m_host_interface->refreshGameList(false);
}

void GameListSettingsWidget::onRefreshGameListButtonPressed()
{
  m_host_interface->refreshGameList(true);
}

void GameListSettingsWidget::onBrowseRedumpPathButtonPressed()
{
  QString filename = QFileDialog::getOpenFileName(this, tr("Select Redump Database File"), QString(),
                                                  tr("Redump Database Files (*.dat)"));
  if (filename.isEmpty())
    return;

  m_ui.redumpDatabasePath->setText(filename);
  m_host_interface->getQSettings().setValue("GameList/RedumpDatabasePath", filename);
  m_host_interface->updateGameListDatabase(true);
}

void GameListSettingsWidget::onDownloadRedumpDatabaseButtonPressed()
{
  QMessageBox::information(this, tr("TODO"), tr("TODO"));
}
