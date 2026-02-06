// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "debuggerwindow.h"
#include "debuggermodels.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/bus.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core_private.h"

#include "common/assert.h"
#include "common/log.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QCursor>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QAbstractScrollArea>
#include <QtWidgets/QFileDialog>

#include "moc_debuggerwindow.cpp"

using namespace Qt::StringLiterals;

LOG_CHANNEL(Host);

static constexpr int TIMER_REFRESH_INTERVAL_MS = 100;

DebuggerWindow::DebuggerWindow(QWidget* parent /* = nullptr */)
  : QMainWindow(parent), m_active_memory_region(Bus::MemoryRegion::Count)
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();
  createModels();
  setMemoryViewRegion(Bus::MemoryRegion::RAM);
  if (QtHost::IsSystemValid() && QtHost::IsSystemPaused())
    onSystemPaused();
  else if (QtHost::IsSystemValid())
    onSystemStarted();
  else
    onSystemDestroyed();
}

DebuggerWindow::~DebuggerWindow() = default;

void DebuggerWindow::onSystemStarted()
{
  setUIEnabled(false, true);
}

void DebuggerWindow::onSystemDestroyed()
{
  setUIEnabled(false, false);
}

void DebuggerWindow::onSystemPaused()
{
  setUIEnabled(true, true);
  refreshAll();

  m_ui.actionPause->setChecked(true);
}

void DebuggerWindow::onSystemResumed()
{
  setUIEnabled(false, true);
  m_ui.codeView->invalidatePC();
  m_ui.actionPause->setChecked(false);
}

void DebuggerWindow::reportMessage(const QString& message)
{
  m_ui.statusbar->showMessage(message, 0);
}

void DebuggerWindow::timerRefresh()
{
  m_ui.memoryView->forceRefresh();
}

void DebuggerWindow::refreshAll()
{
  m_registers_model->updateValues();
  m_stack_model->invalidateView();
  m_ui.memoryView->forceRefresh();

  m_ui.codeView->setPC(CPU::g_state.pc);
  scrollToPC(false);
}

void DebuggerWindow::scrollToPC(bool center)
{
  return scrollToCodeAddress(CPU::g_state.pc, center);
}

void DebuggerWindow::scrollToCodeAddress(VirtualMemoryAddress address, bool center)
{
  m_ui.codeView->scrollToAddress(address, center);
  m_ui.codeView->setSelectedAddress(address);
}

void DebuggerWindow::onPauseActionTriggered(bool paused)
{
  if (!paused)
  {
    saveCurrentState();
    setUIEnabled(false, true);
  }

  g_core_thread->setSystemPaused(paused);
}

void DebuggerWindow::onRunToCursorTriggered()
{
  std::optional<VirtualMemoryAddress> addr = m_ui.codeView->getSelectedAddress();
  if (!addr.has_value())
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, windowTitle(), tr("No address selected."));
    return;
  }

  CPU::AddBreakpoint(CPU::BreakpointType::Execute, addr.value(), true, true);
  g_core_thread->setSystemPaused(false);
}

void DebuggerWindow::onGoToPCTriggered()
{
  scrollToPC(true);
}

void DebuggerWindow::onGoToAddressTriggered()
{
  std::optional<VirtualMemoryAddress> address =
    QtUtils::PromptForAddress(this, windowTitle(), tr("Enter code address:"), true);
  if (!address.has_value())
    return;

  scrollToCodeAddress(address.value(), true);
}

void DebuggerWindow::onDumpAddressTriggered()
{
  std::optional<VirtualMemoryAddress> address =
    QtUtils::PromptForAddress(this, windowTitle(), tr("Enter memory address:"), false);
  if (!address.has_value())
    return;

  scrollToMemoryAddress(address.value());
}

void DebuggerWindow::onTraceTriggered()
{
  Host::RunOnCoreThread([]() {
    const bool trace_enabled = !CPU::IsTraceEnabled();
    if (trace_enabled)
      CPU::StartTrace();
    else
      CPU::StopTrace();

    Host::RunOnUIThread([trace_enabled]() {
      DebuggerWindow* const win = g_main_window->getDebuggerWindow();
      if (!win)
        return;

      if (trace_enabled)
      {
        QtUtils::AsyncMessageBox(
          win, QMessageBox::Critical, win->windowTitle(),
          tr("Trace logging started to cpu_log.txt.\nThis file can be several gigabytes, so be aware of SSD wear."));
      }
      else
      {
        QtUtils::AsyncMessageBox(win, QMessageBox::Critical, win->windowTitle(),
                                 tr("Trace logging to cpu_log.txt stopped."));
      }
    });
  });
}

