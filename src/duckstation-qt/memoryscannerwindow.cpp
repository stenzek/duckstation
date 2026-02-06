// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "memoryscannerwindow.h"
#include "mainwindow.h"
#include "memoryeditorwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/settings.h"
#include "core/system.h"

#include "util/translation.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/path.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtGui/QColor>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QTreeWidgetItemIterator>
#include <array>
#include <utility>

#include "moc_memoryscannerwindow.cpp"

using namespace Qt::StringLiterals;

static constexpr std::array<const char*, 6> s_size_strings = {
  {TRANSLATE_NOOP("MemoryScannerWindow", "Byte"), TRANSLATE_NOOP("MemoryScannerWindow", "Halfword"),
   TRANSLATE_NOOP("MemoryScannerWindow", "Word"), TRANSLATE_NOOP("MemoryScannerWindow", "Signed Byte"),
   TRANSLATE_NOOP("MemoryScannerWindow", "Signed Halfword"), TRANSLATE_NOOP("MemoryScannerWindow", "Signed Word")}};

static QString formatHexValue(u32 value, MemoryAccessSize size)
{
  const u32 width = (2u << static_cast<u32>(size));
  return QStringLiteral("0x%1").arg(static_cast<uint>(value), width, 16, QChar('0'));
}

static QString formatHexAndDecValue(u32 value, MemoryAccessSize size, bool is_signed)
{
  const u32 width = (2u << static_cast<u32>(size));
  if (is_signed)
  {
    u32 value_raw = value;
    if (size == MemoryAccessSize::Byte)
      value_raw &= 0xFF;
    else if (size == MemoryAccessSize::HalfWord)
      value_raw &= 0xFFFF;
    return QStringLiteral("%1 (0x%2)")
      .arg(static_cast<int>(value))
      .arg(static_cast<u32>(value_raw), width, 16, QChar('0'));
  }
  else
  {
    return QStringLiteral("0x%1 (%2)")
      .arg(static_cast<u32>(value), width, 16, QChar('0'))
      .arg(static_cast<uint>(value));
  }
}

static std::string formatCheatCode(u32 address, u32 value, MemoryAccessSize size)
{
  std::string ret;
  if (size == MemoryAccessSize::Byte && address <= 0x00200000)
    ret = fmt::format("CHEAT CODE: {:08X} {:02X}", address + 0x30000000u, static_cast<u8>(value));
  else if (size == MemoryAccessSize::HalfWord && address <= 0x001FFFFE)
    ret = fmt::format("CHEAT CODE: {:08X} {:04X}", address + 0x80000000u, static_cast<u16>(value));
  else if (size == MemoryAccessSize::Word && address <= 0x001FFFFC)
    ret = fmt::format("CHEAT CODE: {:08X} {:08X}", address + 0x90000000u, value);
  else
    ret = fmt::format("OUTSIDE RAM RANGE. POKE {:08X} with {:08X}", address, value);

  return ret;
}

static QString formatValue(u32 value, bool is_signed)
{
  if (is_signed)
    return QString::number(static_cast<int>(value));
  else
    return QString::number(static_cast<uint>(value));
}

MemoryScannerWindow::MemoryScannerWindow() : QWidget()
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectUi();

  m_ui.cheatEngineAddress->setText(tr("Address of RAM for HxD Usage: 0x%1")
                                     .arg(reinterpret_cast<qulonglong>(Bus::g_unprotected_ram), 16, 16, QChar('0')));
}

MemoryScannerWindow::~MemoryScannerWindow() = default;

void MemoryScannerWindow::setupAdditionalUi()
{
  QtUtils::SetColumnWidthsForTableView(m_ui.scanTable, {-1, 100, 100, 100});
  QtUtils::SetColumnWidthsForTableView(m_ui.watchTable, {-1, 100, 100, 150, 40});
}

