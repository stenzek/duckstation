// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "memorycardeditorwindow.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/host.h"
#include "core/settings.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <QtCore/QFileInfo>
#include <QtGui/QPainter>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QStyledItemDelegate>

#include "moc_memorycardeditorwindow.cpp"

using namespace Qt::StringLiterals;

LOG_CHANNEL(Host);

static constexpr char MEMORY_CARD_IMAGE_FILTER[] =
  QT_TRANSLATE_NOOP("MemoryCardEditorWindow", "DuckStation Memory Card (*.mcd)");
static constexpr char MEMORY_CARD_IMPORT_FILTER[] = QT_TRANSLATE_NOOP(
  "MemoryCardEditorWindow",
  "All Importable Memory Card Types (*.mcd *.mcr *.mc *.gme *.srm *.psm *.ps *.ddf *.mem *.vgs *.psx)");
static constexpr char SINGLE_SAVEFILE_FILTER[] =
  QT_TRANSLATE_NOOP("MemoryCardEditorWindow", "Single Save Files (*.mcs);;All Files (*.*)");
static constexpr std::array<std::pair<ConsoleRegion, const char*>, 3> MEMORY_CARD_FILE_REGION_PREFIXES = {{
  {ConsoleRegion::NTSC_U, "BA"},
  {ConsoleRegion::NTSC_J, "BI"},
  {ConsoleRegion::PAL, "BE"},
}};
static constexpr int MEMORY_CARD_ICON_SIZE = MemoryCardImage::ICON_HEIGHT * 2;
static constexpr int MEMORY_CARD_ICON_FRAME_DURATION_MS = 200;

namespace {
class MemoryCardEditorIconStyleDelegate final : public QStyledItemDelegate
{
public:
  explicit MemoryCardEditorIconStyleDelegate(std::vector<MemoryCardImage::FileInfo>& files, qreal dpr,
                                             u32& current_frame_index, QWidget* parent)
    : QStyledItemDelegate(parent), m_files(files), m_dpr(dpr), m_current_frame_index(current_frame_index)
  {
  }
  ~MemoryCardEditorIconStyleDelegate() = default;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
  {
    const QRect& rc = option.rect;
    if (const QPixmap* icon_frame = getIconFrame(static_cast<size_t>(index.row()), m_current_frame_index, rc))
    {
      // center the icon in the available space
      const int x = rc.x() + std::max((rc.width() - MEMORY_CARD_ICON_SIZE) / 2, 0);
      const int y = rc.y() + std::max((rc.height() - MEMORY_CARD_ICON_SIZE) / 2, 0);
      painter->drawPixmap(x, y, *icon_frame);
    }
  }

  void invalidateIconFrames()
  {
    m_icon_frames.clear();
    m_icon_frames.resize(m_files.size());
  }

  const QPixmap* getIconFrame(size_t file_index, u32 frame_index, const QRect& rc) const
  {
    if (file_index >= m_icon_frames.size())
      return nullptr;

    const MemoryCardImage::FileInfo& fi = m_files[file_index];
    if (fi.icon_frames.empty())
      return nullptr;

    std::vector<QPixmap>& frames = m_icon_frames[file_index];
    if (frames.empty())
      frames.resize(fi.icon_frames.size());

    const size_t real_frame_index = frame_index % static_cast<u32>(frames.size());
    QPixmap& pixmap = frames[real_frame_index];
    if (pixmap.isNull())
    {
      // doing this on the UI thread is a bit ehh, but whatever, they're small images.
      const MemoryCardImage::IconFrame& frame = fi.icon_frames[real_frame_index];
      const int pixmap_size = static_cast<int>(std::ceil(static_cast<qreal>(MEMORY_CARD_ICON_SIZE) * m_dpr));

      QImage image = QImage(reinterpret_cast<const uchar*>(frame.pixels), MemoryCardImage::ICON_WIDTH,
                            MemoryCardImage::ICON_HEIGHT, QImage::Format_RGBA8888);
      image.setDevicePixelRatio(m_dpr);
      if (image.width() != pixmap_size || image.height() != pixmap_size)
        QtUtils::ResizeSharpBilinear(image, pixmap_size, MemoryCardImage::ICON_HEIGHT);

      pixmap = QPixmap::fromImage(image);
    }

    return &pixmap;
  }

