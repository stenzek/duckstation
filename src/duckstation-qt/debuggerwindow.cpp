// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: PolyForm-Strict-1.0.0

#include "debuggerwindow.h"
#include "debuggermodels.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/bus.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core_private.h"

#include "common/assert.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QCursor>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>

static constexpr int TIMER_REFRESH_INTERVAL_MS = 100;

DebuggerWindow::DebuggerWindow(QWidget* parent /* = nullptr */)
  : QMainWindow(parent), m_active_memory_region(Bus::MemoryRegion::Count)
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();
  createModels();
  setMemoryViewRegion(Bus::MemoryRegion::RAM);
  setUIEnabled(QtHost::IsSystemPaused(), QtHost::IsSystemValid());
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

  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(true);
  }
}

void DebuggerWindow::onSystemResumed()
{
  setUIEnabled(false, true);

  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(false);
  }
}

void DebuggerWindow::onDebuggerMessageReported(const QString& message)
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

  m_code_model->setPC(CPU::g_state.pc);
  scrollToPC();
}

void DebuggerWindow::scrollToPC()
{
  return scrollToCodeAddress(CPU::g_state.pc);
}

void DebuggerWindow::scrollToCodeAddress(VirtualMemoryAddress address)
{
  m_code_model->ensureAddressVisible(address);

  const int row = m_code_model->getRowForAddress(address);
  if (row >= 0)
  {
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);

    const QModelIndex index = m_code_model->index(row, 0);
    m_ui.codeView->scrollTo(index, QAbstractItemView::PositionAtCenter);
    m_ui.codeView->selectionModel()->setCurrentIndex(index,
                                                     QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  }
}

void DebuggerWindow::onPauseActionToggled(bool paused)
{
  if (!paused)
  {
    saveCurrentState();
    setUIEnabled(false, true);
  }

  g_emu_thread->setSystemPaused(paused);
}

void DebuggerWindow::onRunToCursorTriggered()
{
  std::optional<VirtualMemoryAddress> addr = getSelectedCodeAddress();
  if (!addr.has_value())
  {
    QMessageBox::critical(this, windowTitle(), tr("No address selected."));
    return;
  }

  CPU::AddBreakpoint(CPU::BreakpointType::Execute, addr.value(), true, true);
  g_emu_thread->setSystemPaused(false);
}

void DebuggerWindow::onGoToPCTriggered()
{
  scrollToPC();
}

void DebuggerWindow::onGoToAddressTriggered()
{
  std::optional<VirtualMemoryAddress> address =
    QtUtils::PromptForAddress(this, windowTitle(), tr("Enter code address:"), true);
  if (!address.has_value())
    return;

  scrollToCodeAddress(address.value());
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
  if (!CPU::IsTraceEnabled())
  {
    QMessageBox::critical(
      this, windowTitle(),
      tr("Trace logging started to cpu_log.txt.\nThis file can be several gigabytes, so be aware of SSD wear."));
    CPU::StartTrace();
  }
  else
  {
    CPU::StopTrace();
    QMessageBox::critical(this, windowTitle(), tr("Trace logging to cpu_log.txt stopped."));
  }
}

void DebuggerWindow::onFollowAddressTriggered()
{
  //
}

void DebuggerWindow::onAddBreakpointTriggered()
{
  DebuggerAddBreakpointDialog dlg(this);
  if (!dlg.exec())
    return;

  addBreakpoint(dlg.getType(), dlg.getAddress());
}

void DebuggerWindow::onToggleBreakpointTriggered()
{
  std::optional<VirtualMemoryAddress> address = getSelectedCodeAddress();
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

  QMenu menu(this);
  connect(menu.addAction(tr("&Remove")), &QAction::triggered, this,
          [this, address, type]() { removeBreakpoint(type, address); });
  menu.exec(QCursor::pos());
}

void DebuggerWindow::onStepIntoActionTriggered()
{
  Assert(System::IsPaused());
  saveCurrentState();
  g_emu_thread->singleStepCPU();
}

void DebuggerWindow::onStepOverActionTriggered()
{
  Assert(System::IsPaused());
  if (!CPU::AddStepOverBreakpoint())
  {
    onStepIntoActionTriggered();
    return;
  }

  // unpause to let it run to the breakpoint
  saveCurrentState();
  g_emu_thread->setSystemPaused(false);
}