void MemoryScannerWindow::connectUi()
{
  m_ui.scanStartAddress->setText(formatHexValue(m_scanner.GetStartAddress(), MemoryAccessSize::Word));
  m_ui.scanEndAddress->setText(formatHexValue(m_scanner.GetEndAddress(), MemoryAccessSize::Word));
  m_ui.scanOperator->setCurrentIndex(static_cast<int>(m_scanner.GetOperator()));
  m_ui.scanSize->setCurrentIndex(static_cast<int>(m_scanner.GetSize()));

  connect(m_ui.scanValue, &QLineEdit::textChanged, this, &MemoryScannerWindow::updateScanValue);
  connect(m_ui.scanValueBase, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { updateScanValue(); });
  connect(m_ui.scanSize, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    m_scanner.SetSize(static_cast<MemoryAccessSize>(index));
    m_scanner.ResetSearch();
    updateResults();
  });
  connect(m_ui.scanValueSigned, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    m_scanner.SetValueSigned(index == 0);
    m_scanner.ResetSearch();
    updateResults();
  });
  connect(m_ui.scanOperator, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { m_scanner.SetOperator(static_cast<MemoryScan::Operator>(index)); });
  connect(m_ui.scanStartAddress, &QLineEdit::textChanged, [this](const QString& value) {
    uint address;
    if (value.startsWith("0x"_L1) && value.length() > 2)
      address = value.mid(2).toUInt(nullptr, 16);
    else
      address = value.toUInt(nullptr, 16);
    m_scanner.SetStartAddress(static_cast<PhysicalMemoryAddress>(address));
  });
  connect(m_ui.scanEndAddress, &QLineEdit::textChanged, [this](const QString& value) {
    uint address;
    if (value.startsWith("0x"_L1) && value.length() > 2)
      address = value.mid(2).toUInt(nullptr, 16);
    else
      address = value.toUInt(nullptr, 16);
    m_scanner.SetEndAddress(static_cast<PhysicalMemoryAddress>(address));
  });
  connect(m_ui.scanPresetRange, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index == 0)
    {
      m_ui.scanStartAddress->setText(formatHexValue(0, MemoryAccessSize::Word));
      m_ui.scanEndAddress->setText(formatHexValue(Bus::g_ram_size, MemoryAccessSize::Word));
    }
    else if (index == 1)
    {
      m_ui.scanStartAddress->setText(formatHexValue(CPU::SCRATCHPAD_ADDR, MemoryAccessSize::Word));
      m_ui.scanEndAddress->setText(formatHexValue(CPU::SCRATCHPAD_ADDR + CPU::SCRATCHPAD_SIZE, MemoryAccessSize::Word));
    }
    else
    {
      m_ui.scanStartAddress->setText(formatHexValue(Bus::BIOS_BASE, MemoryAccessSize::Word));
      m_ui.scanEndAddress->setText(formatHexValue(Bus::BIOS_BASE + Bus::BIOS_SIZE, MemoryAccessSize::Word));
    }
  });
  connect(m_ui.scanNewSearch, &QPushButton::clicked, this, &MemoryScannerWindow::newSearchClicked);
  connect(m_ui.scanSearchAgain, &QPushButton::clicked, this, &MemoryScannerWindow::searchAgainClicked);
  connect(m_ui.scanResetSearch, &QPushButton::clicked, this, &MemoryScannerWindow::resetSearchClicked);
  connect(m_ui.scanAddWatch, &QPushButton::clicked, this, &MemoryScannerWindow::addToWatchClicked);
  connect(m_ui.scanAddManualAddress, &QPushButton::clicked, this, &MemoryScannerWindow::addManualWatchAddressClicked);
  connect(m_ui.scanFreezeWatch, &QPushButton::clicked, this, &MemoryScannerWindow::freezeWatchClicked);
  connect(m_ui.scanRemoveWatch, &QPushButton::clicked, this, &MemoryScannerWindow::removeWatchClicked);
  connect(m_ui.scanTable, &QTableWidget::currentItemChanged, this, &MemoryScannerWindow::scanCurrentItemChanged);
  connect(m_ui.scanTable, &QTableWidget::itemChanged, this, &MemoryScannerWindow::scanItemChanged);
  connect(m_ui.scanTable, &QTableWidget::itemDoubleClicked, this, &MemoryScannerWindow::scanItemDoubleClicked);
  connect(m_ui.watchTable, &QTableWidget::currentItemChanged, this, &MemoryScannerWindow::watchCurrentItemChanged);
  connect(m_ui.watchTable, &QTableWidget::itemChanged, this, &MemoryScannerWindow::watchItemChanged);
  connect(m_ui.watchTable, &QTableWidget::itemDoubleClicked, this, &MemoryScannerWindow::watchItemDoubleClicked);

  m_update_timer = new QTimer(this);
  connect(m_update_timer, &QTimer::timeout, this, &MemoryScannerWindow::updateScanUi);

  connect(g_core_thread, &CoreThread::systemStarted, this, &MemoryScannerWindow::onSystemStarted);
  connect(g_core_thread, &CoreThread::systemDestroyed, this, &MemoryScannerWindow::onSystemDestroyed);

  if (QtHost::IsSystemValid())
    onSystemStarted();
  else
    enableUi(false);
}

