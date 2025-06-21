// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "memorycardeditorwindow.h"
#include "qtutils.h"

#include "core/host.h"
#include "core/settings.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/string_util.h"

#include <QtCore/QFileInfo>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>

static constexpr char MEMORY_CARD_IMAGE_FILTER[] =
  QT_TRANSLATE_NOOP("MemoryCardEditorWindow", "DuckStation Memory Card (*.mcd)");
static constexpr char MEMORY_CARD_IMPORT_FILTER[] = QT_TRANSLATE_NOOP(
  "MemoryCardEditorWindow",
  "All Importable Memory Card Types (*.mcd *.mcr *.mc *.gme *.srm *.psm *.ps *.ddf *.mem *.vgs *.psx)");
static constexpr char SINGLE_SAVEFILE_FILTER[] =
  TRANSLATE_NOOP("MemoryCardEditorWindow", "Single Save Files (*.mcs);;All Files (*.*)");
static constexpr std::array<std::pair<ConsoleRegion, const char*>, 3> MEMORY_CARD_FILE_REGION_PREFIXES = {{
  {ConsoleRegion::NTSC_U, "BA"},
  {ConsoleRegion::NTSC_J, "BI"},
  {ConsoleRegion::PAL, "BE"},
}};

MemoryCardEditorWindow::MemoryCardEditorWindow() : QWidget()
{
  m_ui.setupUi(this);
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_deleteFile = m_ui.centerButtonBox->addButton(tr("Delete File"), QDialogButtonBox::ActionRole);
  m_undeleteFile = m_ui.centerButtonBox->addButton(tr("Undelete File"), QDialogButtonBox::ActionRole);
  m_renameFile = m_ui.centerButtonBox->addButton(tr("Rename File"), QDialogButtonBox::ActionRole);
  m_exportFile = m_ui.centerButtonBox->addButton(tr("Export File"), QDialogButtonBox::ActionRole);
  m_moveLeft = m_ui.centerButtonBox->addButton(tr("<<"), QDialogButtonBox::ActionRole);
  m_moveRight = m_ui.centerButtonBox->addButton(tr(">>"), QDialogButtonBox::ActionRole);

  m_card_a.path_cb = m_ui.cardAPath;
  m_card_a.table = m_ui.cardA;
  m_card_a.blocks_free_label = m_ui.cardAUsage;
  m_card_b.path_cb = m_ui.cardBPath;
  m_card_b.table = m_ui.cardB;
  m_card_b.blocks_free_label = m_ui.cardBUsage;

  createCardButtons(&m_card_a, m_ui.buttonBoxA);
  createCardButtons(&m_card_b, m_ui.buttonBoxB);
  connectUi();
  connectCardUi(&m_card_a, m_ui.buttonBoxA);
  connectCardUi(&m_card_b, m_ui.buttonBoxB);
  populateComboBox(m_ui.cardAPath);
  populateComboBox(m_ui.cardBPath);

  const QString new_card_hover_text(tr("New Card..."));
  const QString open_card_hover_text(tr("Open Card..."));
  m_ui.newCardA->setToolTip(new_card_hover_text);
  m_ui.newCardB->setToolTip(new_card_hover_text);
  m_ui.openCardA->setToolTip(open_card_hover_text);
  m_ui.openCardB->setToolTip(open_card_hover_text);
}

MemoryCardEditorWindow::~MemoryCardEditorWindow() = default;

bool MemoryCardEditorWindow::setCardA(const QString& path)
{
  int index = m_ui.cardAPath->findData(QVariant(QDir::toNativeSeparators(path)));
  if (index < 0)
  {
    QFileInfo file(path);
    if (!file.exists())
      return false;

    QSignalBlocker sb(m_card_a.path_cb);
    m_card_a.path_cb->addItem(file.baseName(), QVariant(path));
    index = m_card_a.path_cb->count() - 1;
  }

  m_ui.cardAPath->setCurrentIndex(index);
  return true;
}

