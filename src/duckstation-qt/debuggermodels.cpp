// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "debuggermodels.h"
#include "qtutils.h"

#include "core/bios_types.h"
#include "core/cpu_call_stack.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/cpu_disasm.h"

#include "common/small_string.h"

#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>

#include <algorithm>

#include "moc_debuggermodels.cpp"

using namespace Qt::StringLiterals;

static constexpr u32 STACK_RANGE = 128;
static constexpr u32 STACK_VALUE_SIZE = sizeof(u32);
static constexpr u32 STACK_RANGE_BYTES = STACK_RANGE * STACK_VALUE_SIZE;

DebuggerRegistersModel::DebuggerRegistersModel(QObject* parent /*= nullptr*/) : QAbstractListModel(parent)
{
}

DebuggerRegistersModel::~DebuggerRegistersModel()
{
}

int DebuggerRegistersModel::rowCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return static_cast<int>(CPU::NUM_DEBUGGER_REGISTER_LIST_ENTRIES);
}

int DebuggerRegistersModel::columnCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return 2;
}

QVariant DebuggerRegistersModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  u32 reg_index = static_cast<u32>(index.row());
  if (reg_index >= CPU::NUM_DEBUGGER_REGISTER_LIST_ENTRIES)
    return QVariant();

  if (index.column() < 0 || index.column() > 1)
    return QVariant();

  switch (index.column())
  {
    case 0: // address
    {
      if (role == Qt::DisplayRole)
        return QString::fromUtf8(CPU::g_debugger_register_list[reg_index].name);
    }
    break;

    case 1: // data
    {
      if (role == Qt::DisplayRole)
      {
        return QString::asprintf("0x%08X", m_reg_values[reg_index]);
      }
      else if (role == Qt::ForegroundRole)
      {
        if (m_reg_values[reg_index] != m_old_reg_values[reg_index])
          return QColor(255, 50, 50);
      }
    }
    break;

    default:
      break;
  }

  return QVariant();
}

QVariant DebuggerRegistersModel::headerData(int section, Qt::Orientation orientation,
                                            int role /*= Qt::DisplayRole*/) const
{
  if (orientation != Qt::Horizontal)
    return QVariant();

  if (role != Qt::DisplayRole)
    return QVariant();

  switch (section)
  {
    case 0:
      return tr("Register");
    case 1:
      return tr("Value");
    default:
      return QVariant();
  }
}

void DebuggerRegistersModel::updateValues()
{
  beginResetModel();

  for (u32 i = 0; i < CPU::NUM_DEBUGGER_REGISTER_LIST_ENTRIES; i++)
    m_reg_values[i] = *CPU::g_debugger_register_list[i].value_ptr;

  endResetModel();
}

void DebuggerRegistersModel::saveCurrentValues()
{
  m_old_reg_values = m_reg_values;
}

DebuggerStackModel::DebuggerStackModel(QObject* parent /*= nullptr*/) : QAbstractListModel(parent)
{
}

DebuggerStackModel::~DebuggerStackModel()
{
}

int DebuggerStackModel::rowCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return static_cast<int>(STACK_RANGE * 2);
}

int DebuggerStackModel::columnCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return 2;
}

QVariant DebuggerStackModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  if (index.column() < 0 || index.column() > 1)
    return {};

  if (role != Qt::DisplayRole)
    return {};

  const std::optional<VirtualMemoryAddress> address = getAddressForIndex(index);
  if (!address.has_value())
    return {};

  if (index.column() == 0)
    return QString::asprintf("0x%08X", address.value());

  u32 value;
  if (!CPU::SafeReadMemoryWord(address.value(), &value))
    return tr("<invalid>");

  return QString::asprintf("0x%08X", ZeroExtend32(value));
}