void DebuggerWindow::onStepOutActionTriggered()
{
  Assert(System::IsPaused());
  if (!CPU::AddStepOutBreakpoint())
  {
    QMessageBox::critical(this, tr("Debugger"), tr("Failed to add step-out breakpoint, are you in a valid function?"));
    return;
  }

  // unpause to let it run to the breakpoint
  saveCurrentState();
  g_emu_thread->setSystemPaused(false);
}

void DebuggerWindow::onCodeViewItemActivated(QModelIndex index)
{
  if (!index.isValid())
    return;

  const VirtualMemoryAddress address = m_code_model->getAddressForIndex(index);
  switch (index.column())
  {
    case 0: // breakpoint
    case 3: // disassembly
      toggleBreakpoint(address);
      break;

    case 1: // address
    case 2: // bytes
      scrollToMemoryAddress(address);
      break;

    case 4: // comment
      tryFollowLoadStore(address);
      break;
  }
}

void DebuggerWindow::onCodeViewContextMenuRequested(const QPoint& pt)
{
  const QModelIndex index = m_ui.codeView->indexAt(pt);
  if (!index.isValid())
    return;

  const VirtualMemoryAddress address = m_code_model->getAddressForIndex(index);

  QMenu menu;
  menu.addAction(QStringLiteral("0x%1").arg(static_cast<uint>(address), 8, 16, QChar('0')))->setEnabled(false);
  menu.addSeparator();

  QAction* action = menu.addAction(QIcon::fromTheme("debug-toggle-breakpoint"), tr("Toggle &Breakpoint"));
  connect(action, &QAction::triggered, this, [this, address]() { toggleBreakpoint(address); });

  action = menu.addAction(QIcon::fromTheme("debugger-go-to-cursor"), tr("&Run To Cursor"));
  connect(action, &QAction::triggered, this, [address]() {
    Host::RunOnCPUThread([address]() {
      CPU::AddBreakpoint(CPU::BreakpointType::Execute, address, true, true);
      g_emu_thread->setSystemPaused(false);
    });
  });

  menu.addSeparator();
  action = menu.addAction(QIcon::fromTheme("debugger-go-to-address"), tr("View in &Dump"));
  connect(action, &QAction::triggered, this, [this, address]() { scrollToMemoryAddress(address); });

  action = menu.addAction(QIcon::fromTheme("debug-trace-line"), tr("&Follow Load/Store"));
  connect(action, &QAction::triggered, this, [this, address]() { tryFollowLoadStore(address); });

  menu.exec(m_ui.codeView->mapToGlobal(pt));
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
      QMessageBox::critical(this, windowTitle(),
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
    QMessageBox::critical(this, windowTitle(),
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
  QtUtils::SaveWindowGeometry("DebuggerWindow", this);
  g_emu_thread->disconnect(this);
  Host::RunOnCPUThread(&CPU::ClearBreakpoints);
  QMainWindow::closeEvent(event);
  emit closed();
}

void DebuggerWindow::setupAdditionalUi()
{
  setWindowIcon(QtHost::GetAppIcon());

#ifdef _WIN32
  QFont fixedFont;
  fixedFont.setFamily(QStringLiteral("Consolas"));
  fixedFont.setFixedPitch(true);
  fixedFont.setStyleHint(QFont::TypeWriter);
  fixedFont.setPointSize(10);
#else
  const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
#endif
  m_ui.codeView->setFont(fixedFont);
  m_ui.registerView->setFont(fixedFont);
  m_ui.memoryView->setFont(fixedFont);
  m_ui.stackView->setFont(fixedFont);

  m_ui.codeView->setContextMenuPolicy(Qt::CustomContextMenu);
  m_ui.breakpointsWidget->setContextMenuPolicy(Qt::CustomContextMenu);

  setCentralWidget(nullptr);
  delete m_ui.centralwidget;

  QtUtils::RestoreWindowGeometry("DebuggerWindow", this);
}

void DebuggerWindow::connectSignals()
{
  connect(g_emu_thread, &EmuThread::systemPaused, this, &DebuggerWindow::onSystemPaused);
  connect(g_emu_thread, &EmuThread::systemResumed, this, &DebuggerWindow::onSystemResumed);
  connect(g_emu_thread, &EmuThread::systemStarted, this, &DebuggerWindow::onSystemStarted);
  connect(g_emu_thread, &EmuThread::systemDestroyed, this, &DebuggerWindow::onSystemDestroyed);
  connect(g_emu_thread, &EmuThread::debuggerMessageReported, this, &DebuggerWindow::onDebuggerMessageReported);

  connect(m_ui.actionPause, &QAction::toggled, this, &DebuggerWindow::onPauseActionToggled);
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
  connect(m_ui.codeView, &QTreeView::activated, this, &DebuggerWindow::onCodeViewItemActivated);
  connect(m_ui.codeView, &QTreeView::customContextMenuRequested, this, &DebuggerWindow::onCodeViewContextMenuRequested);
  connect(m_ui.breakpointsWidget, &QTreeWidget::customContextMenuRequested, this,
          &DebuggerWindow::onBreakpointListContextMenuRequested);

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

void DebuggerWindow::disconnectSignals()
{
  EmuThread* hi = g_emu_thread;
  hi->disconnect(this);
}

void DebuggerWindow::createModels()
{
  m_code_model = std::make_unique<DebuggerCodeModel>();
  m_ui.codeView->setModel(m_code_model.get());

  // set default column width in code view
  m_ui.codeView->setColumnWidth(0, 40);
  m_ui.codeView->setColumnWidth(1, 80);
  m_ui.codeView->setColumnWidth(2, 80);
  m_ui.codeView->setColumnWidth(3, 250);
  m_ui.codeView->setColumnWidth(4, m_ui.codeView->width() - (40 + 80 + 80 + 250));

  m_registers_model = std::make_unique<DebuggerRegistersModel>();
  m_ui.registerView->setModel(m_registers_model.get());
  // m_ui->registerView->resizeRowsToContents();

  m_stack_model = std::make_unique<DebuggerStackModel>();
  m_ui.stackView->setModel(m_stack_model.get());

  m_ui.breakpointsWidget->setColumnWidth(0, 50);
  m_ui.breakpointsWidget->setColumnWidth(1, 80);
  m_ui.breakpointsWidget->setColumnWidth(2, 50);
  m_ui.breakpointsWidget->setColumnWidth(3, 40);
  m_ui.breakpointsWidget->setRootIsDecorated(false);
}

void DebuggerWindow::setUIEnabled(bool enabled, bool allow_pause)
{
  const bool memory_view_enabled = (enabled || allow_pause);

  m_ui.actionPause->setEnabled(allow_pause);

  // Disable all UI elements that depend on execution state
  m_ui.codeView->setEnabled(enabled);
  m_ui.registerView->setEnabled(enabled);
  m_ui.stackView->setEnabled(enabled);
  m_ui.memoryView->setEnabled(memory_view_enabled);
  m_ui.actionRunToCursor->setEnabled(enabled);
  m_ui.actionAddBreakpoint->setEnabled(enabled);
  m_ui.actionToggleBreakpoint->setEnabled(enabled);
  m_ui.actionClearBreakpoints->setEnabled(enabled);
  m_ui.actionDumpAddress->setEnabled(memory_view_enabled);
  m_ui.actionStepInto->setEnabled(enabled);
  m_ui.actionStepOver->setEnabled(enabled);
  m_ui.actionStepOut->setEnabled(enabled);
  m_ui.actionGoToAddress->setEnabled(enabled);
  m_ui.actionGoToPC->setEnabled(enabled);
  m_ui.actionTrace->setEnabled(enabled);
  m_ui.memoryRegionRAM->setEnabled(memory_view_enabled);
  m_ui.memoryRegionEXP1->setEnabled(memory_view_enabled);
  m_ui.memoryRegionScratchpad->setEnabled(memory_view_enabled);
  m_ui.memoryRegionBIOS->setEnabled(memory_view_enabled);

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

    const u32 start_page = static_cast<u32>(offset) / HOST_PAGE_SIZE;
    const u32 end_page = static_cast<u32>(offset + count - 1) / HOST_PAGE_SIZE;
    for (u32 i = start_page; i <= end_page; i++)
    {
      if (Bus::g_ram_code_bits[i])
        CPU::CodeCache::InvalidateBlocksWithPageIndex(i);
    }
  };

  const PhysicalMemoryAddress start = Bus::GetMemoryRegionStart(region);
  const PhysicalMemoryAddress end = Bus::GetMemoryRegionEnd(region);
  void* const mem_ptr = Bus::GetMemoryRegionPointer(region);
  const bool mem_writable = Bus::IsMemoryRegionWritable(region);
  const MemoryViewWidget::EditCallback edit_callback =
    ((region == Bus::MemoryRegion::RAM) ? static_cast<MemoryViewWidget::EditCallback>(edit_ram_callback) : nullptr);
  m_ui.memoryView->setData(start, mem_ptr, end - start, mem_writable, edit_callback);

#define SET_REGION_RADIO_BUTTON(name, rb_region)                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    QSignalBlocker sb(name);                                                                                           \
    name->setChecked(region == rb_region);                                                                             \
  } while (0)

  SET_REGION_RADIO_BUTTON(m_ui.memoryRegionRAM, Bus::MemoryRegion::RAM);
  SET_REGION_RADIO_BUTTON(m_ui.memoryRegionEXP1, Bus::MemoryRegion::EXP1);
  SET_REGION_RADIO_BUTTON(m_ui.memoryRegionScratchpad, Bus::MemoryRegion::Scratchpad);
  SET_REGION_RADIO_BUTTON(m_ui.memoryRegionBIOS, Bus::MemoryRegion::BIOS);

#undef SET_REGION_REGION_BUTTON

  m_ui.memoryView->repaint();
}

void DebuggerWindow::toggleBreakpoint(VirtualMemoryAddress address)
{
  Host::RunOnCPUThread([this, address]() {
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

    QtHost::RunOnUIThread([this, address, new_bp_state, bps = CPU::CopyBreakpointList()]() {
      m_code_model->setBreakpointState(address, new_bp_state);
      refreshBreakpointList(bps);
    });
  });
}

void DebuggerWindow::clearBreakpoints()
{
  m_code_model->clearBreakpoints();
  Host::RunOnCPUThread(&CPU::ClearBreakpoints);
}

std::optional<VirtualMemoryAddress> DebuggerWindow::getSelectedCodeAddress()
{
  QItemSelectionModel* sel_model = m_ui.codeView->selectionModel();
  const QModelIndexList indices(sel_model->selectedIndexes());
  if (indices.empty())
    return std::nullopt;

  return m_code_model->getAddressForIndex(indices[0]);
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
  Host::RunOnCPUThread(
    [this]() { QtHost::RunOnUIThread([this, bps = CPU::CopyBreakpointList()]() { refreshBreakpointList(bps); }); });
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
    item->setData(1, Qt::UserRole, bp.address);
    item->setData(2, Qt::UserRole, static_cast<u32>(bp.type));
    m_ui.breakpointsWidget->addTopLevelItem(item);
  }
}