void DebuggerWindow::onAddBreakpointTriggered()
{
  DebuggerAddBreakpointDialog* const dlg = new DebuggerAddBreakpointDialog(this);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  connect(dlg, &QDialog::accepted, this, [this, dlg] { addBreakpoint(dlg->getType(), dlg->getAddress()); });
  dlg->open();
}

void DebuggerWindow::onToggleBreakpointTriggered()
{
  std::optional<VirtualMemoryAddress> address = m_ui.codeView->getSelectedAddress();
  if (!address.has_value())
    return;

  toggleBreakpoint(address.value());
}

void DebuggerWindow::onClearBreakpointsTriggered()
{
  clearBreakpoints();
}

void DebuggerWindow::onBreakpointListContextMenuRequested()
{
  const QList<QTreeWidgetItem*> selected = m_ui.breakpointsWidget->selectedItems();
  if (selected.size() != 1)
    return;

  const QTreeWidgetItem* item = selected[0];
  const u32 address = item->data(1, Qt::UserRole).toUInt();
  const CPU::BreakpointType type = static_cast<CPU::BreakpointType>(item->data(2, Qt::UserRole).toUInt());

  QMenu* const menu = QtUtils::NewPopupMenu(this);
  menu->addAction(tr("&Remove"), [this, address, type]() { removeBreakpoint(type, address); });
  menu->popup(QCursor::pos());
}

void DebuggerWindow::onBreakpointListItemChanged(QTreeWidgetItem* item, int column)
{
  // checkbox
  if (column != 0)
    return;

  bool ok;
  const uint bp_addr = item->data(1, Qt::UserRole).toUInt(&ok);
  if (!ok)
    return;

  const uint bp_type = item->data(2, Qt::UserRole).toUInt(&ok);
  if (!ok)
    return;

  const bool enabled = (item->checkState(0) == Qt::Checked);

  Host::RunOnCoreThread([bp_addr, bp_type, enabled]() {
    CPU::SetBreakpointEnabled(static_cast<CPU::BreakpointType>(bp_type), bp_addr, enabled);
  });
}

void DebuggerWindow::onStepIntoActionTriggered()
{
  Assert(QtHost::IsSystemPaused());
  saveCurrentState();
  g_core_thread->singleStepCPU();
}

void DebuggerWindow::onStepOverActionTriggered()
{
  Assert(QtHost::IsSystemPaused());
  if (!CPU::AddStepOverBreakpoint())
  {
    onStepIntoActionTriggered();
    return;
  }

  // unpause to let it run to the breakpoint
  saveCurrentState();
  g_core_thread->setSystemPaused(false);
}

void DebuggerWindow::onStepOutActionTriggered()
{
  Assert(QtHost::IsSystemPaused());
  if (!CPU::AddStepOutBreakpoint())
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Debugger"),
                             tr("Failed to add step-out breakpoint, are you in a valid function?"));
    return;
  }

  // unpause to let it run to the breakpoint
  saveCurrentState();
  g_core_thread->setSystemPaused(false);
}

void DebuggerWindow::onCodeViewAddressActivated(VirtualMemoryAddress address)
{
  scrollToMemoryAddress(address);
}

void DebuggerWindow::onCodeViewToggleBreakpointActivated(VirtualMemoryAddress address)
{
  toggleBreakpoint(address);
}

void DebuggerWindow::onCodeViewCommentActivated(VirtualMemoryAddress address)
{
  if (!tryFollowLoadStore(address))
    toggleBreakpoint(address);
}