QVariant DebuggerStackModel::headerData(int section, Qt::Orientation orientation, int role /*= Qt::DisplayRole*/) const
{
  if (orientation != Qt::Horizontal)
    return QVariant();

  if (role != Qt::DisplayRole)
    return QVariant();

  switch (section)
  {
    case 0:
      return tr("Address");
    case 1:
      return tr("Value");
    default:
      return QVariant();
  }
}

QModelIndex DebuggerStackModel::getIndexForAddress(VirtualMemoryAddress address) const
{
  const u32 sp = CPU::g_state.regs.sp;
  const u32 start = sp - std::min(sp, STACK_RANGE_BYTES);
  if (address < start || address >= (sp + std::min(STACK_RANGE_BYTES, std::numeric_limits<u32>::max() - sp)))
    return QModelIndex();

  return index(static_cast<int>((address - start) / STACK_VALUE_SIZE));
}

std::optional<VirtualMemoryAddress> DebuggerStackModel::getAddressForIndex(const QModelIndex& index) const
{
  if (!index.isValid())
    return std::nullopt;

  const u32 sp = CPU::g_state.regs.sp;
  const u32 start = sp - std::min(sp, STACK_RANGE_BYTES);
  return start + static_cast<u32>(index.row()) * STACK_VALUE_SIZE;
}

void DebuggerStackModel::invalidateView()
{
  beginResetModel();
  endResetModel();
}

DebuggerCallStackModel::DebuggerCallStackModel(QObject* parent /*= nullptr*/) : QAbstractTableModel(parent)
{
}

DebuggerCallStackModel::~DebuggerCallStackModel() = default;

int DebuggerCallStackModel::rowCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return parent.isValid() ? 0 : static_cast<int>(m_frames.size());
}

int DebuggerCallStackModel::columnCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return 2;
}

QVariant DebuggerCallStackModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  if (!index.isValid() || role != Qt::DisplayRole || index.row() < 0 ||
      static_cast<size_t>(index.row()) >= m_frames.size())
  {
    return {};
  }

  const CPU::CallStack::Frame& frame = m_frames[static_cast<size_t>(index.row())];
  switch (index.column())
  {
    case 0:
      return QString::asprintf("0x%08X", frame.address);
    case 1:
      return QString::asprintf("0x%08X", frame.stack_pointer);
    default:
      return {};
  }
}

QVariant DebuggerCallStackModel::headerData(int section, Qt::Orientation orientation,
                                            int role /*= Qt::DisplayRole*/) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return {};

  switch (section)
  {
    case 0:
      return tr("Address");
    case 1:
      return tr("Frame");
    default:
      return {};
  }
}

void DebuggerCallStackModel::updateValues()
{
  beginResetModel();

  CPU::CallStack call_stack(CPU::g_state.pc, CPU::g_state.regs.sp, CPU::g_state.regs.ra);
  call_stack.Walk();

  m_frames = call_stack.TakeFrames();

  endResetModel();
}

void DebuggerCallStackModel::clear()
{
  beginResetModel();
  m_frames.clear();
  endResetModel();
}

const CPU::CallStack::Frame* DebuggerCallStackModel::getFrame(const QModelIndex& index) const
{
  if (!index.isValid() || index.row() < 0 || static_cast<size_t>(index.row()) >= m_frames.size())
    return nullptr;

  return &m_frames[static_cast<size_t>(index.row())];
}

DebuggerThreadsModel::DebuggerThreadsModel(QObject* parent /*= nullptr*/) : QAbstractItemModel(parent)
{
}

DebuggerThreadsModel::~DebuggerThreadsModel() = default;

QModelIndex DebuggerThreadsModel::index(int row, int column, const QModelIndex& parent) const
{
  if (row < 0 || column < 0 || column >= columnCount(parent))
    return {};

  if (!parent.isValid())
  {
    if (static_cast<size_t>(row) >= m_threads.size())
      return {};

    return createIndex(row, column, quintptr(0));
  }

  if (parent.internalId() != 0 || parent.column() != 0 || row >= static_cast<int>(NUM_REGISTER_VALUES))
    return {};

  return createIndex(row, column, static_cast<quintptr>(parent.row() + 1));
}

