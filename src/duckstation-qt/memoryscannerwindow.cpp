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

#include <QtCore/QEvent>
#include <QtCore/QLocale>
#include <QtGui/QClipboard>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMenu>
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

static QString formatCheatCode(u32 address, u32 value, MemoryAccessSize size)
{
  // codes use physical addresses
  address &= 0x1FFFFFFFu;

  if (address >= g_bus.ram_mapped_size)
    return {};

  address &= g_bus.ram_mask;
  if (size == MemoryAccessSize::Byte)
    return QString::fromStdString(fmt::format("{:08X} {:02X}", address + 0x30000000u, static_cast<u8>(value)));
  else if (size == MemoryAccessSize::HalfWord)
    return QString::fromStdString(fmt::format("{:08X} {:04X}", address + 0x80000000u, static_cast<u16>(value)));
  else
    return QString::fromStdString(fmt::format("{:08X} {:08X}", address + 0x90000000u, value));
}

static std::string formatWatchDescription(u32 address, u32 value, MemoryAccessSize size)
{
  return MemoryScannerWindow::tr("%1 (initial value %2)")
    .arg(formatHexValue(address, MemoryAccessSize::Word))
    .arg(formatHexValue(value, size))
    .toStdString();
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

  m_ui.cheatEngineAddress->setText(tr("Host RAM Address: 0x%1")
                                     .arg(reinterpret_cast<qulonglong>(g_bus.unprotected_ram), 16, 16, QChar('0')));
}

MemoryScannerWindow::~MemoryScannerWindow() = default;

void MemoryScannerWindow::setupAdditionalUi()
{
  QtUtils::SetColumnWidthsForTableView(m_ui.scanTable, {-1, 100, 100, 100});
  QtUtils::SetColumnWidthsForTableView(m_ui.watchTable, {-1, 100, 100, 150, 0});

  const QFont& fixed_font = QtHost::GetFixedFont();
  m_ui.scanTable->setFont(fixed_font);

  const int row_height = std::max(fontMetrics().height(), QFontMetrics(fixed_font).height()) + 8;
  for (QTableWidget* table : {m_ui.scanTable, m_ui.watchTable})
  {
    table->verticalHeader()->setMinimumSectionSize(row_height);
    table->verticalHeader()->setDefaultSectionSize(row_height);
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QTableWidget::customContextMenuRequested, this, [this, table](const QPoint& pos) {
      QMenu* const menu = QtUtils::NewPopupMenu(this);
      menu->addActions(table->actions());
      menu->popup(table->viewport()->mapToGlobal(pos));
    });
  }

  // give extra height to the watch list, not the search parameters
  m_ui.splitter->setStretchFactor(0, 0);
  m_ui.splitter->setStretchFactor(1, 1);

  // used to draw placeholder text when the lists are empty
  m_ui.scanTable->viewport()->installEventFilter(this);
  m_ui.watchTable->viewport()->installEventFilter(this);

  const auto add_action = [this](QTableWidget* table, const QString& icon, const QString& text,
                                 QKeySequence::StandardKey shortcut, auto&& func) {
    QAction* const action = new QAction(QIcon(icon), text, table);
    if (shortcut != QKeySequence::UnknownKey)
    {
      action->setShortcut(shortcut);
      action->setShortcutContext(Qt::WidgetShortcut);
      action->setShortcutVisibleInContextMenu(true);
    }
    connect(action, &QAction::triggered, this, std::forward<decltype(func)>(func));
    table->addAction(action);
  };
  const auto add_separator = [](QTableWidget* table) {
    QAction* const action = new QAction(table);
    action->setSeparator(true);
    table->addAction(action);
  };
  const auto copy_selected = [](QTableWidget* table, const auto& get_text) {
    QStringList lines;
    for (const QTableWidgetSelectionRange& range : table->selectedRanges())
    {
      for (int row = range.topRow(); row <= range.bottomRow(); row++)
      {
        if (QString text = get_text(row); !text.isEmpty())
          lines.append(std::move(text));
      }
    }
    if (!lines.isEmpty())
      QGuiApplication::clipboard()->setText(lines.join(u'\n'));
  };

  add_action(m_ui.scanTable, u":/icons/monochrome/svg/eye-line.svg"_s, tr("Watch Selected"), QKeySequence::UnknownKey,
             &MemoryScannerWindow::addToWatchClicked);
  add_action(m_ui.scanTable, u":/icons/monochrome/svg/chip-line.svg"_s, tr("Open in Memory Editor"),
             QKeySequence::UnknownKey, [this]() {
               if (const QTableWidgetItem* item = m_ui.scanTable->item(m_ui.scanTable->currentRow(), 0))
                 tryOpenAddressInMemoryEditor(item->data(Qt::UserRole).toUInt());
             });
  add_action(m_ui.scanTable, u":/icons/monochrome/svg/file-copy-line.svg"_s, tr("Copy Address"), QKeySequence::Copy,
             [this, copy_selected]() {
               copy_selected(m_ui.scanTable, [this](int row) { return m_ui.scanTable->item(row, 0)->text(); });
             });

  add_action(m_ui.watchTable, u":/icons/monochrome/svg/snowflake-line.svg"_s, tr("Toggle Freeze"),
             QKeySequence::UnknownKey, &MemoryScannerWindow::toggleWatchFreezeClicked);
  add_action(m_ui.watchTable, u":/icons/monochrome/svg/chip-line.svg"_s, tr("Open in Memory Editor"),
             QKeySequence::UnknownKey, [this]() {
               if (const QTableWidgetItem* item = m_ui.watchTable->item(m_ui.watchTable->currentRow(), 1))
                 tryOpenAddressInMemoryEditor(item->data(Qt::UserRole).toUInt());
             });
  add_action(m_ui.watchTable, u":/icons/monochrome/svg/file-copy-line.svg"_s, tr("Copy Code"), QKeySequence::Copy,
             [this, copy_selected]() {
               copy_selected(m_ui.watchTable, [this](int row) {
                 const MemoryWatchList::Entry& entry = m_watch.GetEntry(static_cast<u32>(row));
                 return formatCheatCode(entry.address, entry.value, entry.size);
               });
             });
  add_separator(m_ui.watchTable);
  add_action(m_ui.watchTable, u":/icons/monochrome/svg/trash-fill.svg"_s, tr("Remove"), QKeySequence::Delete,
             &MemoryScannerWindow::removeWatchClicked);
}