void DebuggerWindow::onCodeViewContextMenuRequested(const QPoint& pt)
{
  const VirtualMemoryAddress address = m_ui.codeView->getAddressAtPoint(pt);
  m_ui.codeView->setSelectedAddress(address);

  QMenu* const menu = QtUtils::NewPopupMenu(this);
  menu->addAction(QStringLiteral("0x%1").arg(static_cast<uint>(address), 8, 16, QChar('0')))->setEnabled(false);
  menu->addSeparator();

  menu->addAction(QIcon::fromTheme("debug-toggle-breakpoint"_L1), tr("Toggle &Breakpoint"),
                  [this, address]() { toggleBreakpoint(address); });

  menu->addAction(QIcon::fromTheme("debugger-go-to-cursor"_L1), tr("&Run To Cursor"), [address]() {
    Host::RunOnCoreThread([address]() {
      CPU::AddBreakpoint(CPU::BreakpointType::Execute, address, true, true);
      g_core_thread->setSystemPaused(false);
    });
  });

  menu->addSeparator();
  menu->addAction(QIcon::fromTheme("debugger-go-to-address"_L1), tr("View in &Dump"),
                  [this, address]() { scrollToMemoryAddress(address); });

  menu->addAction(QIcon::fromTheme("debug-trace-line"_L1), tr("&Follow Load/Store"),
                  [this, address]() { tryFollowLoadStore(address); });

  menu->popup(m_ui.codeView->mapToGlobal(pt));
}

void DebuggerWindow::onMemorySearchTriggered()
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
      m_ui.statusbar->showMessage(tr("Pattern not found."));
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
    m_ui.statusbar->showMessage(tr("Pattern found at 0x%1 (passed the end of memory).")
                                  .arg(static_cast<uint>(found_address.value()), 8, 16, static_cast<QChar>('0')));
  }
  else
  {
    m_ui.statusbar->showMessage(
      tr("Pattern found at 0x%1.").arg(static_cast<uint>(found_address.value()), 8, 16, static_cast<QChar>('0')));
  }
}

void DebuggerWindow::onMemorySearchStringChanged(const QString&)
{
  m_next_memory_search_address = 0;
}

void DebuggerWindow::closeEvent(QCloseEvent* event)
{
  QtUtils::SaveWindowGeometry(this);
  g_core_thread->disconnect(this);
  Host::RunOnCoreThread(&CPU::ClearBreakpoints);
  QMainWindow::closeEvent(event);
  emit closed();
}

void DebuggerWindow::setupAdditionalUi()
{
  const QFont& fixed_font = QtHost::GetFixedFont();
  m_ui.codeView->setFont(fixed_font);
  m_ui.codeView->updateRowHeight();
  m_ui.registerView->setFont(fixed_font);
  m_ui.memoryView->setFont(fixed_font);
  m_ui.stackView->setFont(fixed_font);

  m_ui.codeView->setContextMenuPolicy(Qt::CustomContextMenu);
  m_ui.breakpointsWidget->setContextMenuPolicy(Qt::CustomContextMenu);

  setCentralWidget(nullptr);
  delete m_ui.centralwidget;
}

