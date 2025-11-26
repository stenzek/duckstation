// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "memoryeditorwindow.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/bus.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core_private.h"

#include "common/assert.h"
#include "common/string_util.h"

#include <QtGui/QShortcut>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QFileDialog>
#include <bit>

#include "moc_memoryeditorwindow.cpp"

static constexpr int TIMER_REFRESH_INTERVAL_MS = 100;

MemoryEditorWindow::MemoryEditorWindow(QWidget* parent /* = nullptr */) : QWidget(parent)
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();
  updateUIEnabled();
  updateMemoryViewRegion();
  updateDataInspector();
}

MemoryEditorWindow::~MemoryEditorWindow() = default;

void MemoryEditorWindow::onSystemStarted()
{
  updateUIEnabled();
}

void MemoryEditorWindow::onSystemDestroyed()
{
  updateUIEnabled();
}

void MemoryEditorWindow::onSystemPaused()
{
  updateUIEnabled();
  refreshAll();
}

void MemoryEditorWindow::onSystemResumed()
{
  updateUIEnabled();
}

void MemoryEditorWindow::timerRefresh()
{
  m_ui.memoryView->forceRefresh();
  updateDataInspector();
}

void MemoryEditorWindow::refreshAll()
{
  m_ui.memoryView->forceRefresh();
  updateDataInspector();
}

void MemoryEditorWindow::onMemoryViewTopAddressChanged(size_t address)
{
  m_ui.address->setText(formatAddress(static_cast<VirtualMemoryAddress>(address)));
}

void MemoryEditorWindow::onAddressEditingFinished()
{
  QString address_str = m_ui.address->text();
  if (address_str.startsWith(QStringLiteral("0x")) || address_str.startsWith(QStringLiteral("0X")))
    address_str = address_str.mid(2);

  const std::optional<VirtualMemoryAddress> address =
    StringUtil::FromChars<VirtualMemoryAddress>(address_str.toStdString(), 16);
  if (!address.has_value())
  {
    m_ui.address->setText(formatAddress(static_cast<VirtualMemoryAddress>(m_ui.memoryView->topAddress())));
    return;
  }

  scrollToMemoryAddress(address.value());
}

void MemoryEditorWindow::onDumpAddressTriggered()
{
  std::optional<VirtualMemoryAddress> address =
    QtUtils::PromptForAddress(this, windowTitle(), tr("Enter memory address:"), false);
  if (!address.has_value())
    return;

  scrollToMemoryAddress(address.value());
}

void MemoryEditorWindow::onMemoryRegionButtonToggled(QAbstractButton*, bool checked)
{
  if (!checked)
    return;

  updateMemoryViewRegion();
}

void MemoryEditorWindow::onDataInspectorBaseButtonToggled(QAbstractButton*, bool checked)
{
  if (!checked)
    return;

  updateDataInspector();
}

void MemoryEditorWindow::onDataInspectorEndianButtonToggled(QAbstractButton*, bool checked)
{
  if (!checked)
    return;

  updateDataInspector();
}