bool MemoryScannerWindow::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Paint)
  {
    if (object == m_ui.scanTable->viewport())
    {
      if (QtUtils::DrawItemViewPlaceholderText(
            m_ui.scanTable, m_has_searched ?
                              tr("No matches found. Adjust the search parameters and click New Search.") :
                              tr("No results. Choose search parameters and click New Search.")))
      {
        return true;
      }
    }
    else if (object == m_ui.watchTable->viewport())
    {
      if (QtUtils::DrawItemViewPlaceholderText(
            m_ui.watchTable, tr("Select results above and click Watch Selected, or add an address manually.")))
      {
        return true;
      }
    }
  }

  return QWidget::eventFilter(object, event);
}

void MemoryScannerWindow::connectUi()
{
  m_ui.startAddress->setText(formatHexValue(m_scanner.GetStartAddress(), MemoryAccessSize::Word));
  m_ui.endAddress->setText(formatHexValue(m_scanner.GetEndAddress(), MemoryAccessSize::Word));
  m_ui.scanOperator->setCurrentIndex(static_cast<int>(m_scanner.GetOperator()));
  m_ui.scanSize->setCurrentIndex(static_cast<int>(m_scanner.GetSize()));

  connect(m_ui.scanValue, &QLineEdit::textChanged, this, &MemoryScannerWindow::updateScanValue);
  connect(m_ui.valueBase, &QComboBox::currentIndexChanged, [this](int index) {
    m_ui.scanValue->setPlaceholderText((index > 0) ? u"1A2B"_s : u"1234"_s);
    updateScanValue();
  });
  m_ui.scanValue->setPlaceholderText((m_ui.valueBase->currentIndex() > 0) ? u"1A2B"_s : u"1234"_s);
  connect(m_ui.scanValue, &QLineEdit::returnPressed,
          [this]() { (m_ui.searchAgain->isEnabled() ? m_ui.searchAgain : m_ui.newSearch)->click(); });
  connect(m_ui.scanSize, &QComboBox::currentIndexChanged, [this](int index) {
    m_scanner.SetSize(static_cast<MemoryAccessSize>(index));
    m_scanner.ResetSearch();
    m_has_searched = false;
    updateResults();
  });
  connect(m_ui.valueSigned, &QComboBox::currentIndexChanged, [this](int index) {
    m_scanner.SetValueSigned(index == 0);
    m_scanner.ResetSearch();
    m_has_searched = false;
    updateResults();
  });
  connect(m_ui.scanOperator, &QComboBox::currentIndexChanged,
          [this](int index) { m_scanner.SetOperator(static_cast<MemoryScan::Operator>(index)); });
  connect(m_ui.startAddress, &QLineEdit::textChanged, [this](const QString& value) {
    uint address;
    if (value.startsWith("0x"_L1) && value.length() > 2)
      address = value.mid(2).toUInt(nullptr, 16);
    else
      address = value.toUInt(nullptr, 16);
    m_scanner.SetStartAddress(static_cast<PhysicalMemoryAddress>(address));
  });
  connect(m_ui.endAddress, &QLineEdit::textChanged, [this](const QString& value) {
    uint address;
    if (value.startsWith("0x"_L1) && value.length() > 2)
      address = value.mid(2).toUInt(nullptr, 16);
    else
      address = value.toUInt(nullptr, 16);
    m_scanner.SetEndAddress(static_cast<PhysicalMemoryAddress>(address));
  });
  connect(m_ui.presetRange, &QComboBox::currentIndexChanged, [this](int index) {
    if (index == 0)
    {
      m_ui.startAddress->setText(formatHexValue(0x80000000u, MemoryAccessSize::Word));
      m_ui.endAddress->setText(formatHexValue(0x80000000u + g_bus.ram_size, MemoryAccessSize::Word));
    }
    else if (index == 1)
    {
      m_ui.startAddress->setText(formatHexValue(CPU::SCRATCHPAD_ADDR, MemoryAccessSize::Word));
      m_ui.endAddress->setText(formatHexValue(CPU::SCRATCHPAD_ADDR + CPU::SCRATCHPAD_SIZE, MemoryAccessSize::Word));
    }
    else if (index == 2)
    {
      m_ui.startAddress->setText(formatHexValue(0xA0000000u | Bus::BIOS_BASE, MemoryAccessSize::Word));
      m_ui.endAddress->setText(formatHexValue((0xA0000000u | Bus::BIOS_BASE) + Bus::BIOS_SIZE, MemoryAccessSize::Word));
    }
  });
  connect(m_ui.startAddress, &QLineEdit::textEdited, [this]() { m_ui.presetRange->setCurrentIndex(3); });
  connect(m_ui.endAddress, &QLineEdit::textEdited, [this]() { m_ui.presetRange->setCurrentIndex(3); });
  connect(m_ui.newSearch, &QPushButton::clicked, this, &MemoryScannerWindow::newSearchClicked);
  connect(m_ui.searchAgain, &QPushButton::clicked, this, &MemoryScannerWindow::searchAgainClicked);
  connect(m_ui.resetSearch, &QPushButton::clicked, this, &MemoryScannerWindow::resetSearchClicked);
  connect(m_ui.addWatch, &QPushButton::clicked, this, &MemoryScannerWindow::addToWatchClicked);
  connect(m_ui.scanAddManualAddress, &QPushButton::clicked, this, &MemoryScannerWindow::addManualWatchAddressClicked);
  connect(m_ui.toggleWatchFreeze, &QPushButton::clicked, this, &MemoryScannerWindow::toggleWatchFreezeClicked);
  connect(m_ui.removeWatch, &QPushButton::clicked, this, &MemoryScannerWindow::removeWatchClicked);
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

  m_ui.scanGroup->setEnabled(enabled);
  m_ui.searchAgain->setEnabled(enabled && has_results);
  m_ui.resetSearch->setEnabled(enabled && has_results);
  m_ui.addWatch->setEnabled(enabled && !m_ui.scanTable->selectedItems().empty());
  m_ui.scanTable->setEnabled(enabled);
  m_ui.watchTable->setEnabled(enabled);
  m_ui.scanAddManualAddress->setEnabled(enabled);
  m_ui.toggleWatchFreeze->setEnabled(enabled && !m_ui.watchTable->selectedItems().empty());
  m_ui.removeWatch->setEnabled(enabled && !m_ui.watchTable->selectedItems().empty());
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
  m_has_searched = true;
  updateResults();

  // swap to changed value if we're set to any value
  if (m_ui.scanOperator->currentIndex() == static_cast<int>(MemoryScan::Operator::Any))
    m_ui.scanOperator->setCurrentIndex(static_cast<int>(MemoryScan::Operator::NotEqualLast));
}