void MemoryScannerWindow::enableUi(bool enabled)
{
  const bool has_results = (m_scanner.GetResultCount() > 0);

  m_ui.scanValue->setEnabled(enabled);
  m_ui.scanValueBase->setEnabled(enabled);
  m_ui.scanValueSigned->setEnabled(enabled);
  m_ui.scanSize->setEnabled(enabled);
  m_ui.scanOperator->setEnabled(enabled);
  m_ui.scanStartAddress->setEnabled(enabled);
  m_ui.scanEndAddress->setEnabled(enabled);
  m_ui.scanPresetRange->setEnabled(enabled);
  m_ui.scanResultCount->setEnabled(enabled);
  m_ui.scanNewSearch->setEnabled(enabled);
  m_ui.scanSearchAgain->setEnabled(enabled && has_results);
  m_ui.scanResetSearch->setEnabled(enabled && has_results);
  m_ui.scanAddWatch->setEnabled(enabled && !m_ui.scanTable->selectedItems().empty());
  m_ui.watchTable->setEnabled(enabled);
  m_ui.scanAddManualAddress->setEnabled(enabled);
  m_ui.scanFreezeWatch->setEnabled(enabled && !m_ui.watchTable->selectedItems().empty());
  m_ui.scanRemoveWatch->setEnabled(enabled && !m_ui.watchTable->selectedItems().empty());
}

void MemoryScannerWindow::closeEvent(QCloseEvent* event)
{
  QtUtils::SaveWindowGeometry(this);
  QWidget::closeEvent(event);
  emit closed();
}

int MemoryScannerWindow::getSelectedResultIndexFirst() const
{
  QList<QTableWidgetSelectionRange> sel = m_ui.scanTable->selectedRanges();
  if (sel.isEmpty())
    return -1;

  return sel.front().topRow();
}

int MemoryScannerWindow::getSelectedResultIndexLast() const
{
  QList<QTableWidgetSelectionRange> sel = m_ui.scanTable->selectedRanges();
  if (sel.isEmpty())
    return -1;

  return sel.front().bottomRow();
}

int MemoryScannerWindow::getSelectedWatchIndexFirst() const
{
  QList<QTableWidgetSelectionRange> sel = m_ui.watchTable->selectedRanges();
  if (sel.isEmpty())
    return -1;

  return sel.front().topRow();
}

int MemoryScannerWindow::getSelectedWatchIndexLast() const
{
  QList<QTableWidgetSelectionRange> sel = m_ui.watchTable->selectedRanges();
  if (sel.isEmpty())
    return -1;

  return sel.front().bottomRow();
}