bool MemoryCardEditorWindow::setCardB(const QString& path)
{
  int index = m_ui.cardBPath->findData(QVariant(QDir::toNativeSeparators(path)));
  if (index < 0)
  {
    QFileInfo file(path);
    if (!file.exists())
      return false;

    QSignalBlocker sb(m_card_b.path_cb);
    m_card_b.path_cb->addItem(file.baseName(), QVariant(path));
    index = m_card_b.path_cb->count() - 1;
  }

  m_ui.cardBPath->setCurrentIndex(index);
  return true;
}

bool MemoryCardEditorWindow::createMemoryCard(const QString& path, Error* error)
{
  std::unique_ptr<MemoryCardImage::DataArray> data = std::make_unique<MemoryCardImage::DataArray>();
  MemoryCardImage::Format(data.get());

  return MemoryCardImage::SaveToFile(*data.get(), path.toUtf8().constData(), error);
}

void MemoryCardEditorWindow::resizeEvent(QResizeEvent* ev)
{
  QtUtils::ResizeColumnsForTableView(m_card_a.table, {32, -1, 155, 45});
  QtUtils::ResizeColumnsForTableView(m_card_b.table, {32, -1, 155, 45});
}

void MemoryCardEditorWindow::closeEvent(QCloseEvent* ev)
{
  m_card_a.path_cb->setCurrentIndex(0);
  m_card_b.path_cb->setCurrentIndex(0);
}

void MemoryCardEditorWindow::createCardButtons(Card* card, QDialogButtonBox* buttonBox)
{
  card->format_button = buttonBox->addButton(tr("Format Card"), QDialogButtonBox::ActionRole);
  card->import_file_button = buttonBox->addButton(tr("Import File..."), QDialogButtonBox::ActionRole);
  card->import_button = buttonBox->addButton(tr("Import Card..."), QDialogButtonBox::ActionRole);
  card->save_button = buttonBox->addButton(tr("Save"), QDialogButtonBox::ActionRole);
}

void MemoryCardEditorWindow::connectCardUi(Card* card, QDialogButtonBox* buttonBox)
{
  connect(card->save_button, &QPushButton::clicked, [this, card] { saveCard(card); });
  connect(card->format_button, &QPushButton::clicked, [this, card] { formatCard(card); });
  connect(card->import_file_button, &QPushButton::clicked, [this, card] { importSaveFile(card); });
  connect(card->import_button, &QPushButton::clicked, [this, card] { importCard(card); });
}

void MemoryCardEditorWindow::connectUi()
{
  connect(m_ui.cardA, &QTableWidget::itemSelectionChanged, this, &MemoryCardEditorWindow::onCardASelectionChanged);
  connect(m_ui.cardA, &QTableWidget::customContextMenuRequested, this,
          &MemoryCardEditorWindow::onCardContextMenuRequested);
  connect(m_ui.cardB, &QTableWidget::itemSelectionChanged, this, &MemoryCardEditorWindow::onCardBSelectionChanged);
  connect(m_ui.cardB, &QTableWidget::customContextMenuRequested, this,
          &MemoryCardEditorWindow::onCardContextMenuRequested);
  connect(m_moveLeft, &QPushButton::clicked, this, &MemoryCardEditorWindow::doCopyFile);
  connect(m_moveRight, &QPushButton::clicked, this, &MemoryCardEditorWindow::doCopyFile);
  connect(m_deleteFile, &QPushButton::clicked, this, &MemoryCardEditorWindow::doDeleteFile);
  connect(m_undeleteFile, &QPushButton::clicked, this, &MemoryCardEditorWindow::doUndeleteFile);

  connect(m_ui.cardAPath, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { loadCardFromComboBox(&m_card_a, index); });
  connect(m_ui.cardBPath, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { loadCardFromComboBox(&m_card_b, index); });
  connect(m_ui.newCardA, &QPushButton::clicked, [this]() { newCard(&m_card_a); });
  connect(m_ui.newCardB, &QPushButton::clicked, [this]() { newCard(&m_card_b); });
  connect(m_ui.openCardA, &QPushButton::clicked, [this]() { openCard(&m_card_a); });
  connect(m_ui.openCardB, &QPushButton::clicked, [this]() { openCard(&m_card_b); });
  connect(m_renameFile, &QPushButton::clicked, this, &MemoryCardEditorWindow::doRenameSaveFile);
  connect(m_exportFile, &QPushButton::clicked, this, &MemoryCardEditorWindow::doExportSaveFile);
}