void MemoryScannerWindow::searchAgainClicked()
{
  m_scanner.SearchAgain();
  m_has_searched = true;
  updateResults();
}

void MemoryScannerWindow::resetSearchClicked()
{
  m_scanner.ResetSearch();
  m_has_searched = false;
  updateResults();

  // swap back to any value if we're set to changed value
  if (m_ui.scanOperator->currentIndex() == static_cast<int>(MemoryScan::Operator::NotEqualLast))
    m_ui.scanOperator->setCurrentIndex(static_cast<int>(MemoryScan::Operator::Any));
}

void MemoryScannerWindow::addToWatchClicked()
{
  if (!QtHost::IsSystemValid())
    return;

  const int indexFirst = getSelectedResultIndexFirst();
  const int indexLast = getSelectedResultIndexLast();
  if (indexFirst < 0)
    return;

  for (int index = indexFirst; index <= indexLast; index++)
  {
    const MemoryScan::Result& res = m_scanner.GetResults()[static_cast<u32>(index)];
    m_watch.AddEntry(formatWatchDescription(res.address, res.value, m_scanner.GetSize()), res.address,
                     m_scanner.GetSize(), m_scanner.GetValueSigned(), false);
    updateWatch();
  }
}

void MemoryScannerWindow::addManualWatchAddressClicked()
{
  std::optional<unsigned> address = QtUtils::PromptForHexValue(this, windowTitle(), tr("Enter manual address:"));
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
  if (m_watch.AddEntry({}, address.value(), size, (index > 3), false))
  {
    const u32 entry_index = m_watch.GetEntryCount() - 1;
    m_watch.SetEntryDescription(entry_index,
                                formatWatchDescription(address.value(), m_watch.GetEntry(entry_index).value, size));
  }
  updateWatch();
}

