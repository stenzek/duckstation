// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "isobrowserwindow.h"
#include "mainwindow.h"
#include "qtprogresscallback.h"
#include "qtutils.h"

#include "util/cd_image.h"
#include "util/translation.h"

#include "common/align.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"

#include <QtGui/QIcon>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenu>

#include <fmt/format.h>

#include "moc_isobrowserwindow.cpp"

using namespace Qt::StringLiterals;

LOG_CHANNEL(Host);

ISOBrowserWindow::ISOBrowserWindow(QWidget* parent) : QWidget(parent)
{
  m_ui.setupUi(this);
  m_ui.splitter->setSizes({200, 600});
  QtUtils::SetColumnWidthsForTreeView(m_ui.fileView, {-1, 200, 100});
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
  enableUi(false);

  connect(m_ui.openFile, &QAbstractButton::clicked, this, &ISOBrowserWindow::onOpenFileClicked);
  connect(m_ui.extract, &QAbstractButton::clicked, this, [this]() { onExtractClicked(IsoReader::ReadMode::Data); });
  connect(m_ui.extractMode2, &QAbstractButton::clicked, this,
          [this]() { onExtractClicked(IsoReader::ReadMode::Mode2); });
  connect(m_ui.extractRaw, &QAbstractButton::clicked, this, [this]() { onExtractClicked(IsoReader::ReadMode::Raw); });
  connect(m_ui.directoryView, &QTreeWidget::itemClicked, this, &ISOBrowserWindow::onDirectoryItemClicked);
  connect(m_ui.fileView, &QTreeWidget::itemActivated, this, &ISOBrowserWindow::onFileItemActivated);
  connect(m_ui.fileView, &QTreeWidget::itemSelectionChanged, this, &ISOBrowserWindow::onFileItemSelectionChanged);
  connect(m_ui.fileView, &QTreeWidget::customContextMenuRequested, this, &ISOBrowserWindow::onFileContextMenuRequested);
  connect(m_ui.close, &QAbstractButton::clicked, this, &ISOBrowserWindow::close);
}

ISOBrowserWindow::~ISOBrowserWindow() = default;

ISOBrowserWindow* ISOBrowserWindow::createAndOpenFile(QWidget* parent, const QString& path)
{
  ISOBrowserWindow* ib = new ISOBrowserWindow(nullptr);

  Error error;
  if (!ib->tryOpenFile(path, &error))
  {
    QtUtils::AsyncMessageBox(
      parent, QMessageBox::Critical, tr("Error"),
      tr("Failed to open %1:\n%2").arg(path).arg(QString::fromStdString(error.GetDescription())));
    delete ib;
    return nullptr;
  }

  return ib;
}

bool ISOBrowserWindow::tryOpenFile(const QString& path, Error* error /*= nullptr*/)
{
  const std::string native_path = QDir::toNativeSeparators(path).toStdString();
  std::unique_ptr<CDImage> image = CDImage::Open(native_path.c_str(), false, error);
  if (!image)
    return false;

  IsoReader new_reader;
  if (!new_reader.Open(image.get(), 1, error))
    return false;

  m_image = std::move(image);
  m_iso = std::move(new_reader);
  m_ui.openPath->setText(QString::fromStdString(native_path));
  setWindowTitle(tr("ISO Browser - %1").arg(QtUtils::StringViewToQString(Path::GetFileName(native_path))));
  enableUi(true);
  populateDirectories();
  populateFiles(QString());
  return true;
}

void ISOBrowserWindow::onOpenFileClicked()
{
  const QString path = QFileDialog::getOpenFileName(
    this, tr("Select File"),
    m_image ? QtUtils::StringViewToQString(Path::GetDirectory(m_image->GetPath())) : QString());
  if (path.isEmpty())
    return;

  Error error;
  if (!tryOpenFile(path, &error))
  {
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, tr("Error"),
      tr("Failed to open %1:\n%2").arg(path).arg(QString::fromStdString(error.GetDescription())));
    return;
  }
}

void ISOBrowserWindow::onExtractClicked(IsoReader::ReadMode mode)
{
  const QList<QTreeWidgetItem*> items = m_ui.fileView->selectedItems();
  if (items.isEmpty())
    return;

  const QString path = items.front()->data(0, Qt::UserRole).toString();
  extractFile(path, mode);
}

void ISOBrowserWindow::onDirectoryItemClicked(QTreeWidgetItem* item, int column)
{
  populateFiles(item->data(0, Qt::UserRole).toString());
}