void MemoryCardEditorWindow::populateComboBox(QComboBox* cb)
{
  QSignalBlocker sb(cb);

  cb->clear();

  cb->addItem(QString());

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(EmuFolders::MemoryCards.c_str(), "*.mcd",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_SORT_BY_NAME,
                        &results);
  for (FILESYSTEM_FIND_DATA& fd : results)
  {
    std::string real_filename(Path::Combine(EmuFolders::MemoryCards, fd.FileName));
    std::string::size_type pos = fd.FileName.rfind('.');
    if (pos != std::string::npos)
      fd.FileName.erase(pos);

    cb->addItem(QString::fromStdString(fd.FileName), QVariant(QString::fromStdString(real_filename)));
  }
}

void MemoryCardEditorWindow::loadCardFromComboBox(Card* card, int index)
{
  loadCard(card->path_cb->itemData(index).toString(), card);
}

void MemoryCardEditorWindow::onCardASelectionChanged()
{
  {
    QSignalBlocker cb(m_card_b.table);
    m_card_b.table->clearSelection();
  }

  updateButtonState();
}

void MemoryCardEditorWindow::onCardBSelectionChanged()
{
  {
    QSignalBlocker cb(m_card_a.table);
    m_card_a.table->clearSelection();
  }

  updateButtonState();
}

void MemoryCardEditorWindow::clearSelection()
{
  {
    QSignalBlocker cb(m_card_a.table);
    m_card_a.table->clearSelection();
  }

  {
    QSignalBlocker cb(m_card_b.table);
    m_card_b.table->clearSelection();
  }

  updateButtonState();
}