void MemoryScannerWindow::toggleWatchFreezeClicked()
{
  const int start = getSelectedWatchIndexFirst();
  if (start < 0)
    return;

  const int end = getSelectedWatchIndexLast();
  for (int index = start; index <= end; index++)
  {
    m_watch.ToggleEntryFrozen(static_cast<u32>(index));
    updateWatch();
  }
}

void MemoryScannerWindow::removeWatchClicked()
{
  const int start = getSelectedWatchIndexFirst();
  if (start < 0)
    return;

  const int end = getSelectedWatchIndexLast();
  for (int index = end; index >= start; index--)
  {
    m_watch.RemoveEntry(static_cast<u32>(index));
    updateWatch();
  }
}

void MemoryScannerWindow::scanCurrentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous)
{
  m_ui.addWatch->setEnabled((current != nullptr));
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
      const bool frozen = (item->checkState() == Qt::Checked);
      m_watch.SetEntryFrozen(index, frozen);

      const QSignalBlocker sb(m_ui.watchTable);
      item->setIcon(frozen ? QIcon(u":/icons/monochrome/svg/snowflake-line.svg"_s) : QIcon());
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
  m_ui.toggleWatchFreeze->setEnabled((current != nullptr));
  m_ui.removeWatch->setEnabled((current != nullptr));
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
  uint uint_value = value.toUInt(&ok, (m_ui.valueBase->currentIndex() > 0) ? 16 : 10);
  if (ok)
    m_scanner.SetValue(uint_value);
}