  void setDevicePixelRatio(qreal dpr)
  {
    if (m_dpr == dpr)
      return;

    m_dpr = dpr;
    invalidateIconFrames();
  }

  static MemoryCardEditorIconStyleDelegate* getForView(const QTableView* view)
  {
    return static_cast<MemoryCardEditorIconStyleDelegate*>(view->itemDelegateForColumn(0));
  }

private:
  std::vector<MemoryCardImage::FileInfo>& m_files;
  mutable std::vector<std::vector<QPixmap>> m_icon_frames;
  qreal m_dpr = 1.0;
  u32& m_current_frame_index;
};
} // namespace

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
  m_card_a.modified_icon_label = m_ui.cardAModifiedIcon;
  m_card_a.modified_label = m_ui.cardAModified;
  m_card_b.path_cb = m_ui.cardBPath;
  m_card_b.table = m_ui.cardB;
  m_card_b.blocks_free_label = m_ui.cardBUsage;
  m_card_b.modified_icon_label = m_ui.cardBModifiedIcon;
  m_card_b.modified_label = m_ui.cardBModified;

  m_file_icon_width = MEMORY_CARD_ICON_SIZE + (m_card_a.table->showGrid() ? 1 : 0);
  m_file_icon_height = MEMORY_CARD_ICON_SIZE + (m_card_a.table->showGrid() ? 1 : 0);
  QtUtils::SetColumnWidthsForTableView(m_card_a.table, {m_file_icon_width, -1, 155, 45});
  QtUtils::SetColumnWidthsForTableView(m_card_b.table, {m_file_icon_width, -1, 155, 45});

  createCardButtons(&m_card_a, m_ui.buttonBoxA);
  createCardButtons(&m_card_b, m_ui.buttonBoxB);
  connectUi();
  connectCardUi(&m_card_a, m_ui.buttonBoxA);
  connectCardUi(&m_card_b, m_ui.buttonBoxB);
  populateComboBox(m_ui.cardAPath);
  populateComboBox(m_ui.cardBPath);
  updateCardBlocksFree(&m_card_a);
  updateCardBlocksFree(&m_card_b);
  updateButtonState();

  const QString new_card_hover_text(tr("New Card..."));
  const QString open_card_hover_text(tr("Open Card..."));
  m_ui.newCardA->setToolTip(new_card_hover_text);
  m_ui.newCardB->setToolTip(new_card_hover_text);
  m_ui.openCardA->setToolTip(open_card_hover_text);
  m_ui.openCardB->setToolTip(open_card_hover_text);

  m_animation_timer = new QTimer(this);
  m_animation_timer->setInterval(MEMORY_CARD_ICON_FRAME_DURATION_MS);
  connect(m_animation_timer, &QTimer::timeout, this, &MemoryCardEditorWindow::incrementAnimationFrame);
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

bool MemoryCardEditorWindow::event(QEvent* event)
{
  if (event->type() == QEvent::DevicePixelRatioChange)
  {
    const qreal dpr = devicePixelRatio();
    MemoryCardEditorIconStyleDelegate::getForView(m_card_a.table)->setDevicePixelRatio(dpr);
    MemoryCardEditorIconStyleDelegate::getForView(m_card_b.table)->setDevicePixelRatio(dpr);
  }

  return QWidget::event(event);
}

void MemoryCardEditorWindow::closeEvent(QCloseEvent* event)
{
  m_card_a.path_cb->setCurrentIndex(0);
  m_card_b.path_cb->setCurrentIndex(0);

  QWidget::closeEvent(event);
}