void DebuggerWindow::connectSignals()
{
  connect(g_core_thread, &CoreThread::systemPaused, this, &DebuggerWindow::onSystemPaused);
  connect(g_core_thread, &CoreThread::systemResumed, this, &DebuggerWindow::onSystemResumed);
  connect(g_core_thread, &CoreThread::systemStarted, this, &DebuggerWindow::onSystemStarted);
  connect(g_core_thread, &CoreThread::systemDestroyed, this, &DebuggerWindow::onSystemDestroyed);

  connect(m_ui.actionPause, &QAction::triggered, this, &DebuggerWindow::onPauseActionTriggered);
  connect(m_ui.actionRunToCursor, &QAction::triggered, this, &DebuggerWindow::onRunToCursorTriggered);
  connect(m_ui.actionGoToPC, &QAction::triggered, this, &DebuggerWindow::onGoToPCTriggered);
  connect(m_ui.actionGoToAddress, &QAction::triggered, this, &DebuggerWindow::onGoToAddressTriggered);
  connect(m_ui.actionDumpAddress, &QAction::triggered, this, &DebuggerWindow::onDumpAddressTriggered);
  connect(m_ui.actionTrace, &QAction::triggered, this, &DebuggerWindow::onTraceTriggered);
  connect(m_ui.actionStepInto, &QAction::triggered, this, &DebuggerWindow::onStepIntoActionTriggered);
  connect(m_ui.actionStepOver, &QAction::triggered, this, &DebuggerWindow::onStepOverActionTriggered);
  connect(m_ui.actionStepOut, &QAction::triggered, this, &DebuggerWindow::onStepOutActionTriggered);
  connect(m_ui.actionAddBreakpoint, &QAction::triggered, this, &DebuggerWindow::onAddBreakpointTriggered);
  connect(m_ui.actionToggleBreakpoint, &QAction::triggered, this, &DebuggerWindow::onToggleBreakpointTriggered);
  connect(m_ui.actionClearBreakpoints, &QAction::triggered, this, &DebuggerWindow::onClearBreakpointsTriggered);
  connect(m_ui.actionClose, &QAction::triggered, this, &DebuggerWindow::close);
  connect(m_ui.codeView, &DebuggerCodeView::addressActivated, this, &DebuggerWindow::onCodeViewAddressActivated);
  connect(m_ui.codeView, &DebuggerCodeView::toggleBreakpointActivated, this,
          &DebuggerWindow::onCodeViewToggleBreakpointActivated);
  connect(m_ui.codeView, &DebuggerCodeView::commentActivated, this, &DebuggerWindow::onCodeViewCommentActivated);
  connect(m_ui.codeView, &QWidget::customContextMenuRequested, this, &DebuggerWindow::onCodeViewContextMenuRequested);
  connect(m_ui.breakpointsWidget, &QTreeWidget::customContextMenuRequested, this,
          &DebuggerWindow::onBreakpointListContextMenuRequested);
  connect(m_ui.breakpointsWidget, &QTreeWidget::itemChanged, this, &DebuggerWindow::onBreakpointListItemChanged);

  connect(m_ui.memoryRegionRAM, &QRadioButton::clicked, [this]() { setMemoryViewRegion(Bus::MemoryRegion::RAM); });
  connect(m_ui.memoryRegionEXP1, &QRadioButton::clicked, [this]() { setMemoryViewRegion(Bus::MemoryRegion::EXP1); });
  connect(m_ui.memoryRegionScratchpad, &QRadioButton::clicked,
          [this]() { setMemoryViewRegion(Bus::MemoryRegion::Scratchpad); });
  connect(m_ui.memoryRegionBIOS, &QRadioButton::clicked, [this]() { setMemoryViewRegion(Bus::MemoryRegion::BIOS); });

  connect(m_ui.memorySearch, &QPushButton::clicked, this, &DebuggerWindow::onMemorySearchTriggered);
  connect(m_ui.memorySearchString, &QLineEdit::textChanged, this, &DebuggerWindow::onMemorySearchStringChanged);

  connect(&m_refresh_timer, &QTimer::timeout, this, &DebuggerWindow::timerRefresh);
  m_refresh_timer.setInterval(TIMER_REFRESH_INTERVAL_MS);
}

void DebuggerWindow::createModels()
{
  m_registers_model = new DebuggerRegistersModel(this);
  m_ui.registerView->setModel(m_registers_model);
  // m_ui->registerView->resizeRowsToContents();

  m_stack_model = new DebuggerStackModel(this);
  m_ui.stackView->setModel(m_stack_model);

  m_ui.breakpointsWidget->setColumnWidth(0, 50);
  m_ui.breakpointsWidget->setColumnWidth(1, 80);
  m_ui.breakpointsWidget->setColumnWidth(2, 50);
  m_ui.breakpointsWidget->setColumnWidth(3, 40);
  m_ui.breakpointsWidget->setRootIsDecorated(false);
}

