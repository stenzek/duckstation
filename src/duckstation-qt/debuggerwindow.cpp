#include "debuggerwindow.h"
#include "common/assert.h"
#include "core/cpu_core_private.h"
#include "debuggermodels.h"
#include "qthost.h"
#include "qtutils.h"
#include <QtCore/QSignalBlocker>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>

DebuggerWindow::DebuggerWindow(QWidget* parent /* = nullptr */)
  : QMainWindow(parent), m_active_memory_region(Bus::MemoryRegion::Count)
{
  m_ui.setupUi(this);
  setupAdditionalUi();
  connectSignals();
  createModels();
  setMemoryViewRegion(Bus::MemoryRegion::RAM);
  setUIEnabled(false);
}

DebuggerWindow::~DebuggerWindow() = default;

void DebuggerWindow::onEmulationPaused()
{
  setUIEnabled(true);
  refreshAll();
  refreshBreakpointList();

  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(true);
  }
}

void DebuggerWindow::onEmulationResumed()
{
  setUIEnabled(false);

  {
    QSignalBlocker sb(m_ui.actionPause);
    m_ui.actionPause->setChecked(false);
  }
}

void DebuggerWindow::onDebuggerMessageReported(const QString& message)
{
  m_ui.statusbar->showMessage(message, 0);
}

void DebuggerWindow::refreshAll()
{
  m_registers_model->invalidateView();
  m_stack_model->invalidateView();
  m_ui.memoryView->repaint();

  m_code_model->setPC(CPU::g_state.regs.pc);
  scrollToPC();
}

void DebuggerWindow::scrollToPC()
{
  return scrollToCodeAddress(CPU::g_state.regs.pc);
}

void DebuggerWindow::scrollToCodeAddress(VirtualMemoryAddress address)
{
  m_code_model->ensureAddressVisible(address);

  int row = m_code_model->getRowForAddress(address);
  if (row >= 0)
  {
    qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
    m_ui.codeView->scrollTo(m_code_model->index(row, 0));
  }
}