QModelIndex DebuggerThreadsModel::parent(const QModelIndex& child) const
{
  if (!child.isValid() || child.internalId() == 0)
    return {};

  const int thread_row = static_cast<int>(child.internalId() - 1);
  if (thread_row < 0 || static_cast<size_t>(thread_row) >= m_threads.size())
    return {};

  return createIndex(thread_row, 0, quintptr(0));
}

int DebuggerThreadsModel::rowCount(const QModelIndex& parent) const
{
  if (!parent.isValid())
    return static_cast<int>(m_threads.size());

  return (parent.internalId() == 0 && parent.column() == 0) ? static_cast<int>(NUM_REGISTER_VALUES) : 0;
}

int DebuggerThreadsModel::columnCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return 2;
}

QVariant DebuggerThreadsModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  if (!index.isValid() || (role != Qt::DisplayRole && role != Qt::DecorationRole) || index.column() < 0 ||
      index.column() >= 2)
  {
    return {};
  }

  if (index.internalId() == 0)
  {
    if (static_cast<size_t>(index.row()) >= m_threads.size())
      return {};

    const Thread& thread = m_threads[static_cast<size_t>(index.row())];

    if (role == Qt::DecorationRole)
    {
      if (index.column() == 0 && thread.current)
        return QIcon(u":/icons/debug-pc.png"_s).pixmap(12);
      else
        return {};
    }
    else
    {
      if (index.column() == 0)
        return QString::asprintf("0x%08X", thread.handle);
      else
        return QString::asprintf("0x%08X", thread.pc);
    }
  }

  const size_t thread_index = static_cast<size_t>(index.internalId() - 1);
  if (role != Qt::DisplayRole || thread_index >= m_threads.size() || index.row() < 0 ||
      index.row() >= static_cast<int>(NUM_REGISTER_VALUES))
  {
    return {};
  }

  if (index.column() == 0)
  {
    if (index.row() < 32)
      return QString::fromLatin1(CPU::GetRegName(static_cast<CPU::Reg>(index.row())));

    static constexpr const char* register_names[] = {"hi", "lo", "SR", "CAUSE"};
    return QString::fromLatin1(register_names[index.row() - 32]);
  }

  return QString::asprintf("0x%08X", m_threads[thread_index].registers[static_cast<size_t>(index.row())]);
}

QVariant DebuggerThreadsModel::headerData(int section, Qt::Orientation orientation,
                                          int role /*= Qt::DisplayRole*/) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return {};

  switch (section)
  {
    case 0:
      return tr("Name");
    case 1:
      return tr("Value");
    default:
      return {};
  }
}