void MemoryEditorWindow::onMemorySearchTriggered()
{
  m_ui.memoryView->clearHighlightRange();

  const QString pattern_str = m_ui.memorySearchString->text();
  if (pattern_str.isEmpty())
    return;

  std::vector<u8> pattern;
  std::vector<u8> mask;
  u8 spattern = 0;
  u8 smask = 0;
  bool msb = false;

  pattern.reserve(static_cast<size_t>(pattern_str.length()) / 2);
  mask.reserve(static_cast<size_t>(pattern_str.length()) / 2);

  for (int i = 0; i < pattern_str.length(); i++)
  {
    const QChar ch = pattern_str[i];
    if (ch == ' ')
      continue;

    if (ch == '?')
    {
      spattern = (spattern << 4);
      smask = (smask << 4);
    }
    else if (ch.isDigit())
    {
      spattern = (spattern << 4) | static_cast<u8>(ch.digitValue());
      smask = (smask << 4) | 0xF;
    }
    else if (ch.unicode() >= 'a' && ch.unicode() <= 'f')
    {
      spattern = (spattern << 4) | (0xA + static_cast<u8>(ch.unicode() - 'a'));
      smask = (smask << 4) | 0xF;
    }
    else if (ch.unicode() >= 'A' && ch.unicode() <= 'F')
    {
      spattern = (spattern << 4) | (0xA + static_cast<u8>(ch.unicode() - 'A'));
      smask = (smask << 4) | 0xF;
    }
    else
    {
      QtUtils::AsyncMessageBox(this, QMessageBox::Critical, windowTitle(),
                               tr("Invalid search pattern. It should contain hex digits or question marks."));
      return;
    }

    if (msb)
    {
      pattern.push_back(spattern);
      mask.push_back(smask);
      spattern = 0;
      smask = 0;
    }

    msb = !msb;
  }

  if (msb)
  {
    // partial byte on the end
    spattern = (spattern << 4);
    smask = (smask << 4);
    pattern.push_back(spattern);
    mask.push_back(smask);
  }

  if (pattern.empty())
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, windowTitle(),
                             tr("Invalid search pattern. It should contain hex digits or question marks."));
    return;
  }

  std::optional<PhysicalMemoryAddress> found_address =
    Bus::SearchMemory(m_next_memory_search_address, pattern.data(), mask.data(), static_cast<u32>(pattern.size()));
  bool wrapped_around = false;
  if (!found_address.has_value())
  {
    found_address = Bus::SearchMemory(0, pattern.data(), mask.data(), static_cast<u32>(pattern.size()));
    if (!found_address.has_value())
    {
      QtUtils::AsyncMessageBox(this, QMessageBox::Critical, windowTitle(), tr("Pattern not found in memory."));
      return;
    }

    wrapped_around = true;
  }

  m_next_memory_search_address = found_address.value() + 1;
  if (scrollToMemoryAddress(found_address.value()))
  {
    const size_t highlight_offset = found_address.value() - m_ui.memoryView->addressOffset();
    m_ui.memoryView->setHighlightRange(highlight_offset, highlight_offset + pattern.size());
  }

  if (wrapped_around)
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Information, windowTitle(),
                             tr("Pattern found at 0x%1 (passed the end of memory).")
                               .arg(static_cast<uint>(found_address.value()), 8, 16, static_cast<QChar>('0')));
  }
  else
  {
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Information, windowTitle(),
      tr("Pattern found at 0x%1.").arg(static_cast<uint>(found_address.value()), 8, 16, static_cast<QChar>('0')));
  }
}

void MemoryEditorWindow::onMemorySearchStringChanged(const QString&)
{
  m_next_memory_search_address = 0;
}

QString MemoryEditorWindow::formatAddress(VirtualMemoryAddress address)
{
  return QString::asprintf("0x%08X", static_cast<uint>(address));
}

void MemoryEditorWindow::setIfChanged(QLineEdit* const widget, const QString& text)
{
  if (widget->text() == text)
    return;

  widget->setText(text);
  widget->setCursorPosition(0);
}

QString MemoryEditorWindow::formatNumber(u64 value, bool is_signed, int byte_size) const
{
  QString ret;

  QString prefix;
  int base;
  int width;

  if (m_ui.dataInspectorOctal->isChecked())
  {
    prefix = QStringLiteral("0");
    base = 8;
    width = 0;
  }
  else if (m_ui.dataInspectorHexadecimal->isChecked())
  {
    prefix = QStringLiteral("0x");
    base = 16;
    width = byte_size * 2;
  }
  else
  {
    // prefix = QString();
    base = 10;
    width = 0;
  }

  switch (byte_size)
  {
    case 1:
    {
      if (is_signed)
      {
        if (static_cast<s8>(value) < 0)
        {
          ret = QStringLiteral("%1").arg(static_cast<s8>(value), width, base, QChar('0'));
          ret.insert(1, prefix);
        }
        else
        {
          ret = QStringLiteral("%1%2").arg(prefix).arg(static_cast<s8>(value), width, base, QChar('0'));
        }
      }
      else
      {
        ret = QStringLiteral("%1%2").arg(prefix).arg(static_cast<u8>(value), width, base, QChar('0'));
      }
    }
    break;

    case 2:
    {
      if (is_signed)
      {
        if (static_cast<s16>(value) < 0)
        {
          ret = QStringLiteral("%1").arg(static_cast<s16>(value), width, base, QChar('0'));
          ret.insert(1, prefix);
        }
        else
        {
          ret = QStringLiteral("%1%2").arg(prefix).arg(static_cast<s16>(value), width, base, QChar('0'));
        }
      }
      else
      {
        ret = QStringLiteral("%1%2").arg(prefix).arg(static_cast<u16>(value), width, base, QChar('0'));
      }
    }
    break;

    case 4:
    {
      if (is_signed)
      {
        if (static_cast<s32>(value) < 0)
        {
          ret = QStringLiteral("%1").arg(static_cast<s32>(value), width, base, QChar('0'));
          ret.insert(1, prefix);
        }
        else
        {
          ret = QStringLiteral("%1%2").arg(prefix).arg(static_cast<s32>(value), width, base, QChar('0'));
        }
      }
      else
      {
        ret = QStringLiteral("%1%2").arg(prefix).arg(static_cast<u32>(value), width, base, QChar('0'));
      }
    }
    break;

    case 8:
    {
      if (is_signed)
      {
        if (static_cast<s64>(value) < 0)
        {
          ret = QStringLiteral("%1").arg(static_cast<s64>(value), width, base, QChar('0'));
          ret.insert(1, prefix);
        }
        else
        {
          ret = QStringLiteral("%1%2").arg(prefix).arg(static_cast<s64>(value), width, base, QChar('0'));
        }
      }
      else
      {
        ret = QStringLiteral("%1%2").arg(prefix).arg(static_cast<u64>(value), width, base, QChar('0'));
      }
    }
    break;

    default:
      break;
  }

  return ret;
}