void MemoryScannerWindow::onSystemStarted()
{
  if (!m_update_timer->isActive())
    m_update_timer->start(SCAN_INTERVAL);

  enableUi(true);

  // this is a bit yuck, but the title is cleared by the time that onSystemDestroyed() is called,
  // which means we can't generate it there to save...
  m_watch_save_filename = QStringLiteral("%1.ini").arg(QtHost::GetCurrentGameTitle()).toStdString();
  Path::SanitizeFileName(&m_watch_save_filename);

  reloadWatches();
}

void MemoryScannerWindow::onSystemDestroyed()
{
  if (m_update_timer->isActive())
    m_update_timer->stop();

  clearWatches();
  m_watch_save_filename = {};

  enableUi(false);
}

void MemoryScannerWindow::newSearchClicked()
{
  // swap back to any value if we're set to changed value
  if (m_ui.scanOperator->currentIndex() == static_cast<int>(MemoryScan::Operator::NotEqualLast))
    m_ui.scanOperator->setCurrentIndex(static_cast<int>(MemoryScan::Operator::Any));

  m_scanner.Search();
  updateResults();

  // swap to changed value if we're set to any value
  if (m_ui.scanOperator->currentIndex() == static_cast<int>(MemoryScan::Operator::Any))
    m_ui.scanOperator->setCurrentIndex(static_cast<int>(MemoryScan::Operator::NotEqualLast));
}

void MemoryScannerWindow::searchAgainClicked()
{
  m_scanner.SearchAgain();
  updateResults();
}

void MemoryScannerWindow::resetSearchClicked()
{
  m_scanner.ResetSearch();
  updateResults();

  // swap back to any value if we're set to changed value
  if (m_ui.scanOperator->currentIndex() == static_cast<int>(MemoryScan::Operator::NotEqualLast))
    m_ui.scanOperator->setCurrentIndex(static_cast<int>(MemoryScan::Operator::Any));
}

void MemoryScannerWindow::addToWatchClicked()
{
  const int indexFirst = getSelectedResultIndexFirst();
  const int indexLast = getSelectedResultIndexLast();
  if (indexFirst < 0)
    return;

  for (int index = indexFirst; index <= indexLast; index++)
  {
    const MemoryScan::Result& res = m_scanner.GetResults()[static_cast<u32>(index)];
    m_watch.AddEntry(formatCheatCode(res.address, res.value, m_scanner.GetSize()), res.address, m_scanner.GetSize(),
                     m_scanner.GetValueSigned(), false);
    updateWatch();
  }
}

void MemoryScannerWindow::addManualWatchAddressClicked()
{
  std::optional<unsigned> address = QtUtils::PromptForAddress(this, windowTitle(), tr("Enter manual address:"), false);
  if (!address.has_value())
    return;

  QStringList items;
  for (const char* title : s_size_strings)
    items.append(tr(title));

  bool ok = false;
  const QString selected_item =
    QInputDialog::getItem(this, windowTitle(), tr("Select data size:"), items, 0, false, &ok);
  const qsizetype index = items.indexOf(selected_item);
  if (index < 0 || !ok)
    return;

  if (index == 1 || index == 4)
    address.value() &= 0xFFFFFFFE;
  else if (index == 2 || index == 5)
    address.value() &= 0xFFFFFFFC;

  const MemoryAccessSize size = static_cast<MemoryAccessSize>(index % 3);
  m_watch.AddEntry(formatCheatCode(address.value(), 0, size), address.value(), size, (index > 3), false);
  updateWatch();
}

void MemoryScannerWindow::freezeWatchClicked()
{
  const int indexFirst = getSelectedWatchIndexFirst();
  const int indexLast = getSelectedWatchIndexLast();
  if (indexFirst < 0)
    return;

  const bool freeze = m_watch.GetEntryFreeze(indexFirst);

  for (int index = indexLast; index >= indexFirst; index--)
  {
    m_watch.SetEntryFreeze(static_cast<u32>(index), !freeze);
    updateWatch();
  }
}

