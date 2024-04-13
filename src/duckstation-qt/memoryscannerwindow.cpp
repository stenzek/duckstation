// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "memoryscannerwindow.h"
#include "cheatcodeeditordialog.h"
#include "common/assert.h"
#include "common/string_util.h"
#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/host.h"
#include "core/system.h"
#include "qthost.h"
#include "qtutils.h"
#include <QtCore/QFileInfo>
#include <QtGui/QColor>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTreeWidgetItemIterator>
#include <array>
#include <utility>

static constexpr std::array<const char*, 6> s_size_strings = {
  {TRANSLATE_NOOP("MemoryScannerWindow", "Byte"), TRANSLATE_NOOP("MemoryScannerWindow", "Halfword"),
   TRANSLATE_NOOP("MemoryScannerWindow", "Word"), TRANSLATE_NOOP("MemoryScannerWindow", "Signed Byte"),
   TRANSLATE_NOOP("MemoryScannerWindow", "Signed Halfword"), TRANSLATE_NOOP("MemoryScannerWindow", "Signed Word")}};

static QString formatHexValue(u32 value, u8 size)
{
  return QStringLiteral("0x%1").arg(static_cast<uint>(value), size, 16, QChar('0'));
}

static QString formatHexAndDecValue(u32 value, u8 size, bool is_signed)
{

  if (is_signed)
  {
    u32 value_raw = value;
    if (size == 2)
      value_raw &= 0xFF;
    else if (size == 4)
      value_raw &= 0xFFFF;
    return QStringLiteral("0x%1 (%2)")
      .arg(static_cast<u32>(value_raw), size, 16, QChar('0'))
      .arg(static_cast<int>(value));
  }
  else
    return QStringLiteral("0x%1 (%2)").arg(static_cast<u32>(value), size, 16, QChar('0')).arg(static_cast<uint>(value));
}

static QString formatCheatCode(u32 address, u32 value, const MemoryAccessSize size)
{

  if (size == MemoryAccessSize::Byte && address <= 0x00200000)
    return QStringLiteral("CHEAT CODE: %1 %2")
      .arg(static_cast<u32>(address) + 0x30000000, 8, 16, QChar('0'))
      .toUpper()
      .arg(static_cast<u16>(value), 4, 16, QChar('0'))
      .toUpper();
  else if (size == MemoryAccessSize::HalfWord && address <= 0x001FFFFE)
    return QStringLiteral("CHEAT CODE: %1 %2")
      .arg(static_cast<u32>(address) + 0x80000000, 8, 16, QChar('0'))
      .toUpper()
      .arg(static_cast<u16>(value), 4, 16, QChar('0'))
      .toUpper();
  else if (size == MemoryAccessSize::Word && address <= 0x001FFFFC)
    return QStringLiteral("CHEAT CODE: %1 %2")
      .arg(static_cast<u32>(address) + 0x90000000, 8, 16, QChar('0'))
      .toUpper()
      .arg(static_cast<u32>(value), 8, 16, QChar('0'))
      .toUpper();
  else
    return QStringLiteral("OUTSIDE RAM RANGE. POKE %1 with %2")
      .arg(static_cast<u32>(address), 8, 16, QChar('0'))
      .toUpper()
      .arg(static_cast<u16>(value), 8, 16, QChar('0'))
      .toUpper();
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
  connectUi();

  m_ui.cheatEngineAddress->setText(tr("Address of RAM for HxD Usage: 0x%1").arg(reinterpret_cast<qulonglong>(Bus::g_unprotected_ram), 16, 16, QChar('0')));
}

MemoryScannerWindow::~MemoryScannerWindow() = default;