void DebuggerWindow::onPauseActionToggled(bool paused)
{
  if (!paused)
  {
    m_registers_model->saveCurrentValues();
    setUIEnabled(false);
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

  CPU::AddBreakpoint(addr.value(), true, true);
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
  std::optional<VirtualMemoryAddress> address =
    QtUtils::PromptForAddress(this, windowTitle(), tr("Enter code address:"), true);
  if (!address.has_value())
    return;

  if (CPU::HasBreakpointAtAddress(address.value()))
  {
    QMessageBox::critical(this, windowTitle(), tr("A breakpoint already exists at this address."));
    return;
  }

  toggleBreakpoint(address.value());
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

void DebuggerWindow::onStepIntoActionTriggered()
{
  Assert(System::IsPaused());
  m_registers_model->saveCurrentValues();
  g_emu_thread->singleStepCPU();
  refreshAll();
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
  m_registers_model->saveCurrentValues();
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
  m_registers_model->saveCurrentValues();
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
  QMainWindow::closeEvent(event);
  g_emu_thread->setSystemPaused(true, true);
  CPU::ClearBreakpoints();
  g_emu_thread->setSystemPaused(false);
  emit closed();
}

void DebuggerWindow::setupAdditionalUi()
{
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

  setCentralWidget(nullptr);
  delete m_ui.centralwidget;
}

void DebuggerWindow::connectSignals()
{
  EmuThread* hi = g_emu_thread;
  connect(hi, &EmuThread::systemPaused, this, &DebuggerWindow::onEmulationPaused);
  connect(hi, &EmuThread::systemResumed, this, &DebuggerWindow::onEmulationResumed);
  connect(hi, &EmuThread::debuggerMessageReported, this, &DebuggerWindow::onDebuggerMessageReported);

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

  connect(m_ui.memoryRegionRAM, &QRadioButton::clicked, [this]() { setMemoryViewRegion(Bus::MemoryRegion::RAM); });
  connect(m_ui.memoryRegionEXP1, &QRadioButton::clicked, [this]() { setMemoryViewRegion(Bus::MemoryRegion::EXP1); });
  connect(m_ui.memoryRegionScratchpad, &QRadioButton::clicked,
          [this]() { setMemoryViewRegion(Bus::MemoryRegion::Scratchpad); });
  connect(m_ui.memoryRegionBIOS, &QRadioButton::clicked, [this]() { setMemoryViewRegion(Bus::MemoryRegion::BIOS); });

  connect(m_ui.memorySearch, &QPushButton::clicked, this, &DebuggerWindow::onMemorySearchTriggered);
  connect(m_ui.memorySearchString, &QLineEdit::textChanged, this, &DebuggerWindow::onMemorySearchStringChanged);
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
  m_ui.breakpointsWidget->setColumnWidth(2, 40);
  m_ui.breakpointsWidget->setRootIsDecorated(false);
}

void DebuggerWindow::setUIEnabled(bool enabled)
{
  // Disable all UI elements that depend on execution state
  m_ui.codeView->setEnabled(enabled);
  m_ui.registerView->setEnabled(enabled);
  m_ui.stackView->setEnabled(enabled);
  m_ui.memoryView->setEnabled(enabled);
  m_ui.actionRunToCursor->setEnabled(enabled);
  m_ui.actionAddBreakpoint->setEnabled(enabled);
  m_ui.actionToggleBreakpoint->setEnabled(enabled);
  m_ui.actionClearBreakpoints->setEnabled(enabled);
  m_ui.actionDumpAddress->setEnabled(enabled);
  m_ui.actionStepInto->setEnabled(enabled);
  m_ui.actionStepOver->setEnabled(enabled);
  m_ui.actionStepOut->setEnabled(enabled);
  m_ui.actionGoToAddress->setEnabled(enabled);
  m_ui.actionGoToPC->setEnabled(enabled);
  m_ui.actionTrace->setEnabled(enabled);
  m_ui.memoryRegionRAM->setEnabled(enabled);
  m_ui.memoryRegionEXP1->setEnabled(enabled);
  m_ui.memoryRegionScratchpad->setEnabled(enabled);
  m_ui.memoryRegionBIOS->setEnabled(enabled);
}

void DebuggerWindow::setMemoryViewRegion(Bus::MemoryRegion region)
{
  if (m_active_memory_region == region)
    return;

  m_active_memory_region = region;

  const PhysicalMemoryAddress start = Bus::GetMemoryRegionStart(region);
  const PhysicalMemoryAddress end = Bus::GetMemoryRegionEnd(region);
  m_ui.memoryView->setData(start, Bus::GetMemoryRegionPointer(region), end - start);

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
  const bool new_bp_state = !CPU::HasBreakpointAtAddress(address);
  if (new_bp_state)
  {
    if (!CPU::AddBreakpoint(address, false))
      return;
  }
  else
  {
    if (!CPU::RemoveBreakpoint(address))
      return;
  }

  m_code_model->setBreakpointState(address, new_bp_state);
  refreshBreakpointList();
}

void DebuggerWindow::clearBreakpoints()
{
  m_code_model->clearBreakpoints();
  CPU::ClearBreakpoints();
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
  m_ui.memoryView->scrolltoOffset(offset);
  return true;
}

void DebuggerWindow::refreshBreakpointList()
{
  while (m_ui.breakpointsWidget->topLevelItemCount() > 0)
    delete m_ui.breakpointsWidget->takeTopLevelItem(0);

  const CPU::BreakpointList bps(CPU::GetBreakpointList());
  for (const CPU::Breakpoint& bp : bps)
  {
    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, bp.enabled ? Qt::Checked : Qt::Unchecked);
    item->setText(0, QString::asprintf("%u", bp.number));
    item->setText(1, QString::asprintf("0x%08X", bp.address));
    item->setText(2, QString::asprintf("%u", bp.hit_count));
    m_ui.breakpointsWidget->addTopLevelItem(item);
  }
}