void MemoryScannerWindow::removeWatchClicked()
{
  const int indexFirst = getSelectedWatchIndexFirst();
  const int indexLast = getSelectedWatchIndexLast();
  if (indexFirst < 0)
    return;

  for (int index = indexLast; index >= indexFirst; index--)
  {
    m_watch.RemoveEntry(static_cast<u32>(index));
    updateWatch();
  }
}

void MemoryScannerWindow::scanCurrentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous)
{
  m_ui.scanAddWatch->setEnabled((current != nullptr));
}

void MemoryScannerWindow::scanItemDoubleClicked(QTableWidgetItem* item)
{
  const QModelIndex index = m_ui.scanTable->indexFromItem(item);
  if (!index.isValid() || index.column() != 0)
    return;

  tryOpenAddressInMemoryEditor(item->data(Qt::UserRole).toUInt());
}

void MemoryScannerWindow::tryOpenAddressInMemoryEditor(VirtualMemoryAddress address)
{
  MemoryEditorWindow* const editor = g_main_window->getMemoryEditorWindow();
  if (!editor->scrollToMemoryAddress(address))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, windowTitle(),
                             tr("Failed to open memory editor at specified address."));
    return;
  }

  QtUtils::ShowOrRaiseWindow(editor, g_main_window, true);
}

void MemoryScannerWindow::scanItemChanged(QTableWidgetItem* item)
{
  const u32 index = static_cast<u32>(item->row());
  switch (item->column())
  {
    case 1:
    {
      bool value_ok = false;
      if (m_scanner.GetValueSigned())
      {
        int value = item->text().toInt(&value_ok);
        if (value_ok)
          m_scanner.SetResultValue(index, static_cast<u32>(value));
      }
      else
      {
        uint value = item->text().toUInt(&value_ok);
        if (value_ok)
          m_scanner.SetResultValue(index, static_cast<u32>(value));
      }
    }
    break;

    default:
      break;
  }
}

void MemoryScannerWindow::watchItemChanged(QTableWidgetItem* item)
{
  const u32 index = static_cast<u32>(item->row());
  if (index >= m_watch.GetEntryCount())
    return;

  switch (item->column())
  {
    case 4:
    {
      m_watch.SetEntryFreeze(index, (item->checkState() == Qt::Checked));
    }
    break;

    case 0:
    {
      m_watch.SetEntryDescription(index, item->text().toStdString());
    }
    break;

    case 3:
    {
      const MemoryWatchList::Entry& entry = m_watch.GetEntry(index);
      if (entry.is_signed)
      {
        const std::optional<s32> value = StringUtil::FromChars<s32>(item->text().toStdString());
        if (value.has_value())
          m_watch.SetEntryValue(index, static_cast<u32>(value.value()));
      }
      else
      {
        const std::optional<u32> value = StringUtil::FromCharsWithOptionalBase<u32>(item->text().toStdString());
        if (value.has_value())
          m_watch.SetEntryValue(index, value.value());
      }

      const QSignalBlocker sb(m_ui.watchTable);
      item->setText(formatHexAndDecValue(entry.value, entry.size, entry.is_signed));
    }
    break;

    default:
      break;
  }
}

void MemoryScannerWindow::watchCurrentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous)
{
  m_ui.scanFreezeWatch->setEnabled((current != nullptr));
  m_ui.scanRemoveWatch->setEnabled((current != nullptr));
}

void MemoryScannerWindow::watchItemDoubleClicked(QTableWidgetItem* item)
{
  const QModelIndex index = m_ui.watchTable->indexFromItem(item);
  if (!index.isValid() || index.column() != 1)
    return;

  tryOpenAddressInMemoryEditor(item->data(Qt::UserRole).toUInt());
}

void MemoryScannerWindow::updateScanValue()
{
  QString value = m_ui.scanValue->text();
  if (value.startsWith("0x"_L1))
    value.remove(0, 2);

  bool ok = false;
  uint uint_value = value.toUInt(&ok, (m_ui.scanValueBase->currentIndex() > 0) ? 16 : 10);
  if (ok)
    m_scanner.SetValue(uint_value);
}

