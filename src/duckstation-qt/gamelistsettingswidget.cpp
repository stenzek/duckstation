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
    if (std::find_if(m_entries.begin(), m_entries.end(), [path](const Entry& e) { return e.path == path; }) !=
        m_entries.end())
    {
      return;
    }

    beginInsertRows(QModelIndex(), static_cast<int>(m_entries.size()), static_cast<int>(m_entries.size()));
    m_entries.push_back({path, recursive});
    endInsertRows();

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
    QStringList path_list = m_host_interface->getSettingValue(QStringLiteral("GameList/Paths")).toStringList();
    for (QString& entry : path_list)
      m_entries.push_back({std::move(entry), false});

    path_list = m_host_interface->getSettingValue(QStringLiteral("GameList/RecursivePaths")).toStringList();
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
      m_host_interface->removeSettingValue(QStringLiteral("GameList/Paths"));
    else
      m_host_interface->putSettingValue(QStringLiteral("GameList/Paths"), paths);

    if (recursive_paths.empty())
      m_host_interface->removeSettingValue(QStringLiteral("GameList/RecursivePaths"));
    else
      m_host_interface->putSettingValue(QStringLiteral("GameList/RecursivePaths"), recursive_paths);
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
  connect(m_ui.addSearchDirectoryButton, &QToolButton::pressed, this,
          &GameListSettingsWidget::onAddSearchDirectoryButtonPressed);
  connect(m_ui.removeSearchDirectoryButton, &QToolButton::pressed, this,
          &GameListSettingsWidget::onRemoveSearchDirectoryButtonPressed);
  connect(m_ui.refreshGameListButton, &QToolButton::pressed, this,
          &GameListSettingsWidget::onRefreshGameListButtonPressed);
  connect(m_ui.updateRedumpDatabase, &QToolButton::pressed, this,
          &GameListSettingsWidget::onUpdateRedumpDatabaseButtonPressed);
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
  QString dir = QFileDialog::getExistingDirectory(parent_widget, tr("Select Search Directory"));
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

void GameListSettingsWidget::onAddSearchDirectoryButtonPressed()
{
  addSearchDirectory(this);
}

void GameListSettingsWidget::onRemoveSearchDirectoryButtonPressed()
{
  QModelIndexList selection = m_ui.searchDirectoryList->selectionModel()->selectedIndexes();
  if (selection.size() < 1)
    return;

  const int row = selection[0].row();
  m_search_directories_model->removeEntry(row);
}

void GameListSettingsWidget::onRefreshGameListButtonPressed()
{
  m_host_interface->refreshGameList(true);
}

void GameListSettingsWidget::onUpdateRedumpDatabaseButtonPressed()
{
  QMessageBox::information(this, tr("TODO"), tr("TODO"));
}