bool MemoryCardEditorWindow::loadCard(const QString& filename, Card* card)
{
  promptForSave(card);

  card->table->setRowCount(0);
  card->dirty = false;
  card->blocks_free_label->clear();
  card->save_button->setEnabled(false);

  card->filename.clear();

  if (filename.isEmpty())
  {
    updateButtonState();
    return false;
  }

  Error error;
  std::string filename_str = filename.toStdString();
  if (!MemoryCardImage::LoadFromFile(&card->data, filename_str.c_str(), &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to load memory card: %1").arg(QString::fromStdString(error.GetDescription())));
    return false;
  }

  card->filename = std::move(filename_str);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
  return true;
}

static void setCardTableItemProperties(QTableWidgetItem* item, const MemoryCardImage::FileInfo& fi)
{
  item->setFlags(item->flags() & ~(Qt::ItemIsEditable));
  if (fi.deleted)
  {
    item->setBackground(Qt::darkRed);
    item->setForeground(Qt::white);
  }
}

void MemoryCardEditorWindow::updateCardTable(Card* card)
{
  card->table->setRowCount(0);

  card->files = MemoryCardImage::EnumerateFiles(card->data, true);
  for (const MemoryCardImage::FileInfo& fi : card->files)
  {
    const int row = card->table->rowCount();
    card->table->insertRow(row);

    if (!fi.icon_frames.empty())
    {
      const QImage image(reinterpret_cast<const u8*>(fi.icon_frames[0].pixels), MemoryCardImage::ICON_WIDTH,
                         MemoryCardImage::ICON_HEIGHT, QImage::Format_RGBA8888);

      QTableWidgetItem* icon = new QTableWidgetItem();
      setCardTableItemProperties(icon, fi);
      icon->setIcon(QIcon(QPixmap::fromImage(image)));
      card->table->setItem(row, 0, icon);
    }

    QString title_str(QString::fromStdString(fi.title));
    if (fi.deleted)
      title_str += tr(" (Deleted)");

    QTableWidgetItem* item = new QTableWidgetItem(title_str);
    setCardTableItemProperties(item, fi);
    card->table->setItem(row, 1, item);

    item = new QTableWidgetItem(QString::fromStdString(fi.filename));
    setCardTableItemProperties(item, fi);
    card->table->setItem(row, 2, item);

    item = new QTableWidgetItem(QString::number(fi.num_blocks));
    setCardTableItemProperties(item, fi);
    card->table->setItem(row, 3, item);
  }
}

void MemoryCardEditorWindow::updateCardBlocksFree(Card* card)
{
  card->blocks_free = MemoryCardImage::GetFreeBlockCount(card->data);
  card->blocks_free_label->setText(
    tr("%n block(s) free%1", "", card->blocks_free).arg(card->dirty ? QStringLiteral(" (*)") : QString()));
}

void MemoryCardEditorWindow::setCardDirty(Card* card)
{
  card->dirty = true;
  card->save_button->setEnabled(true);
}

void MemoryCardEditorWindow::newCard(Card* card)
{
  promptForSave(card);

  QString filename = QDir::toNativeSeparators(
    QFileDialog::getSaveFileName(this, tr("Select Memory Card"), QString(), tr(MEMORY_CARD_IMAGE_FILTER)));
  if (filename.isEmpty())
    return;

  {
    // add to combo box
    QFileInfo file(filename);
    QSignalBlocker sb(card->path_cb);
    card->path_cb->addItem(file.baseName(), QVariant(filename));
    card->path_cb->setCurrentIndex(card->path_cb->count() - 1);
  }

  card->filename = filename.toStdString();

  MemoryCardImage::Format(&card->data);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
  saveCard(card);
}

void MemoryCardEditorWindow::openCard(Card* card)
{
  promptForSave(card);

  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Memory Card"), QString(), tr(MEMORY_CARD_IMAGE_FILTER)));
  if (filename.isEmpty())
    return;

  Error error;
  if (!MemoryCardImage::LoadFromFile(&card->data, filename.toUtf8().constData(), &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to load memory card: %1").arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  {
    // add to combo box
    QFileInfo file(filename);
    QSignalBlocker sb(card->path_cb);
    card->path_cb->addItem(file.baseName(), QVariant(filename));
    card->path_cb->setCurrentIndex(card->path_cb->count() - 1);
  }

  card->filename = filename.toStdString();
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
}

void MemoryCardEditorWindow::saveCard(Card* card)
{
  if (card->filename.empty())
    return;

  Error error;
  if (!MemoryCardImage::SaveToFile(card->data, card->filename.c_str(), &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save memory card: %1").arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  card->dirty = false;
  card->save_button->setEnabled(false);
  updateCardBlocksFree(card);
}

void MemoryCardEditorWindow::promptForSave(Card* card)
{
  if (card->filename.empty() || !card->dirty)
    return;

  if (QMessageBox::question(this, tr("Save memory card?"),
                            tr("Memory card '%1' is not saved, do you want to save before closing?")
                              .arg(QString::fromStdString(card->filename)),
                            QMessageBox::Yes, QMessageBox::No) == QMessageBox::No)
  {
    return;
  }

  saveCard(card);
}

void MemoryCardEditorWindow::doCopyFile()
{
  const auto [src, fi] = getSelectedFile();
  if (!fi)
    return;

  Card* dst = (src == &m_card_a) ? &m_card_b : &m_card_a;

  for (const MemoryCardImage::FileInfo& dst_fi : dst->files)
  {
    if (dst_fi.filename == fi->filename)
    {
      QMessageBox::critical(
        this, tr("Error"),
        tr("Destination memory card already contains a save file with the same name (%1) as the one you are attempting "
           "to copy. Please delete this file from the destination memory card before copying.")
          .arg(QString(fi->filename.c_str())));
      return;
    }
  }

  if (dst->blocks_free < fi->num_blocks)
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Insufficient blocks, this file needs %1 but only %2 are available.")
                            .arg(fi->num_blocks)
                            .arg(dst->blocks_free));
    return;
  }

  Error error;
  std::vector<u8> buffer;
  if (!MemoryCardImage::ReadFile(src->data, *fi, &buffer, &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to read file %1:\n%2")
                            .arg(QString::fromStdString(fi->filename))
                            .arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  if (!MemoryCardImage::WriteFile(&dst->data, fi->filename, buffer, &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to write file %1:\n%2")
                            .arg(QString::fromStdString(fi->filename))
                            .arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  clearSelection();
  setCardDirty(dst);
  updateCardTable(dst);
  updateCardBlocksFree(dst);
  updateButtonState();
}

void MemoryCardEditorWindow::doDeleteFile()
{
  const auto [card, fi] = getSelectedFile();
  if (!fi)
    return;

  if (!MemoryCardImage::DeleteFile(&card->data, *fi, fi->deleted))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to delete file %1").arg(QString::fromStdString(fi->filename)));
    return;
  }

  clearSelection();
  setCardDirty(card);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
}

void MemoryCardEditorWindow::doUndeleteFile()
{
  const auto [card, fi] = getSelectedFile();
  if (!fi)
    return;

  if (!MemoryCardImage::UndeleteFile(&card->data, *fi))
  {
    QMessageBox::critical(
      this, tr("Error"),
      tr("Failed to undelete file %1. The file may have been partially overwritten by another save.")
        .arg(QString::fromStdString(fi->filename)));
    return;
  }

  clearSelection();
  setCardDirty(card);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
}

void MemoryCardEditorWindow::doExportSaveFile()
{
  QString filename = QDir::toNativeSeparators(
    QFileDialog::getSaveFileName(this, tr("Select Single Savefile"), QString(), tr(SINGLE_SAVEFILE_FILTER)));

  if (filename.isEmpty())
    return;

  const auto [card, fi] = getSelectedFile();
  if (!fi)
    return;

  Error error;
  if (!MemoryCardImage::ExportSave(&card->data, *fi, filename.toStdString().c_str(), &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to export save file %1:\n%2")
                            .arg(QString::fromStdString(fi->filename))
                            .arg(QString::fromStdString(error.GetDescription())));
    return;
  }
}

void MemoryCardEditorWindow::doRenameSaveFile()
{
  const auto [card, fi] = getSelectedFile();
  if (!fi)
    return;

  const std::string new_name = MemoryCardRenameFileDialog::promptForNewName(this, fi->filename);
  if (new_name.empty())
    return;

  Error error;
  if (!MemoryCardImage::RenameFile(&card->data, *fi, new_name, &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to rename save file %1:\n%2")
                            .arg(QString::fromStdString(fi->filename))
                            .arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  clearSelection();
  setCardDirty(card);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
}

void MemoryCardEditorWindow::importCard(Card* card)
{
  promptForSave(card);

  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Import File"), QString(), tr(MEMORY_CARD_IMPORT_FILTER)));
  if (filename.isEmpty())
    return;

  Error error;
  std::unique_ptr<MemoryCardImage::DataArray> temp = std::make_unique<MemoryCardImage::DataArray>();
  if (!MemoryCardImage::ImportCard(temp.get(), filename.toStdString().c_str(), &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to import memory card from %1:\n%2")
                            .arg(QFileInfo(filename).fileName())
                            .arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  clearSelection();

  card->data = *temp;
  setCardDirty(card);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
}

void MemoryCardEditorWindow::formatCard(Card* card)
{
  promptForSave(card);

  if (QMessageBox::question(this, tr("Format memory card?"),
                            tr("Formatting the memory card will destroy all saves, and they will not be recoverable. "
                               "The memory card which will be formatted is located at '%1'.")
                              .arg(QString::fromStdString(card->filename)),
                            QMessageBox::Yes, QMessageBox::No) == QMessageBox::No)
  {
    return;
  }

  clearSelection();

  MemoryCardImage::Format(&card->data);

  setCardDirty(card);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
}

void MemoryCardEditorWindow::importSaveFile(Card* card)
{
  QString filename = QDir::toNativeSeparators(
    QFileDialog::getOpenFileName(this, tr("Select Save File"), QString(), tr(SINGLE_SAVEFILE_FILTER)));

  if (filename.isEmpty())
    return;

  Error error;
  if (!MemoryCardImage::ImportSave(&card->data, filename.toStdString().c_str(), &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to import save from %1:\n%2")
                            .arg(QFileInfo(filename).fileName())
                            .arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  setCardDirty(card);
  updateCardTable(card);
  updateCardBlocksFree(card);
}

void MemoryCardEditorWindow::onCardContextMenuRequested(const QPoint& pos)
{
  QTableWidget* table = qobject_cast<QTableWidget*>(sender());
  if (!table)
    return;

  const auto& [card, fi] = getSelectedFile();
  if (!card)
    return;

  QMenu menu(table);
  QAction* action = menu.addAction(tr("Delete File"));
  action->setEnabled(fi && !fi->deleted);
  connect(action, &QAction::triggered, this, &MemoryCardEditorWindow::doDeleteFile);
  action = menu.addAction(tr("Undelete File"));
  action->setEnabled(fi && fi->deleted);
  connect(action, &QAction::triggered, this, &MemoryCardEditorWindow::doUndeleteFile);
  action = menu.addAction(tr("Rename File"));
  action->setEnabled(fi != nullptr);
  connect(action, &QAction::triggered, this, &MemoryCardEditorWindow::doRenameSaveFile);
  action = menu.addAction(tr("Export File"));
  connect(action, &QAction::triggered, this, &MemoryCardEditorWindow::doExportSaveFile);
  action = menu.addAction(tr("Copy File"));
  action->setEnabled(fi && !m_card_a.filename.empty() && !m_card_b.filename.empty());
  connect(action, &QAction::triggered, this, &MemoryCardEditorWindow::doCopyFile);

  menu.exec(table->mapToGlobal(pos));
}

std::tuple<MemoryCardEditorWindow::Card*, const MemoryCardImage::FileInfo*> MemoryCardEditorWindow::getSelectedFile()
{
  QList<QTableWidgetSelectionRange> sel = m_card_a.table->selectedRanges();
  Card* card = &m_card_a;

  if (sel.isEmpty())
  {
    sel = m_card_b.table->selectedRanges();
    card = &m_card_b;
  }

  if (sel.isEmpty())
    return std::tuple<Card*, const MemoryCardImage::FileInfo*>(nullptr, nullptr);

  const int index = sel.front().topRow();
  Assert(index >= 0 && static_cast<u32>(index) < card->files.size());

  return std::tuple<Card*, const MemoryCardImage::FileInfo*>(card, &card->files[index]);
}

void MemoryCardEditorWindow::updateButtonState()
{
  const auto [selected_card, selected_file] = getSelectedFile();
  const bool is_card_b = (selected_card == &m_card_b);
  const bool has_selection = (selected_file != nullptr);
  const bool is_deleted = (selected_file != nullptr && selected_file->deleted);
  const bool card_a_present = !m_card_a.filename.empty();
  const bool card_b_present = !m_card_b.filename.empty();
  const bool both_cards_present = card_a_present && card_b_present;
  m_deleteFile->setEnabled(has_selection);
  m_undeleteFile->setEnabled(is_deleted);
  m_exportFile->setEnabled(has_selection);
  m_renameFile->setEnabled(has_selection);
  m_moveLeft->setEnabled(both_cards_present && has_selection && is_card_b);
  m_moveRight->setEnabled(both_cards_present && has_selection && !is_card_b);
  m_ui.buttonBoxA->setEnabled(card_a_present);
  m_ui.buttonBoxB->setEnabled(card_b_present);
}

MemoryCardRenameFileDialog::MemoryCardRenameFileDialog(QWidget* parent, std::string_view old_name) : QDialog(parent)
{
  m_ui.setupUi(this);
  setupAdditionalUi();

  const QString original_name = QtUtils::StringViewToQString(old_name);
  m_ui.originalName->setText(original_name);
  m_ui.fullFilename->setText(original_name);
  updateSimplifiedFieldsFromFullName();
}

MemoryCardRenameFileDialog::~MemoryCardRenameFileDialog() = default;

std::string MemoryCardRenameFileDialog::promptForNewName(QWidget* parent, std::string_view old_name)
{
  MemoryCardRenameFileDialog dlg(parent, old_name);
  if (dlg.exec() == QDialog::Rejected)
    return {};

  return dlg.m_ui.fullFilename->text().toStdString();
}

void MemoryCardRenameFileDialog::setupAdditionalUi()
{
  m_ui.icon->setPixmap(QIcon::fromTheme(QStringLiteral("memcard-line")).pixmap(32));

  for (const auto& [region, prefix] : MEMORY_CARD_FILE_REGION_PREFIXES)
  {
    m_ui.region->addItem(QtUtils::GetIconForRegion(region), Settings::GetConsoleRegionDisplayName(region),
                         QVariant(QString::fromUtf8(prefix)));
  }

  connect(m_ui.region, &QComboBox::currentIndexChanged, this,
          &MemoryCardRenameFileDialog::updateFullNameFromSimplifiedFields);
  connect(m_ui.serial, &QLineEdit::textChanged, this, &MemoryCardRenameFileDialog::updateFullNameFromSimplifiedFields);
  connect(m_ui.filename, &QLineEdit::textChanged, this,
          &MemoryCardRenameFileDialog::updateFullNameFromSimplifiedFields);

  connect(m_ui.fullFilename, &QLineEdit::textChanged, this,
          &MemoryCardRenameFileDialog::updateSimplifiedFieldsFromFullName);

  connect(m_ui.buttonBox, &QDialogButtonBox::accepted, this, &MemoryCardRenameFileDialog::accept);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &MemoryCardRenameFileDialog::reject);

  m_ui.fullFilename->setFocus();
}

void MemoryCardRenameFileDialog::updateSimplifiedFieldsFromFullName()
{
  const QString full_name = m_ui.fullFilename->text();

  const QString region = full_name.mid(0, MemoryCardImage::FILE_REGION_LENGTH);
  const QString serial = full_name.mid(MemoryCardImage::FILE_REGION_LENGTH, MemoryCardImage::FILE_SERIAL_LENGTH);
  const QString filename = full_name.mid(MemoryCardImage::FILE_REGION_LENGTH + MemoryCardImage::FILE_SERIAL_LENGTH);

  {
    QSignalBlocker sb(m_ui.region);

    while (m_ui.region->count() > static_cast<int>(MEMORY_CARD_FILE_REGION_PREFIXES.size()))
      m_ui.region->removeItem(m_ui.region->count() - 1);

    const std::string regionStr = region.toStdString();
    size_t i;
    for (i = 0; i < MEMORY_CARD_FILE_REGION_PREFIXES.size(); i++)
    {
      if (regionStr == MEMORY_CARD_FILE_REGION_PREFIXES[i].second)
      {
        m_ui.region->setCurrentIndex(static_cast<int>(i));
        break;
      }
    }
    if (i == MEMORY_CARD_FILE_REGION_PREFIXES.size())
    {
      m_ui.region->addItem(tr("Unknown (%1)").arg(region), region);
      m_ui.region->setCurrentIndex(m_ui.region->count() - 1);
    }
  }

  {
    QSignalBlocker sb(m_ui.serial);
    m_ui.serial->setText(serial);
  }

  {
    QSignalBlocker sb(m_ui.filename);
    m_ui.filename->setText(filename);
  }
}

void MemoryCardRenameFileDialog::updateFullNameFromSimplifiedFields()
{
  const QString region = m_ui.region->currentData().toString();
  const QString serial = m_ui.serial->text()
                           .left(MemoryCardImage::FILE_SERIAL_LENGTH)
                           .leftJustified(MemoryCardImage::FILE_SERIAL_LENGTH, QChar(' '));
  const QString filename = m_ui.filename->text()
                             .left(MemoryCardImage::FILE_FILENAME_LENGTH)
                             .leftJustified(MemoryCardImage::FILE_FILENAME_LENGTH, QChar(' '));

  const QSignalBlocker sb(m_ui.fullFilename);
  m_ui.fullFilename->setText(QStringLiteral("%1%2%3").arg(region).arg(serial).arg(filename));
}