QTableWidgetItem* MemoryScannerWindow::createValueItem(MemoryAccessSize size, u32 value, bool is_signed,
                                                       bool editable) const
{
  QTableWidgetItem* item;
  if (m_ui.scanValueBase->currentIndex() == 0)
    item = new QTableWidgetItem(formatValue(value, is_signed));
  else
    item = new QTableWidgetItem(formatHexValue(value, m_scanner.GetSize()));

  if (!editable)
    item->setFlags(item->flags() & ~(Qt::ItemIsEditable));

  item->setTextAlignment(Qt::AlignCenter | Qt::AlignHCenter);
  return item;
}

void MemoryScannerWindow::updateResults()
{
  QSignalBlocker sb(m_ui.scanTable);
  m_ui.scanTable->setRowCount(0);

  int row = 0;
  const MemoryScan::ResultVector& results = m_scanner.GetResults();
  for (const MemoryScan::Result& res : results)
  {
    if (row == MAX_DISPLAYED_SCAN_RESULTS)
      break;

    m_ui.scanTable->insertRow(row);

    QTableWidgetItem* address_item = new QTableWidgetItem(formatHexValue(res.address, MemoryAccessSize::Word));
    address_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
    address_item->setTextAlignment(Qt::AlignCenter | Qt::AlignHCenter);
    address_item->setData(Qt::UserRole, static_cast<uint>(res.address));
    m_ui.scanTable->setItem(row, 0, address_item);

    m_ui.scanTable->setItem(row, 1, createValueItem(m_scanner.GetSize(), res.value, m_scanner.GetValueSigned(), true));
    m_ui.scanTable->setItem(row, 2,
                            createValueItem(m_scanner.GetSize(), res.last_value, m_scanner.GetValueSigned(), false));
    m_ui.scanTable->setItem(row, 3,
                            createValueItem(m_scanner.GetSize(), res.first_value, m_scanner.GetValueSigned(), false));

    row++;
  }

  m_ui.scanResultCount->setText((row < static_cast<int>(results.size())) ?
                                  tr("%1 (only showing first %2)").arg(results.size()).arg(row) :
                                  QString::number(m_scanner.GetResultCount()));

  m_ui.scanResetSearch->setEnabled(!results.empty());
  m_ui.scanSearchAgain->setEnabled(!results.empty());
  m_ui.scanAddWatch->setEnabled(false);
}

void MemoryScannerWindow::updateResultsValues()
{
  QSignalBlocker sb(m_ui.scanTable);

  const QBrush changed_color(QtHost::IsDarkApplicationTheme() ? QColor(255, 80, 80) : QColor(191, 121, 20));

  int row = 0;
  for (const MemoryScan::Result& res : m_scanner.GetResults())
  {
    if (res.value_changed)
    {
      QTableWidgetItem* item = m_ui.scanTable->item(row, 1);
      if (m_ui.scanValueBase->currentIndex() == 0)
        item->setText(formatValue(res.value, m_scanner.GetValueSigned()));
      else
        item->setText(formatHexValue(res.value, m_scanner.GetSize()));
      item->setForeground(changed_color);
    }

    row++;
    if (row == MAX_DISPLAYED_SCAN_RESULTS)
      break;
  }
}