void DebuggerWindow::setUIEnabled(bool enabled, bool allow_pause)
{
  const bool read_only_views = (enabled || allow_pause);

  m_ui.actionPause->setEnabled(allow_pause);

  // Disable all UI elements that depend on execution state
  m_ui.codeView->setEnabled(read_only_views);
  m_ui.registerView->setEnabled(read_only_views);
  m_ui.stackView->setEnabled(read_only_views);
  m_ui.memoryView->setEnabled(read_only_views);
  m_ui.actionRunToCursor->setEnabled(enabled);
  m_ui.actionAddBreakpoint->setEnabled(enabled);
  m_ui.actionToggleBreakpoint->setEnabled(enabled);
  m_ui.actionClearBreakpoints->setEnabled(enabled);
  m_ui.actionDumpAddress->setEnabled(read_only_views);
  m_ui.actionStepInto->setEnabled(enabled);
  m_ui.actionStepOver->setEnabled(enabled);
  m_ui.actionStepOut->setEnabled(enabled);
  m_ui.actionGoToAddress->setEnabled(enabled);
  m_ui.actionGoToPC->setEnabled(enabled);
  m_ui.actionTrace->setEnabled(enabled);
  m_ui.memoryRegionRAM->setEnabled(read_only_views);
  m_ui.memoryRegionEXP1->setEnabled(read_only_views);
  m_ui.memoryRegionScratchpad->setEnabled(read_only_views);
  m_ui.memoryRegionBIOS->setEnabled(read_only_views);
  m_ui.memorySearch->setEnabled(read_only_views);
  m_ui.memorySearchString->setEnabled(read_only_views);

  // Partial/timer refreshes only active when not paused.
  const bool timer_active = (!enabled && allow_pause);
  if (m_refresh_timer.isActive() != timer_active)
    timer_active ? m_refresh_timer.start() : m_refresh_timer.stop();
}

void DebuggerWindow::saveCurrentState()
{
  m_registers_model->saveCurrentValues();
  m_ui.memoryView->saveCurrentData();
}