void MemoryEditorWindow::setupAdditionalUi()
{
  const QFont& fixed_font = QtHost::GetFixedFont();
  m_ui.memoryView->setFont(fixed_font);

  // Set minimum width for data inspector.
  m_ui.dataInspectorAddress->setFont(fixed_font);
  m_ui.dataInspectorUnsignedByte->setFont(fixed_font);
  m_ui.dataInspectorSignedByte->setFont(fixed_font);
  m_ui.dataInspectorUnsignedHalfword->setFont(fixed_font);
  m_ui.dataInspectorSignedHalfword->setFont(fixed_font);
  m_ui.dataInspectorUnsignedWord->setFont(fixed_font);
  m_ui.dataInspectorSignedWord->setFont(fixed_font);
  m_ui.dataInspectorUnsignedDoubleWord->setFont(fixed_font);
  m_ui.dataInspectorSignedDoubleWord->setFont(fixed_font);
  m_ui.dataInspectorFloat32->setFont(fixed_font);
  m_ui.dataInspectorFloat64->setFont(fixed_font);
  m_ui.dataInspectorASCIICharacter->setFont(fixed_font);
  m_ui.dataInspectorUTF8String->setFont(fixed_font);
  m_ui.dataInspectorSignedDoubleWord->setMinimumWidth(
    QFontMetrics(fixed_font).size(0, QStringLiteral("-8888888888888888888888")).width());

  // Default selection.
  m_ui.memoryRegionRAM->setChecked(true);
  m_ui.dataInspectorHexadecimal->setChecked(true);
  m_ui.dataInspectorLittleEndian->setChecked(true);
}

void MemoryEditorWindow::connectSignals()
{
  connect(g_emu_thread, &EmuThread::systemPaused, this, &MemoryEditorWindow::onSystemPaused);
  connect(g_emu_thread, &EmuThread::systemResumed, this, &MemoryEditorWindow::onSystemResumed);
  connect(g_emu_thread, &EmuThread::systemStarted, this, &MemoryEditorWindow::onSystemStarted);
  connect(g_emu_thread, &EmuThread::systemDestroyed, this, &MemoryEditorWindow::onSystemDestroyed);

  connect(m_ui.address, &QLineEdit::editingFinished, this, &MemoryEditorWindow::onAddressEditingFinished);

  connect(m_ui.memoryView, &MemoryViewWidget::topAddressChanged, this,
          &MemoryEditorWindow::onMemoryViewTopAddressChanged);
  connect(m_ui.memoryView, &MemoryViewWidget::selectedAddressChanged, this, &MemoryEditorWindow::updateDataInspector);
  connect(m_ui.memoryRegionButtonGroup, &QButtonGroup::buttonToggled, this,
          &MemoryEditorWindow::onMemoryRegionButtonToggled);
  connect(m_ui.endianButtonGroup, &QButtonGroup::buttonToggled, this,
          &MemoryEditorWindow::onDataInspectorEndianButtonToggled);
  connect(m_ui.baseButtonGroup, &QButtonGroup::buttonToggled, this,
          &MemoryEditorWindow::onDataInspectorBaseButtonToggled);

  connect(m_ui.memorySearch, &QPushButton::clicked, this, &MemoryEditorWindow::onMemorySearchTriggered);
  connect(m_ui.memorySearchString, &QLineEdit::textChanged, this, &MemoryEditorWindow::onMemorySearchStringChanged);

  connect(&m_refresh_timer, &QTimer::timeout, this, &MemoryEditorWindow::timerRefresh);
  m_refresh_timer.setInterval(TIMER_REFRESH_INTERVAL_MS);

  m_go_shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_G), this);
  connect(m_go_shortcut, &QShortcut::activated, this, &MemoryEditorWindow::onDumpAddressTriggered);
}