void MemoryCardEditorWindow::createCardButtons(Card* card, QDialogButtonBox* buttonBox)
{
  card->modified_icon_label->setPixmap(QIcon(QtHost::GetResourceQPath("images/warning.svg", true)).pixmap(16, 16));
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
  const qreal dpr = devicePixelRatio();
  m_ui.cardA->setItemDelegateForColumn(
    0, new MemoryCardEditorIconStyleDelegate(m_card_a.files, dpr, m_current_frame_index, m_ui.cardA));
  m_ui.cardB->setItemDelegateForColumn(
    0, new MemoryCardEditorIconStyleDelegate(m_card_b.files, dpr, m_current_frame_index, m_ui.cardB));

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
  card->save_button->setEnabled(false);
  card->filename.clear();

  if (filename.isEmpty())
  {
    updateButtonState();
    updateCardBlocksFree(card);
    return false;
  }

  Error error;
  std::string filename_str = filename.toStdString();
  if (!MemoryCardImage::LoadFromFile(&card->data, filename_str.c_str(), &error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                             tr("Failed to load memory card: %1").arg(QString::fromStdString(error.GetDescription())));
    return false;
  }

  card->filename = std::move(filename_str);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
  updateAnimationTimerActive();
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
  MemoryCardEditorIconStyleDelegate::getForView(card->table)->invalidateIconFrames();

  for (const MemoryCardImage::FileInfo& fi : card->files)
  {
    const int row = card->table->rowCount();
    card->table->insertRow(row);

    card->table->setRowHeight(row, m_file_icon_height);

    QString title_str(QString::fromStdString(fi.title));
    if (fi.deleted)
      title_str += tr(" (Deleted)");

    QTableWidgetItem* item = new QTableWidgetItem(title_str);
    setCardTableItemProperties(item, fi);
    card->table->setItem(row, 1, item);

    item = new QTableWidgetItem(QString::fromStdString(fi.filename));
    item->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    setCardTableItemProperties(item, fi);
    card->table->setItem(row, 2, item);

    item = new QTableWidgetItem(QString::number(fi.num_blocks));
    item->setTextAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    setCardTableItemProperties(item, fi);
    card->table->setItem(row, 3, item);
  }
}

void MemoryCardEditorWindow::updateAnimationTimerActive()
{
  bool has_animation_frames = false;
  for (const Card& card : {m_card_a, m_card_b})
  {
    for (const MemoryCardImage::FileInfo& fi : card.files)
    {
      if (fi.icon_frames.size() > 1)
      {
        has_animation_frames = true;
        break;
      }
    }

    if (has_animation_frames)
      break;
  }

  if (m_animation_timer->isActive() != has_animation_frames)
  {
    INFO_LOG("Animation timer is now {}", has_animation_frames ? "active" : "inactive");

    m_current_frame_index = 0;
    if (has_animation_frames)
      m_animation_timer->start();
    else
      m_animation_timer->stop();
  }
}

void MemoryCardEditorWindow::incrementAnimationFrame()
{
  m_current_frame_index++;

  for (QTableWidget* table : {m_ui.cardA, m_ui.cardB})
  {
    const int row_count = table->rowCount();
    if (row_count == 0)
      continue;

    emit table->model()->dataChanged(table->model()->index(0, 0), table->model()->index(row_count - 1, 0),
                                     {Qt::DecorationRole});
  }
}

void MemoryCardEditorWindow::updateCardBlocksFree(Card* card)
{
  if (!card->filename.empty())
  {
    card->blocks_free = MemoryCardImage::GetFreeBlockCount(card->data);
    card->blocks_free_label->setText(tr("%n block(s) free", "", card->blocks_free));
    card->modified_icon_label->setVisible(card->dirty);
    card->modified_label->setVisible(card->dirty);
  }
  else
  {
    card->blocks_free_label->clear();
    card->modified_icon_label->setVisible(false);
    card->modified_label->setVisible(false);
  }
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
  updateAnimationTimerActive();
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
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
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
  card->save_button->setEnabled(false);
  card->dirty = false;
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
  updateAnimationTimerActive();
}