void MemoryScannerWindow::connectUi()
{
  m_ui.scanStartAddress->setText(formatHexValue(m_scanner.GetStartAddress(), 8));
  m_ui.scanEndAddress->setText(formatHexValue(m_scanner.GetEndAddress(), 8));

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
    if (value.startsWith(QStringLiteral("0x")) && value.length() > 2)
      address = value.mid(2).toUInt(nullptr, 16);
    else
      address = value.toUInt(nullptr, 16);
    m_scanner.SetStartAddress(static_cast<PhysicalMemoryAddress>(address));
  });
  connect(m_ui.scanEndAddress, &QLineEdit::textChanged, [this](const QString& value) {
    uint address;
    if (value.startsWith(QStringLiteral("0x")) && value.length() > 2)
      address = value.mid(2).toUInt(nullptr, 16);
    else
      address = value.toUInt(nullptr, 16);
    m_scanner.SetEndAddress(static_cast<PhysicalMemoryAddress>(address));
  });
  connect(m_ui.scanPresetRange, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index == 0)
    {
      m_ui.scanStartAddress->setText(formatHexValue(0, 8));
      m_ui.scanEndAddress->setText(formatHexValue(Bus::g_ram_size, 8));
    }
    else if (index == 1)
    {
      m_ui.scanStartAddress->setText(formatHexValue(CPU::SCRATCHPAD_ADDR, 8));
      m_ui.scanEndAddress->setText(formatHexValue(CPU::SCRATCHPAD_ADDR + CPU::SCRATCHPAD_SIZE, 8));
    }
    else
    {
      m_ui.scanStartAddress->setText(formatHexValue(Bus::BIOS_BASE, 8));
      m_ui.scanEndAddress->setText(formatHexValue(Bus::BIOS_BASE + Bus::BIOS_SIZE, 8));
    }
  });
  connect(m_ui.scanNewSearch, &QPushButton::clicked, [this]() {
    m_scanner.Search();
    updateResults();
  });
  connect(m_ui.scanSearchAgain, &QPushButton::clicked, [this]() {
    m_scanner.SearchAgain();
    updateResults();
  });
  connect(m_ui.scanResetSearch, &QPushButton::clicked, [this]() {
    m_scanner.ResetSearch();
    updateResults();
  });
  connect(m_ui.scanAddWatch, &QPushButton::clicked, this, &MemoryScannerWindow::addToWatchClicked);
  connect(m_ui.scanAddManualAddress, &QPushButton::clicked, this, &MemoryScannerWindow::addManualWatchAddressClicked);
  connect(m_ui.scanRemoveWatch, &QPushButton::clicked, this, &MemoryScannerWindow::removeWatchClicked);
  connect(m_ui.scanTable, &QTableWidget::currentItemChanged, this, &MemoryScannerWindow::scanCurrentItemChanged);
  connect(m_ui.watchTable, &QTableWidget::currentItemChanged, this, &MemoryScannerWindow::watchCurrentItemChanged);
  connect(m_ui.scanTable, &QTableWidget::itemChanged, this, &MemoryScannerWindow::scanItemChanged);
  connect(m_ui.watchTable, &QTableWidget::itemChanged, this, &MemoryScannerWindow::watchItemChanged);

  m_update_timer = new QTimer(this);
  connect(m_update_timer, &QTimer::timeout, this, &MemoryScannerWindow::updateScanUi);

  connect(g_emu_thread, &EmuThread::systemStarted, this, &MemoryScannerWindow::onSystemStarted);
  connect(g_emu_thread, &EmuThread::systemDestroyed, this, &MemoryScannerWindow::onSystemDestroyed);

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
  m_ui.scanRemoveWatch->setEnabled(enabled && !m_ui.watchTable->selectedItems().empty());
}

void MemoryScannerWindow::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  resizeColumns();
}

void MemoryScannerWindow::closeEvent(QCloseEvent* event)
{
  QWidget::closeEvent(event);
  emit closed();
}

void MemoryScannerWindow::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  resizeColumns();
}

void MemoryScannerWindow::resizeColumns()
{
  QtUtils::ResizeColumnsForTableView(m_ui.scanTable, {-1, 130, 130});
  QtUtils::ResizeColumnsForTableView(m_ui.watchTable, {-1, 100, 100, 100, 40});
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
}

void MemoryScannerWindow::onSystemDestroyed()
{
  if (m_update_timer->isActive())
    m_update_timer->stop();

  enableUi(false);
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
    m_watch.AddEntry(fmt::format("0x{:08x}", res.address), res.address, m_scanner.GetSize(), m_scanner.GetValueSigned(),
                     false);
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
  QString selected_item(QInputDialog::getItem(this, windowTitle(), tr("Select data size:"), items, 0, false, &ok));
  int index = items.indexOf(selected_item);
  if (index < 0 || !ok)
    return;

  if (index == 1 || index == 4)
    address.value() &= 0xFFFFFFFE;
  else if (index == 2 || index == 5)
    address.value() &= 0xFFFFFFFC;

  m_watch.AddEntry(fmt::format("0x{:08x}", address.value()), address.value(), static_cast<MemoryAccessSize>(index % 3),
                   (index > 3), false);
  updateWatch();
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

void MemoryScannerWindow::watchCurrentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous)
{
  m_ui.scanRemoveWatch->setEnabled((current != nullptr));
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
      bool value_ok = false;
      if (entry.is_signed)
      {
        int value = item->text().toInt(&value_ok);
        if (value_ok)
          m_watch.SetEntryValue(index, static_cast<u32>(value));
      }
      else
      {
        uint value;
        if (item->text()[1] == 'x' || item->text()[1] == 'X')
          value = item->text().toUInt(&value_ok, 16);
        else
          value = item->text().toUInt(&value_ok);
        if (value_ok)
          m_watch.SetEntryValue(index, static_cast<u32>(value));
      }
    }
    break;

    default:
      break;
  }
}