void ISOBrowserWindow::onFileItemActivated(QTreeWidgetItem* item, int column)
{
  if (item->data(0, Qt::UserRole + 1).toBool())
  {
    // directory
    const QString dir = item->data(0, Qt::UserRole).toString();
    populateFiles(dir);

    // select it in the directory list
    QTreeWidgetItem* dir_item = findDirectoryItemForPath(dir, nullptr);
    if (dir_item)
    {
      QSignalBlocker sb(m_ui.directoryView);
      m_ui.directoryView->clearSelection();
      dir_item->setSelected(true);
    }

    return;
  }

  // file, go to extract
  extractFile(item->data(0, Qt::UserRole).toString(), IsoReader::ReadMode::Data);
}

void ISOBrowserWindow::onFileItemSelectionChanged()
{
  const QList<QTreeWidgetItem*> items = m_ui.fileView->selectedItems();

  // directory?
  const bool enabled = (!items.isEmpty() && !items.front()->data(0, Qt::UserRole + 1).toBool());
  enableExtractButtons(enabled);
}

void ISOBrowserWindow::onFileContextMenuRequested(const QPoint& pos)
{
  const QList<QTreeWidgetItem*> items = m_ui.fileView->selectedItems();
  if (items.isEmpty())
    return;

  QMenu* const menu = QtUtils::NewPopupMenu(this);

  const bool is_directory = items.front()->data(0, Qt::UserRole + 1).toBool();
  const QString path = items.front()->data(0, Qt::UserRole).toString();
  if (is_directory)
  {
    menu->addAction(QIcon::fromTheme(QIcon::ThemeIcon::FolderOpen), tr("&Open"),
                    [this, path]() { populateFiles(path); });
  }
  else
  {
    menu->addAction(QIcon::fromTheme(QIcon::ThemeIcon::DocumentSaveAs), tr("&Extract"),
                    [this, path]() { extractFile(path, IsoReader::ReadMode::Data); });
    menu->addAction(QIcon::fromTheme(QIcon::ThemeIcon::DocumentSaveAs), tr("Extract (&XA)"),
                    [this, path]() { extractFile(path, IsoReader::ReadMode::Mode2); });
    menu->addAction(QIcon::fromTheme(QIcon::ThemeIcon::DocumentSaveAs), tr("Extract (&Raw)"),
                    [this, path]() { extractFile(path, IsoReader::ReadMode::Raw); });
  }

  menu->popup(m_ui.fileView->mapToGlobal(pos));
}

void ISOBrowserWindow::extractFile(const QString& path, IsoReader::ReadMode mode)
{
  std::string spath = path.toStdString();
  const QString filename = QtUtils::StringViewToQString(Path::GetFileName(spath));
  std::string save_path =
    QDir::toNativeSeparators(QFileDialog::getSaveFileName(this, tr("Extract File"), filename)).toStdString();
  if (save_path.empty())
    return;

  const std::string status_text =
    fmt::format(TRANSLATE_FS("ISOBrowserWindow", "Extracting {}..."), Path::GetFileName(spath));
  QtAsyncTaskWithProgressDialog::create(
    this, windowTitle().toStdString(), status_text, false, true, 0, 0, 0.15f, true,
    [this, spath = std::move(spath), save_path = std::move(save_path), mode](ProgressCallback* const progress) mutable {
      Error error;
      std::optional<IsoReader::ISODirectoryEntry> de = m_iso.LocateFile(spath, &error);
      bool result;
      if ((result = de.has_value()))
      {
        auto fp = FileSystem::CreateAtomicRenamedFile(std::move(save_path), &error);
        if (fp)
        {
          if (m_iso.WriteFileToStream(de.value(), fp.get(), mode, &error, progress))
          {
            result = FileSystem::CommitAtomicRenamedFile(fp, &error);
          }
          else
          {
            // don't display error if cancelled
            FileSystem::DiscardAtomicRenamedFile(fp);
            result = progress->IsCancelled();
          }
        }
      }

      return [this, spath = std::move(spath), error = std::move(error), result]() mutable {
        if (!result)
        {
          QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                                   tr("Failed to save %1:\n%2")
                                     .arg(QtUtils::StringViewToQString(Path::GetFileName(spath)))
                                     .arg(QString::fromStdString(error.GetDescription())));
        }
      };
    });
}

QTreeWidgetItem* ISOBrowserWindow::findDirectoryItemForPath(const QString& path, QTreeWidgetItem* parent) const
{
  if (!parent)
  {
    parent = m_ui.directoryView->topLevelItem(0);
    if (path.isEmpty())
      return parent;
  }

  const int count = parent->childCount();
  for (int i = 0; i < count; i++)
  {
    QTreeWidgetItem* item = parent->child(i);
    if (item->data(0, Qt::UserRole) == path)
      return item;

    QTreeWidgetItem* child_item = findDirectoryItemForPath(path, item);
    if (child_item)
      return child_item;
  }

  return nullptr;
}