QTableWidgetItem* MemoryScannerWindow::createValueItem(MemoryAccessSize size, u32 value, bool is_signed,
                                                       bool editable) const
{
  QTableWidgetItem* item;
  if (m_ui.valueBase->currentIndex() == 0)
    item = new QTableWidgetItem(formatValue(value, is_signed));
  else
    item = new QTableWidgetItem(formatHexValue(value, m_scanner.GetSize()));

  if (editable)
    item->setToolTip(tr("Double-click to change the value."));
  else
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

  const QLocale locale;
  m_ui.resultCount->setText((row < static_cast<int>(results.size())) ?
                              tr("%1 (showing first %2)")
                                .arg(locale.toString(static_cast<qlonglong>(results.size())))
                                .arg(locale.toString(row)) :
                              locale.toString(static_cast<qlonglong>(results.size())));

  m_ui.resetSearch->setEnabled(!results.empty());
  m_ui.searchAgain->setEnabled(!results.empty());
  m_ui.addWatch->setEnabled(false);
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
      if (m_ui.valueBase->currentIndex() == 0)
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
      description_item->setToolTip(tr("Double-click to edit the description."));
      m_ui.watchTable->setItem(row, 0, description_item);

      QTableWidgetItem* address_item = new QTableWidgetItem(formatHexValue(res.address, MemoryAccessSize::Word));
      address_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      address_item->setData(Qt::UserRole, static_cast<uint>(res.address));
      address_item->setTextAlignment(Qt::AlignCenter);
      address_item->setFont(QtHost::GetFixedFont());
      m_ui.watchTable->setItem(row, 1, address_item);

      QTableWidgetItem* size_item =
        new QTableWidgetItem(tr(s_size_strings[static_cast<u32>(res.size) + (res.is_signed ? 3 : 0)]));
      size_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      size_item->setTextAlignment(Qt::AlignCenter);
      m_ui.watchTable->setItem(row, 2, size_item);

      QTableWidgetItem* value_item = new QTableWidgetItem(formatHexAndDecValue(res.value, res.size, res.is_signed));
      value_item->setToolTip(tr("Double-click to change the value."));
      value_item->setTextAlignment(Qt::AlignCenter);
      value_item->setFont(QtHost::GetFixedFont());

      m_ui.watchTable->setItem(row, 3, value_item);

      QTableWidgetItem* freeze_item = new QTableWidgetItem();
      freeze_item->setFlags(freeze_item->flags() | (Qt::ItemIsEditable | Qt::ItemIsUserCheckable));
      freeze_item->setCheckState(res.freeze ? Qt::Checked : Qt::Unchecked);
      if (res.freeze)
        freeze_item->setIcon(QIcon(u":/icons/monochrome/svg/snowflake-line.svg"_s));
      m_ui.watchTable->setItem(row, 4, freeze_item);

      row++;
    }
  }

  m_ui.saveWatch->setEnabled(!entries.empty());
  m_ui.toggleWatchFreeze->setEnabled(false);
  m_ui.removeWatch->setEnabled(false);
}

void MemoryScannerWindow::updateWatchValues()
{
  QSignalBlocker sb(m_ui.watchTable);
  int row = 0;
  for (const MemoryWatchList::Entry& res : m_watch.GetEntries())
  {
    if (res.changed)
    {
      if (m_ui.valueBase->currentIndex() == 0)
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
        QStringLiteral("Failed to create watches directory: %1").arg(QString::fromStdString(error.GetDescription())));
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
      QStringLiteral("Failed to save watches to file: %1").arg(QString::fromStdString(error.GetDescription())));
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
        QStringLiteral("Failed to load watches from file: %1").arg(QString::fromStdString(error.GetDescription())));
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