void MemoryEditorWindow::updateUIEnabled()
{
  const bool system_valid = QtHost::IsSystemValid();

  m_ui.memoryView->setEnabled(system_valid);
  m_ui.address->setEnabled(system_valid);
  m_ui.memoryRegionRAM->setEnabled(system_valid);
  m_ui.memoryRegionEXP1->setEnabled(system_valid);
  m_ui.memoryRegionScratchpad->setEnabled(system_valid);
  m_ui.memoryRegionBIOS->setEnabled(system_valid);
  m_ui.memorySearch->setEnabled(system_valid);
  m_ui.memorySearchString->setEnabled(system_valid);
  m_ui.dataInspector->setEnabled(system_valid);
  m_go_shortcut->setEnabled(system_valid);

  // Partial/timer refreshes only active when not paused.
  const bool timer_active = system_valid && !QtHost::IsSystemPaused();
  if (m_refresh_timer.isActive() != timer_active)
    timer_active ? m_refresh_timer.start() : m_refresh_timer.stop();
}

void MemoryEditorWindow::closeEvent(QCloseEvent* event)
{
  QtUtils::SaveWindowGeometry(this);
  QWidget::closeEvent(event);
  emit closed();
}

bool MemoryEditorWindow::scrollToMemoryAddress(VirtualMemoryAddress address)
{
  const PhysicalMemoryAddress phys_address = CPU::VirtualAddressToPhysical(address);
  std::optional<Bus::MemoryRegion> region = Bus::GetMemoryRegionForAddress(phys_address);
  if (!region.has_value())
    return false;

  if (region.value() == Bus::MemoryRegion::EXP1)
    m_ui.memoryRegionEXP1->setChecked(true);
  else if (region.value() == Bus::MemoryRegion::Scratchpad)
    m_ui.memoryRegionScratchpad->setChecked(true);
  else if (region.value() == Bus::MemoryRegion::BIOS)
    m_ui.memoryRegionBIOS->setChecked(true);
  else
    m_ui.memoryRegionRAM->setChecked(true);

  const PhysicalMemoryAddress offset = phys_address - Bus::GetMemoryRegionStart(region.value());
  m_ui.memoryView->scrollToOffset(offset);
  return true;
}

void MemoryEditorWindow::updateMemoryViewRegion()
{
  Bus::MemoryRegion region;
  if (m_ui.memoryRegionEXP1->isChecked())
    region = Bus::MemoryRegion::EXP1;
  else if (m_ui.memoryRegionScratchpad->isChecked())
    region = Bus::MemoryRegion::Scratchpad;
  else if (m_ui.memoryRegionBIOS->isChecked())
    region = Bus::MemoryRegion::BIOS;
  else
    region = Bus::MemoryRegion::RAM;

  static constexpr auto edit_ram_callback = [](size_t offset, size_t count) {
    // shouldn't happen
    if (offset > Bus::g_ram_size)
      return;

    const u32 start_page = static_cast<u32>(offset) >> HOST_PAGE_SHIFT;
    const u32 end_page = static_cast<u32>(offset + count - 1) >> HOST_PAGE_SHIFT;
    Host::RunOnCPUThread([start_page, end_page]() {
      for (u32 i = start_page; i <= end_page; i++)
      {
        if (Bus::g_ram_code_bits[i])
          CPU::CodeCache::InvalidateBlocksWithPageIndex(i);
      }
    });
  };

  const PhysicalMemoryAddress start = Bus::GetMemoryRegionStart(region);
  const PhysicalMemoryAddress end = Bus::GetMemoryRegionEnd(region);
  void* const mem_ptr = Bus::GetMemoryRegionPointer(region);
  const bool mem_writable = Bus::IsMemoryRegionWritable(region);
  const MemoryViewWidget::EditCallback edit_callback =
    ((region == Bus::MemoryRegion::RAM) ? static_cast<MemoryViewWidget::EditCallback>(edit_ram_callback) : nullptr);
  m_ui.memoryView->setData(start, mem_ptr, end - start, mem_writable, edit_callback);
}