void DebuggerThreadsModel::updateValues()
{
  beginResetModel();
  m_threads.clear();

  BIOS::ControlBlockTableEntry pcb_table;
  BIOS::ControlBlockTableEntry tcb_table;
  if (!CPU::SafeReadMemoryBytes(BIOS::PCB_TABLE_ADDRESS, &pcb_table, sizeof(pcb_table)) ||
      !CPU::SafeReadMemoryBytes(BIOS::TCB_TABLE_ADDRESS, &tcb_table, sizeof(tcb_table)) ||
      pcb_table.size < sizeof(BIOS::ProcessControlBlock) || pcb_table.size > BIOS::KERNEL_CONTROL_BLOCK_MEMORY_SIZE ||
      tcb_table.size == 0 || tcb_table.size > BIOS::KERNEL_CONTROL_BLOCK_MEMORY_SIZE ||
      (tcb_table.size % sizeof(BIOS::ThreadControlBlock)) != 0)
  {
    endResetModel();
    return;
  }

  BIOS::ProcessControlBlock pcb;
  if (!CPU::SafeReadMemoryBytes(pcb_table.address, &pcb, sizeof(pcb)))
  {
    endResetModel();
    return;
  }

  const u32 num_tcbs = tcb_table.size / sizeof(BIOS::ThreadControlBlock);
  if (num_tcbs > BIOS::MAX_THREAD_CONTROL_BLOCKS)
  {
    endResetModel();
    return;
  }

  m_threads.reserve(num_tcbs);
  bool valid = true;
  for (u32 i = 0; i < num_tcbs; i++)
  {
    const u64 tcb_address_64 =
      static_cast<u64>(tcb_table.address) + (static_cast<u64>(i) * sizeof(BIOS::ThreadControlBlock));
    if (tcb_address_64 > UINT32_MAX)
    {
      valid = false;
      break;
    }

    const VirtualMemoryAddress tcb_address = static_cast<VirtualMemoryAddress>(tcb_address_64);
    BIOS::ThreadControlBlock tcb;
    if (!CPU::SafeReadMemoryBytes(tcb_address, &tcb, sizeof(tcb)))
    {
      valid = false;
      break;
    }

    if (tcb.status != BIOS::ThreadStatus::Used)
      continue;

    Thread& thread = m_threads.emplace_back();
    thread.index = i;
    thread.handle = BIOS::THREAD_HANDLE_BASE | i;
    thread.current = (CPU::VirtualAddressToPhysical(tcb_address) == CPU::VirtualAddressToPhysical(pcb.current_thread));

    if (thread.current)
    {
      std::copy_n(CPU::g_state.regs.r, 32, thread.registers.begin());
      thread.pc = CPU::g_state.pc;
      thread.registers[32] = CPU::g_state.regs.hi;
      thread.registers[33] = CPU::g_state.regs.lo;
      thread.registers[34] = CPU::g_state.cop0_regs.sr.bits;
      thread.registers[35] = CPU::g_state.cop0_regs.cause.bits;
    }
    else
    {
      std::copy(tcb.regs.begin(), tcb.regs.end(), thread.registers.begin());
      thread.pc = tcb.epc;
      thread.registers[32] = tcb.hi;
      thread.registers[33] = tcb.lo;
      thread.registers[34] = tcb.sr;
      thread.registers[35] = tcb.cause;
    }
  }

  if (!valid)
    m_threads.clear();

  endResetModel();
}

void DebuggerThreadsModel::clear()
{
  beginResetModel();
  m_threads.clear();
  endResetModel();
}

std::optional<VirtualMemoryAddress> DebuggerThreadsModel::getThreadPC(const QModelIndex& index) const
{
  if (!index.isValid() || index.internalId() != 0 || static_cast<size_t>(index.row()) >= m_threads.size())
    return std::nullopt;

  return m_threads[static_cast<size_t>(index.row())].pc;
}

DebuggerAddBreakpointDialog::DebuggerAddBreakpointDialog(QWidget* parent /*= nullptr*/) : QDialog(parent)
{
  m_ui.setupUi(this);
  connect(m_ui.buttonBox, &QDialogButtonBox::accepted, this, &DebuggerAddBreakpointDialog::okClicked);
}

DebuggerAddBreakpointDialog::~DebuggerAddBreakpointDialog() = default;

void DebuggerAddBreakpointDialog::okClicked()
{
  const QString address_str = m_ui.address->text();
  m_address = 0;
  bool ok = false;

  if (!address_str.isEmpty())
  {
    if (address_str.startsWith("0x"))
      m_address = address_str.mid(2).toUInt(&ok, 16);
    else
      m_address = address_str.toUInt(&ok, 16);

    if (!ok)
    {
      QtUtils::AsyncMessageBox(
        this, QMessageBox::Critical, windowTitle(),
        QCoreApplication::translate("DebuggerWindow", "Invalid address. It should be in hex (0x12345678 or 12345678)"));
      return;
    }

    if (m_ui.read->isChecked())
      m_type = CPU::BreakpointType::Read;
    else if (m_ui.write->isChecked())
      m_type = CPU::BreakpointType::Write;
    else
      m_type = CPU::BreakpointType::Execute;

    accept();
  }
}