void MemoryScannerWindow::updateWatch()
{
  m_watch.UpdateValues();

  QSignalBlocker sb(m_ui.watchTable);
  m_ui.watchTable->setRowCount(0);

  const MemoryWatchList::EntryVector& entries = m_watch.GetEntries();
  if (!entries.empty())
  {
    int row = 0;
    for (const MemoryWatchList::Entry& res : entries)
    {
      m_ui.watchTable->insertRow(row);

      QTableWidgetItem* description_item = new QTableWidgetItem(QString::fromStdString(res.description));
      m_ui.watchTable->setItem(row, 0, description_item);

      QTableWidgetItem* address_item = new QTableWidgetItem(formatHexValue(res.address, MemoryAccessSize::Word));
      address_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      address_item->setData(Qt::UserRole, static_cast<uint>(res.address));
      m_ui.watchTable->setItem(row, 1, address_item);

      QTableWidgetItem* size_item =
        new QTableWidgetItem(tr(s_size_strings[static_cast<u32>(res.size) + (res.is_signed ? 3 : 0)]));
      size_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      m_ui.watchTable->setItem(row, 2, size_item);

      QTableWidgetItem* value_item = new QTableWidgetItem(formatHexAndDecValue(res.value, res.size, res.is_signed));

      m_ui.watchTable->setItem(row, 3, value_item);

      QTableWidgetItem* freeze_item = new QTableWidgetItem();
      freeze_item->setFlags(freeze_item->flags() | (Qt::ItemIsEditable | Qt::ItemIsUserCheckable));
      freeze_item->setCheckState(res.freeze ? Qt::Checked : Qt::Unchecked);
      m_ui.watchTable->setItem(row, 4, freeze_item);

      row++;
    }
  }

  m_ui.scanSaveWatch->setEnabled(!entries.empty());
  m_ui.scanFreezeWatch->setEnabled(false);
  m_ui.scanRemoveWatch->setEnabled(false);
}

void MemoryScannerWindow::updateWatchValues()
{
  QSignalBlocker sb(m_ui.watchTable);
  int row = 0;
  for (const MemoryWatchList::Entry& res : m_watch.GetEntries())
  {
    if (res.changed)
    {
      if (m_ui.scanValueBase->currentIndex() == 0)
        m_ui.watchTable->item(row, 3)->setText(formatValue(res.value, res.is_signed));
      else
        m_ui.watchTable->item(row, 3)->setText(formatHexAndDecValue(res.value, res.size, res.is_signed));
    }
    row++;
  }
}

void MemoryScannerWindow::updateScanUi()
{
  m_scanner.UpdateResultsValues();
  m_watch.UpdateValues();

  updateResultsValues();
  updateWatchValues();
}

std::string MemoryScannerWindow::getWatchSavePath(bool saving)
{
  std::string ret;

  if (m_watch_save_filename.empty())
    return ret;

  const std::string dir = Path::Combine(EmuFolders::DataRoot, "watches");
  if (saving && !FileSystem::DirectoryExists(dir.c_str()))
  {
    Error error;
    if (!FileSystem::CreateDirectory(dir.c_str(), false, &error))
    {
      QtUtils::AsyncMessageBox(
        this, QMessageBox::Critical, windowTitle(),
        tr("Failed to create watches directory: %1").arg(QString::fromStdString(error.GetDescription())));
      return ret;
    }
  }

  ret = Path::Combine(dir, m_watch_save_filename);
  return ret;
}

void MemoryScannerWindow::saveWatches()
{
  if (!m_watch.HasEntriesChanged())
    return;

  const std::string path = getWatchSavePath(true);
  if (path.empty())
    return;

  Error error;
  if (!m_watch.SaveToFile(path.c_str(), &error))
  {
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, windowTitle(),
      tr("Failed to save watches to file: %1").arg(QString::fromStdString(error.GetDescription())));
  }
}

void MemoryScannerWindow::reloadWatches()
{
  saveWatches();

  m_watch.ClearEntries();

  const std::string path = getWatchSavePath(false);
  if (!path.empty() && FileSystem::FileExists(path.c_str()))
  {
    Error error;
    if (!m_watch.LoadFromFile(path.c_str(), &error))
    {
      QtUtils::AsyncMessageBox(
        this, QMessageBox::Critical, windowTitle(),
        tr("Failed to load watches from file: %1").arg(QString::fromStdString(error.GetDescription())));
    }
  }

  updateWatch();
}

void MemoryScannerWindow::clearWatches()
{
  saveWatches();

  m_watch.ClearEntries();
  updateWatch();
}