void MemoryScannerWindow::updateScanValue()
{
  QString value = m_ui.scanValue->text();
  if (value.startsWith(QStringLiteral("0x")))
    value.remove(0, 2);

  bool ok = false;
  uint uint_value = value.toUInt(&ok, (m_ui.scanValueBase->currentIndex() > 0) ? 16 : 10);
  if (ok)
    m_scanner.SetValue(uint_value);
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

    QTableWidgetItem* address_item = new QTableWidgetItem(formatHexValue(res.address, 8));
    address_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
    m_ui.scanTable->setItem(row, 0, address_item);

    QTableWidgetItem* value_item;
    if (m_ui.scanValueBase->currentIndex() == 0)
      value_item = new QTableWidgetItem(formatValue(res.value, m_scanner.GetValueSigned()));
    else if (m_scanner.GetSize() == MemoryAccessSize::Byte)
      value_item = new QTableWidgetItem(formatHexValue(res.value, 2));
    else if (m_scanner.GetSize() == MemoryAccessSize::HalfWord)
      value_item = new QTableWidgetItem(formatHexValue(res.value, 4));
    else
      value_item = new QTableWidgetItem(formatHexValue(res.value, 8));
    m_ui.scanTable->setItem(row, 1, value_item);

    QTableWidgetItem* previous_item;
    if (m_ui.scanValueBase->currentIndex() == 0)
      previous_item = new QTableWidgetItem(formatValue(res.last_value, m_scanner.GetValueSigned()));
    else if (m_scanner.GetSize() == MemoryAccessSize::Byte)
      previous_item = new QTableWidgetItem(formatHexValue(res.last_value, 2));
    else if (m_scanner.GetSize() == MemoryAccessSize::HalfWord)
      previous_item = new QTableWidgetItem(formatHexValue(res.last_value, 4));
    else
      previous_item = new QTableWidgetItem(formatHexValue(res.last_value, 8));

    previous_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
    m_ui.scanTable->setItem(row, 2, previous_item);
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

  int row = 0;
  for (const MemoryScan::Result& res : m_scanner.GetResults())
  {
    if (res.value_changed)
    {
      QTableWidgetItem* item = m_ui.scanTable->item(row, 1);
      if (m_ui.scanValueBase->currentIndex() == 0)
        item->setText(formatValue(res.value, m_scanner.GetValueSigned()));
      else if (m_scanner.GetSize() == MemoryAccessSize::Byte)
        item->setText(formatHexValue(res.value, 2));
      else if (m_scanner.GetSize() == MemoryAccessSize::HalfWord)
        item->setText(formatHexValue(res.value, 4));
      else
        item->setText(formatHexValue(res.value, 8));
      item->setForeground(Qt::red);
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

      QTableWidgetItem* description_item = new QTableWidgetItem(formatCheatCode(res.address, res.value, res.size));
      m_ui.watchTable->setItem(row, 0, description_item);

      QTableWidgetItem* address_item = new QTableWidgetItem(formatHexValue(res.address, 8));
      address_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      m_ui.watchTable->setItem(row, 1, address_item);

      QTableWidgetItem* size_item =
        new QTableWidgetItem(tr(s_size_strings[static_cast<u32>(res.size) + (res.is_signed ? 3 : 0)]));
      size_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      m_ui.watchTable->setItem(row, 2, size_item);

      QTableWidgetItem* value_item;
      if (res.size == MemoryAccessSize::Byte)
        value_item = new QTableWidgetItem(formatHexAndDecValue(res.value, 2, res.is_signed));
      else if (res.size == MemoryAccessSize::HalfWord)
        value_item = new QTableWidgetItem(formatHexAndDecValue(res.value, 4, res.is_signed));
      else
        value_item = new QTableWidgetItem(formatHexAndDecValue(res.value, 8, res.is_signed));

      m_ui.watchTable->setItem(row, 3, value_item);

      QTableWidgetItem* freeze_item = new QTableWidgetItem();
      freeze_item->setFlags(freeze_item->flags() | (Qt::ItemIsEditable | Qt::ItemIsUserCheckable));
      freeze_item->setCheckState(res.freeze ? Qt::Checked : Qt::Unchecked);
      m_ui.watchTable->setItem(row, 4, freeze_item);

      row++;
    }
  }

  m_ui.scanSaveWatch->setEnabled(!entries.empty());
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
      else if (m_scanner.GetSize() == MemoryAccessSize::Byte)
        m_ui.watchTable->item(row, 3)->setText(formatHexValue(res.value, 2));
      else if (m_scanner.GetSize() == MemoryAccessSize::HalfWord)
        m_ui.watchTable->item(row, 3)->setText(formatHexValue(res.value, 4));
      else
        m_ui.watchTable->item(row, 3)->setText(formatHexValue(res.value, 8));
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