void DebuggerWindow::addBreakpoint(CPU::BreakpointType type, u32 address)
{
  Host::RunOnCPUThread([this, address, type]() {
    const bool result = CPU::AddBreakpoint(type, address);
    QtHost::RunOnUIThread([this, address, type, result, bps = CPU::CopyBreakpointList()]() {
      if (!result)
      {
        QMessageBox::critical(this, windowTitle(),
                              tr("Failed to add breakpoint. A breakpoint may already exist at this address."));
        return;
      }

      if (type == CPU::BreakpointType::Execute)
        m_code_model->setBreakpointState(address, true);

      refreshBreakpointList(bps);
    });
  });
}

void DebuggerWindow::removeBreakpoint(CPU::BreakpointType type, u32 address)
{
  Host::RunOnCPUThread([this, address, type]() {
    const bool result = CPU::RemoveBreakpoint(type, address);
    QtHost::RunOnUIThread([this, address, type, result, bps = CPU::CopyBreakpointList()]() {
      if (!result)
      {
        QMessageBox::critical(this, windowTitle(), tr("Failed to remove breakpoint. This breakpoint may not exist."));
        return;
      }

      if (type == CPU::BreakpointType::Execute)
        m_code_model->setBreakpointState(address, false);

      refreshBreakpointList(bps);
    });
  });
}