void MemoryEditorWindow::updateDataInspector()
{
  const size_t address = m_ui.memoryView->selectedAddress();
  if (address == MemoryViewWidget::INVALID_SELECTED_ADDRESS)
  {
    m_ui.dataInspectorAddress->clear();
    m_ui.dataInspectorUnsignedByte->clear();
    m_ui.dataInspectorSignedByte->clear();
    m_ui.dataInspectorUnsignedHalfword->clear();
    m_ui.dataInspectorSignedHalfword->clear();
    m_ui.dataInspectorUnsignedWord->clear();
    m_ui.dataInspectorSignedWord->clear();
    m_ui.dataInspectorUnsignedDoubleWord->clear();
    m_ui.dataInspectorSignedDoubleWord->clear();
    m_ui.dataInspectorFloat32->clear();
    m_ui.dataInspectorFloat64->clear();
    m_ui.dataInspectorASCIICharacter->clear();
    m_ui.dataInspectorUTF8String->clear();
    return;
  }

  u8 value8 = 0;
  u16 value16 = 0;
  u32 value32 = 0;
  u32 value64_high = 0;
  u64 value64 = 0;
  CPU::SafeReadMemoryWord(static_cast<VirtualMemoryAddress>(address), &value32);
  CPU::SafeReadMemoryWord(static_cast<VirtualMemoryAddress>(address), &value64_high);
  value64 = (ZeroExtend64(value64_high) << 32) | ZeroExtend64(value32);

  const bool big_endian = m_ui.dataInspectorBigEndian->isChecked();
  if (big_endian)
  {
    value8 = Truncate8(value32);
    value16 = ByteSwap(Truncate16(value32));
    value32 = ByteSwap(value32);
    value64 = ByteSwap(value64);
  }
  else
  {
    value8 = Truncate8(value32);
    value16 = Truncate16(value32);
  }

  setIfChanged(m_ui.dataInspectorAddress, formatAddress(static_cast<VirtualMemoryAddress>(address)));
  setIfChanged(m_ui.dataInspectorUnsignedByte, formatNumber(value8, false, 1));
  setIfChanged(m_ui.dataInspectorSignedByte, formatNumber(value8, true, 1));
  setIfChanged(m_ui.dataInspectorUnsignedHalfword, formatNumber(value16, false, 2));
  setIfChanged(m_ui.dataInspectorSignedHalfword, formatNumber(value16, true, 2));
  setIfChanged(m_ui.dataInspectorUnsignedWord, formatNumber(value32, false, 4));
  setIfChanged(m_ui.dataInspectorSignedWord, formatNumber(value32, true, 4));
  setIfChanged(m_ui.dataInspectorUnsignedDoubleWord, formatNumber(value64, false, 8));
  setIfChanged(m_ui.dataInspectorSignedDoubleWord, formatNumber(value64, true, 8));
  setIfChanged(m_ui.dataInspectorFloat32, QString::number(std::bit_cast<float>(value32)));
  setIfChanged(m_ui.dataInspectorFloat64, QString::number(std::bit_cast<double>(value64)));

  if (value8 >= 0x20 && value8 <= 0x7E)
  {
    m_ui.dataInspectorASCIICharacter->setText(QStringLiteral("'%1'").arg(static_cast<QChar>(value8)));

    SmallString str;
    CPU::SafeReadMemoryCString(static_cast<VirtualMemoryAddress>(address), &str, 32);

    // only display printable characters
    for (size_t i = 0; i < str.length(); i++)
    {
      if (str[i] < 0x20 || str[i] > 0x7E)
      {
        str.resize(static_cast<u32>(i));
        break;
      }
    }
    if (!str.empty())
    {
      str.prepend('\"');
      str.append('\"');
      setIfChanged(m_ui.dataInspectorUTF8String, QtUtils::StringViewToQString(str));
    }
    else
    {
      m_ui.dataInspectorUTF8String->clear();
    }
  }
  else
  {
    setIfChanged(m_ui.dataInspectorASCIICharacter,
                 QStringLiteral("'\\x%1'").arg(static_cast<uint>(value8), 2, 16, QChar('0')));
    m_ui.dataInspectorUTF8String->clear();
  }
}