void DebuggerWindow::setMemoryViewRegion(Bus::MemoryRegion region)
{
  if (m_active_memory_region == region)
    return;

  m_active_memory_region = region;

  static constexpr auto edit_ram_callback = [](size_t offset, size_t count) {
    // shouldn't happen
    if (offset > Bus::g_ram_size)
      return;

    const u32 start_page = static_cast<u32>(offset) >> HOST_PAGE_SHIFT;
    const u32 end_page = static_cast<u32>(offset + count - 1) >> HOST_PAGE_SHIFT;
    Host::RunOnCoreThread([start_page, end_page]() {
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

  m_ui.memoryRegionRAM->setChecked(region == Bus::MemoryRegion::RAM);
  m_ui.memoryRegionEXP1->setChecked(region == Bus::MemoryRegion::EXP1);
  m_ui.memoryRegionScratchpad->setChecked(region == Bus::MemoryRegion::Scratchpad);
  m_ui.memoryRegionBIOS->setChecked(region == Bus::MemoryRegion::BIOS);

  m_ui.memoryView->repaint();
}

void DebuggerWindow::toggleBreakpoint(VirtualMemoryAddress address)
{
  Host::RunOnCoreThread([address]() {
    const bool new_bp_state = !CPU::HasBreakpointAtAddress(CPU::BreakpointType::Execute, address);
    if (new_bp_state)
    {
      if (!CPU::AddBreakpoint(CPU::BreakpointType::Execute, address, false))
        return;
    }
    else
    {
      if (!CPU::RemoveBreakpoint(CPU::BreakpointType::Execute, address))
        return;
    }

    Host::RunOnUIThread([bps = CPU::CopyBreakpointList()]() {
      DebuggerWindow* const win = g_main_window->getDebuggerWindow();
      if (!win)
        return;

      win->refreshBreakpointList(bps);
    });
  });
}

void DebuggerWindow::clearBreakpoints()
{
  m_ui.codeView->clearBreakpoints();
  Host::RunOnCoreThread(&CPU::ClearBreakpoints);
}

bool DebuggerWindow::tryFollowLoadStore(VirtualMemoryAddress address)
{
  CPU::Instruction inst;
  if (!CPU::SafeReadInstruction(address, &inst.bits))
    return false;

  const std::optional<VirtualMemoryAddress> ea = GetLoadStoreEffectiveAddress(inst, &CPU::g_state.regs);
  if (!ea.has_value())
    return false;

  scrollToMemoryAddress(ea.value());
  return true;
}

bool DebuggerWindow::scrollToMemoryAddress(VirtualMemoryAddress address)
{
  const PhysicalMemoryAddress phys_address = CPU::VirtualAddressToPhysical(address);
  std::optional<Bus::MemoryRegion> region = Bus::GetMemoryRegionForAddress(phys_address);
  if (!region.has_value())
    return false;

  setMemoryViewRegion(region.value());

  const PhysicalMemoryAddress offset = phys_address - Bus::GetMemoryRegionStart(region.value());
  m_ui.memoryView->scrollToOffset(offset);
  return true;
}

void DebuggerWindow::refreshBreakpointList()
{
  Host::RunOnCoreThread([]() {
    Host::RunOnUIThread([bps = CPU::CopyBreakpointList()]() {
      DebuggerWindow* const win = g_main_window->getDebuggerWindow();
      if (!win)
        return;

      win->refreshBreakpointList(bps);
    });
  });
}

void DebuggerWindow::refreshBreakpointList(const CPU::BreakpointList& bps)
{
  while (m_ui.breakpointsWidget->topLevelItemCount() > 0)
    delete m_ui.breakpointsWidget->takeTopLevelItem(0);

  for (const CPU::Breakpoint& bp : bps)
  {
    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, bp.enabled ? Qt::Checked : Qt::Unchecked);
    item->setText(0, QString::asprintf("%u", bp.number));
    item->setText(1, QString::asprintf("0x%08X", bp.address));
    item->setText(2, QString::fromUtf8(CPU::GetBreakpointTypeName(bp.type)));
    item->setText(3, QString::asprintf("%u", bp.hit_count));
    item->setData(0, Qt::UserRole, bp.number);
    item->setData(1, Qt::UserRole, QVariant(static_cast<uint>(bp.address)));
    item->setData(2, Qt::UserRole, QVariant(static_cast<uint>(bp.type)));
    m_ui.breakpointsWidget->addTopLevelItem(item);
  }

  m_ui.codeView->updateBreakpointList(bps);
}

void DebuggerWindow::addBreakpoint(CPU::BreakpointType type, u32 address)
{
  Host::RunOnCoreThread([address, type]() {
    const bool result = CPU::AddBreakpoint(type, address);
    Host::RunOnUIThread([bps = CPU::CopyBreakpointList(), result]() {
      DebuggerWindow* const win = g_main_window->getDebuggerWindow();
      if (!win)
        return;

      if (!result)
      {
        QtUtils::AsyncMessageBox(win, QMessageBox::Critical, win->windowTitle(),
                                 tr("Failed to add breakpoint. A breakpoint may already exist at this address."));
        return;
      }

      win->refreshBreakpointList(bps);
    });
  });
}

void DebuggerWindow::removeBreakpoint(CPU::BreakpointType type, u32 address)
{
  Host::RunOnCoreThread([address, type]() {
    const bool result = CPU::RemoveBreakpoint(type, address);
    Host::RunOnUIThread([bps = CPU::CopyBreakpointList(), result]() {
      DebuggerWindow* const win = g_main_window->getDebuggerWindow();
      if (!win)
        return;

      if (!result)
      {
        QtUtils::AsyncMessageBox(win, QMessageBox::Critical, win->windowTitle(),
                                 tr("Failed to remove breakpoint. This breakpoint may not exist."));
        return;
      }

      win->refreshBreakpointList(bps);
    });
  });
}

void DebuggerWindow::updateBreakpointHitCounts(const CPU::BreakpointList& bps)
{
  for (size_t i = 0; i < bps.size(); i++)
  {
    const CPU::Breakpoint& bp = bps[i];
    QTreeWidgetItem* const item = m_ui.breakpointsWidget->topLevelItem(static_cast<int>(i));
    if (!item)
      continue;

    item->setText(3, QString::asprintf("%u", bp.hit_count));
  }
}

void Host::ReportDebuggerEvent(CPU::DebuggerEvent event, std::string_view message)
{
  if (event == CPU::DebuggerEvent::Message)
  {
    if (!message.empty())
      return;

    INFO_LOG("Debugger message: {}", message);
    Host::RunOnUIThread([message = QtUtils::StringViewToQString(message)]() {
      DebuggerWindow* const win = g_main_window->getDebuggerWindow();
      if (!win)
        return;

      win->reportMessage(message);
    });
  }
  else if (event == CPU::DebuggerEvent::BreakpointHit)
  {
    Host::RunOnUIThread([bps = CPU::CopyBreakpointList(), message = QtUtils::StringViewToQString(message)]() {
      DebuggerWindow* const win = g_main_window->getDebuggerWindow();
      if (!win)
        return;

      win->updateBreakpointHitCounts(bps);

      if (!message.isEmpty())
        win->reportMessage(message);
    });
  }
}