void ISOBrowserWindow::enableUi(bool enabled)
{
  m_ui.directoryView->setEnabled(enabled);
  m_ui.fileView->setEnabled(enabled);

  if (!enabled)
    enableExtractButtons(enabled);
}

void ISOBrowserWindow::enableExtractButtons(bool enabled)
{
  m_ui.extract->setEnabled(enabled);
  m_ui.extractMode2->setEnabled(enabled);
  m_ui.extractRaw->setEnabled(enabled);
}

void ISOBrowserWindow::populateDirectories()
{
  m_ui.directoryView->clear();
  enableExtractButtons(false);

  QTreeWidgetItem* root = new QTreeWidgetItem;
  root->setIcon(0, QIcon::fromTheme("disc-line"_L1));
  root->setText(0, QtUtils::StringViewToQString(Path::GetFileTitle(m_image->GetPath())));
  root->setData(0, Qt::UserRole, QString());
  m_ui.directoryView->addTopLevelItem(root);

  populateSubdirectories(std::string_view(), root);

  root->setExpanded(true);

  QSignalBlocker sb(m_ui.directoryView);
  root->setSelected(true);
}

void ISOBrowserWindow::populateSubdirectories(std::string_view dir, QTreeWidgetItem* parent)
{
  Error error;
  std::vector<std::pair<std::string, IsoReader::ISODirectoryEntry>> entries = m_iso.GetEntriesInDirectory(dir, &error);
  if (entries.empty() && error.IsValid())
  {
    ERROR_LOG("Failed to populate directory '{}': {}", dir, error.GetDescription());
    return;
  }

  for (const auto& [full_path, entry] : entries)
  {
    if (!entry.IsDirectory())
      continue;

    QTreeWidgetItem* item = new QTreeWidgetItem(parent);
    item->setIcon(0, QIcon::fromTheme("folder-open-line"_L1));
    item->setText(0, QtUtils::StringViewToQString(Path::GetFileName(full_path)));
    item->setData(0, Qt::UserRole, QString::fromStdString(full_path));
    populateSubdirectories(full_path, item);
  }
}

void ISOBrowserWindow::populateFiles(const QString& path)
{
  const std::string spath = path.toStdString();

  m_ui.fileView->clear();

  Error error;
  std::vector<std::pair<std::string, IsoReader::ISODirectoryEntry>> entries =
    m_iso.GetEntriesInDirectory(spath, &error);
  if (entries.empty() && error.IsValid())
  {
    ERROR_LOG("Failed to populate files '{}': {}", spath, error.GetDescription());
    return;
  }

  const auto add_entry = [this](const std::string& full_path, const IsoReader::ISODirectoryEntry& entry) {
    QTreeWidgetItem* item = new QTreeWidgetItem;
    item->setIcon(
      0, QIcon::fromTheme(entry.IsDirectory() ? "folder-open-line"_L1 : "file-line"_L1));
    item->setText(0, QtUtils::StringViewToQString(Path::GetFileName(full_path)));
    item->setData(0, Qt::UserRole, QString::fromStdString(full_path));
    item->setData(0, Qt::UserRole + 1, entry.IsDirectory());
    item->setText(1, QString::fromStdString(entry.recoding_time.GetFormattedTime()));
    if (!entry.IsDirectory())
      item->setText(2, tr("%1 KB").arg(Common::AlignUpPow2(entry.length_le, 1024) / 1024));
    m_ui.fileView->addTopLevelItem(item);
  };

  if (!path.isEmpty())
  {
    QTreeWidgetItem* item = new QTreeWidgetItem;
    item->setIcon(0, QIcon::fromTheme("folder-open-line"_L1));
    item->setText(0, tr("<Parent Directory>"));
    item->setData(0, Qt::UserRole, QtUtils::StringViewToQString(Path::GetDirectory(spath)));
    item->setData(0, Qt::UserRole + 1, true);
    m_ui.fileView->addTopLevelItem(item);
  }

  // list directories first
  for (const auto& [full_path, entry] : entries)
  {
    if (!entry.IsDirectory())
      continue;

    add_entry(full_path, entry);
  }

  for (const auto& [full_path, entry] : entries)
  {
    if (entry.IsDirectory())
      continue;

    add_entry(full_path, entry);
  }
}