void MemoryCardEditorWindow::saveCard(Card* card)
{
  if (card->filename.empty())
    return;

  Error error;
  if (!MemoryCardImage::SaveToFile(card->data, card->filename.c_str(), &error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
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

  if (QtUtils::MessageBoxQuestion(this, tr("Save memory card?"),
                                  tr("Memory card '%1' is not saved, do you want to save before closing?")
                                    .arg(QString::fromStdString(card->filename))) == QMessageBox::No)
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
      QtUtils::AsyncMessageBox(
        this, QMessageBox::Critical, tr("Error"),
        tr("Destination memory card already contains a save file with the same name (%1) as the one you are attempting "
           "to copy. Please delete this file from the destination memory card before copying.")
          .arg(QString(fi->filename.c_str())));
      return;
    }
  }

  if (dst->blocks_free < fi->num_blocks)
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                             tr("Insufficient blocks, this file needs %1 but only %2 are available.")
                               .arg(fi->num_blocks)
                               .arg(dst->blocks_free));
    return;
  }

  Error error;
  std::vector<u8> buffer;
  if (!MemoryCardImage::ReadFile(src->data, *fi, &buffer, &error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                             tr("Failed to read file %1:\n%2")
                               .arg(QString::fromStdString(fi->filename))
                               .arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  if (!MemoryCardImage::WriteFile(&dst->data, fi->filename, buffer, &error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
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
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                             tr("Failed to delete file %1").arg(QString::fromStdString(fi->filename)));
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
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, tr("Error"),
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
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
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

  MemoryCardRenameFileDialog* const dlg = new MemoryCardRenameFileDialog(this, fi->filename);
  dlg->setAttribute(Qt::WA_DeleteOnClose);

  connect(dlg, &QDialog::accepted, this, [this, dlg] {
    const auto [card, fi] = getSelectedFile();
    if (!fi)
      return;

    const std::string new_name = dlg->getNewName();
    if (new_name.empty())
      return;

    Error error;
    if (!MemoryCardImage::RenameFile(&card->data, *fi, new_name, &error))
    {
      QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
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
  });

  dlg->open();
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
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
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
  updateAnimationTimerActive();
}

void MemoryCardEditorWindow::formatCard(Card* card)
{
  promptForSave(card);

  if (QtUtils::MessageBoxQuestion(
        this, tr("Format memory card?"),
        tr("Formatting the memory card will destroy all saves, and they will not be recoverable. "
           "The memory card which will be formatted is located at '%1'.")
          .arg(QString::fromStdString(card->filename))) != QMessageBox::Yes)
  {
    return;
  }

  clearSelection();

  MemoryCardImage::Format(&card->data);

  setCardDirty(card);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateButtonState();
  updateAnimationTimerActive();
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
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                             tr("Failed to import save from %1:\n%2")
                               .arg(QFileInfo(filename).fileName())
                               .arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  setCardDirty(card);
  updateCardTable(card);
  updateCardBlocksFree(card);
  updateAnimationTimerActive();
}

void MemoryCardEditorWindow::onCardContextMenuRequested(const QPoint& pos)
{
  QTableWidget* table = qobject_cast<QTableWidget*>(sender());
  if (!table)
    return;

  const auto& [card, fi] = getSelectedFile();
  if (!card)
    return;

  QMenu* const menu = QtUtils::NewPopupMenu(this);
  QAction* action = menu->addAction(tr("Delete File"), this, &MemoryCardEditorWindow::doDeleteFile);
  action->setEnabled(fi && !fi->deleted);
  action = menu->addAction(tr("Undelete File"), this, &MemoryCardEditorWindow::doUndeleteFile);
  action->setEnabled(fi && fi->deleted);
  action = menu->addAction(tr("Rename File"), this, &MemoryCardEditorWindow::doRenameSaveFile);
  action->setEnabled(fi != nullptr);
  action = menu->addAction(tr("Export File"), this, &MemoryCardEditorWindow::doExportSaveFile);
  action->setEnabled(fi != nullptr);
  action = menu->addAction(tr("Copy File"), this, &MemoryCardEditorWindow::doCopyFile);
  action->setEnabled(fi && !m_card_a.filename.empty() && !m_card_b.filename.empty());

  menu->popup(table->mapToGlobal(pos));
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
    return {nullptr, nullptr};

  const int index = sel.front().topRow();
  Assert(index >= 0 && static_cast<u32>(index) < card->files.size());

  return {card, &card->files[index]};
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

std::string MemoryCardRenameFileDialog::getNewName() const
{
  return m_ui.fullFilename->text().toStdString();
}

void MemoryCardRenameFileDialog::setupAdditionalUi()
{
  m_ui.icon->setPixmap(QIcon::fromTheme("memcard-line"_L1).pixmap(32));

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
