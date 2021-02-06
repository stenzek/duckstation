#include "debuggermodels.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/cpu_disasm.h"
#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>

static constexpr int NUM_COLUMNS = 5;
static constexpr int STACK_RANGE = 128;
static constexpr u32 STACK_VALUE_SIZE = sizeof(u32);

DebuggerCodeModel::DebuggerCodeModel(QObject* parent /*= nullptr*/) : QAbstractTableModel(parent)
{
  resetCodeView(0);
  m_pc_pixmap = QIcon(QStringLiteral(":/icons/debug-pc.png")).pixmap(QSize(12, 12));
  m_breakpoint_pixmap = QIcon(QStringLiteral(":/icons/media-record.png")).pixmap(QSize(12, 12));
}

DebuggerCodeModel::~DebuggerCodeModel() {}

int DebuggerCodeModel::rowCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return static_cast<int>((m_code_region_end - m_code_region_start) / CPU::INSTRUCTION_SIZE);
}

int DebuggerCodeModel::columnCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return NUM_COLUMNS;
}

int DebuggerCodeModel::getRowForAddress(VirtualMemoryAddress address) const
{
  return static_cast<int>((address - m_code_region_start) / CPU::INSTRUCTION_SIZE);
}

VirtualMemoryAddress DebuggerCodeModel::getAddressForRow(int row) const
{
  return m_code_region_start + (static_cast<u32>(row) * CPU::INSTRUCTION_SIZE);
}

VirtualMemoryAddress DebuggerCodeModel::getAddressForIndex(QModelIndex index) const
{
  return getAddressForRow(index.row());
}

int DebuggerCodeModel::getRowForPC() const
{
  return getRowForAddress(m_last_pc);
}

QVariant DebuggerCodeModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  if (index.column() < 0 || index.column() >= NUM_COLUMNS)
    return QVariant();

  if (role == Qt::DisplayRole)
  {
    const VirtualMemoryAddress address = getAddressForRow(index.row());
    switch (index.column())
    {
      case 0:
        // breakpoint
        return QVariant();

      case 1:
      {
        // Address
        return QVariant(QString::asprintf("0x%08X", address));
      }

      case 2:
      {
        // Bytes
        u32 instruction_bits;
        if (!CPU::SafeReadInstruction(address, &instruction_bits))
          return tr("<invalid>");

        return QString::asprintf("%08X", instruction_bits);
      }

      case 3:
      {
        // Instruction
        u32 instruction_bits;
        if (!CPU::SafeReadInstruction(address, &instruction_bits))
          return tr("<invalid>");

        SmallString str;
        CPU::DisassembleInstruction(&str, address, instruction_bits);
        return QString::fromUtf8(str.GetCharArray(), static_cast<int>(str.GetLength()));
      }

      case 4:
      {
        // Comment
        if (address != m_last_pc)
          return QVariant();

        u32 instruction_bits;
        if (!CPU::SafeReadInstruction(address, &instruction_bits))
          return tr("<invalid>");

        TinyString str;
        CPU::DisassembleInstructionComment(&str, address, instruction_bits, &CPU::g_state.regs);
        return QString::fromUtf8(str.GetCharArray(), static_cast<int>(str.GetLength()));
      }

      default:
        return QVariant();
    }
  }
  else if (role == Qt::DecorationRole)
  {
    if (index.column() == 0)
    {
      // breakpoint
      const VirtualMemoryAddress address = getAddressForRow(index.row());
      if (m_last_pc == address)
        return m_pc_pixmap;
      else if (hasBreakpointAtAddress(address))
        return m_breakpoint_pixmap;
    }

    return QVariant();
  }
  else if (role == Qt::BackgroundRole)
  {
    const VirtualMemoryAddress address = getAddressForRow(index.row());

    // breakpoint
    if (hasBreakpointAtAddress(address))
      return QVariant(QColor(171, 97, 107));

    //     if (address == m_last_pc)
    //       return QApplication::palette().toolTipBase();
    if (address == m_last_pc)
      return QColor(100, 100, 0);
    else
      return QVariant();
  }
  else if (role == Qt::ForegroundRole)
  {
    const VirtualMemoryAddress address = getAddressForRow(index.row());

    //     if (address == m_last_pc)
    //       return QApplication::palette().toolTipText();
    if (address == m_last_pc || hasBreakpointAtAddress(address))
      return QColor(Qt::white);
    else
      return QVariant();
  }
  else
  {
    return QVariant();
  }
}

QVariant DebuggerCodeModel::headerData(int section, Qt::Orientation orientation, int role /*= Qt::DisplayRole*/) const
{
  if (orientation != Qt::Horizontal)
    return QVariant();

  if (role != Qt::DisplayRole)
    return QVariant();

  switch (section)
  {
    case 1:
      return tr("Address");
    case 2:
      return tr("Bytes");
    case 3:
      return tr("Instruction");
    case 4:
      return tr("Comment");
    case 0:
    default:
      return QVariant();
  }
}

bool DebuggerCodeModel::updateRegion(VirtualMemoryAddress address)
{
  CPU::Segment segment = CPU::GetSegmentForAddress(address);
  std::optional<Bus::MemoryRegion> region = Bus::GetMemoryRegionForAddress(CPU::VirtualAddressToPhysical(address));
  if (!region.has_value() || (address >= m_code_region_start && address < m_code_region_end))
    return false;

  static constexpr unsigned NUM_INSTRUCTIONS_BEFORE = 4096;
  static constexpr unsigned NUM_INSTRUCTIONS_AFTER = 4096;
  static constexpr unsigned NUM_BYTES_BEFORE = NUM_INSTRUCTIONS_BEFORE * sizeof(CPU::Instruction);
  static constexpr unsigned NUM_BYTES_AFTER = NUM_INSTRUCTIONS_AFTER * sizeof(CPU::Instruction);

  const VirtualMemoryAddress start_address =
    CPU::PhysicalAddressToVirtual(Bus::GetMemoryRegionStart(region.value()), segment);
  const VirtualMemoryAddress end_address =
    CPU::PhysicalAddressToVirtual(Bus::GetMemoryRegionEnd(region.value()), segment);

  beginResetModel();
  m_code_region_start = ((address - start_address) < NUM_BYTES_BEFORE) ? start_address : (address - NUM_BYTES_BEFORE);
  m_code_region_end = ((end_address - address) < NUM_BYTES_AFTER) ? end_address : (address + NUM_BYTES_AFTER);
  m_current_segment = segment;
  m_current_code_region = region.value();
  endResetModel();
  return true;
}

bool DebuggerCodeModel::emitDataChangedForAddress(VirtualMemoryAddress address)
{
  CPU::Segment segment = CPU::GetSegmentForAddress(address);
  std::optional<Bus::MemoryRegion> region = Bus::GetMemoryRegionForAddress(CPU::VirtualAddressToPhysical(address));
  if (!region.has_value() || segment != m_current_segment || region != m_current_code_region)
    return false;

  const int row = getRowForAddress(address);
  emit dataChanged(index(row, 0), index(row, NUM_COLUMNS - 1));
  return true;
}

bool DebuggerCodeModel::hasBreakpointAtAddress(VirtualMemoryAddress address) const
{
  return std::find(m_breakpoints.begin(), m_breakpoints.end(), address) != m_breakpoints.end();
}

void DebuggerCodeModel::resetCodeView(VirtualMemoryAddress start_address)
{
  updateRegion(start_address);
}

void DebuggerCodeModel::setPC(VirtualMemoryAddress pc)
{
  const VirtualMemoryAddress prev_pc = m_last_pc;

  m_last_pc = pc;
  if (!updateRegion(pc))
  {
    emitDataChangedForAddress(prev_pc);
    emitDataChangedForAddress(pc);
  }
}

void DebuggerCodeModel::ensureAddressVisible(VirtualMemoryAddress address)
{
  updateRegion(address);
}

void DebuggerCodeModel::setBreakpointList(std::vector<VirtualMemoryAddress> bps)
{
  clearBreakpoints();

  m_breakpoints = std::move(bps);
  for (VirtualMemoryAddress bp : m_breakpoints)
    emitDataChangedForAddress(bp);
}

void DebuggerCodeModel::clearBreakpoints()
{
  std::vector<VirtualMemoryAddress> old_bps(std::move(m_breakpoints));

  for (VirtualMemoryAddress old_bp : old_bps)
    emitDataChangedForAddress(old_bp);
}

void DebuggerCodeModel::setBreakpointState(VirtualMemoryAddress address, bool enabled)
{
  if (enabled)
  {
    if (std::find(m_breakpoints.begin(), m_breakpoints.end(), address) != m_breakpoints.end())
      return;

    m_breakpoints.push_back(address);
    emitDataChangedForAddress(address);
  }
  else
  {
    auto it = std::find(m_breakpoints.begin(), m_breakpoints.end(), address);
    if (it == m_breakpoints.end())
      return;

    m_breakpoints.erase(it);
    emitDataChangedForAddress(address);
  }
}

DebuggerRegistersModel::DebuggerRegistersModel(QObject* parent /*= nullptr*/) : QAbstractListModel(parent) {}

DebuggerRegistersModel::~DebuggerRegistersModel() {}

int DebuggerRegistersModel::rowCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return static_cast<int>(CPU::Reg::count);
}

int DebuggerRegistersModel::columnCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return 2;
}

QVariant DebuggerRegistersModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  u32 reg_index = static_cast<u32>(index.row());
  if (reg_index >= static_cast<u32>(CPU::Reg::count))
    return QVariant();

  if (index.column() < 0 || index.column() > 1)
    return QVariant();

  switch (index.column())
  {
    case 0: // address
    {
      if (role == Qt::DisplayRole)
        return QString::fromUtf8(CPU::GetRegName(static_cast<CPU::Reg>(reg_index)));
    }
    break;

    case 1: // data
    {
      if (role == Qt::DisplayRole)
      {
        return QString::asprintf("0x%08X", CPU::g_state.regs.r[reg_index]);
      }
      else if (role == Qt::ForegroundRole)
      {
        if (CPU::g_state.regs.r[reg_index] != m_old_reg_values[reg_index])
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

void DebuggerRegistersModel::invalidateView()
{
  beginResetModel();
  endResetModel();
}

void DebuggerRegistersModel::saveCurrentValues()
{
  for (u32 i = 0; i < static_cast<u32>(CPU::Reg::count); i++)
    m_old_reg_values[i] = CPU::g_state.regs.r[i];
}

DebuggerStackModel::DebuggerStackModel(QObject* parent /*= nullptr*/) : QAbstractListModel(parent) {}

DebuggerStackModel::~DebuggerStackModel() {}

int DebuggerStackModel::rowCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return STACK_RANGE * 2;
}

int DebuggerStackModel::columnCount(const QModelIndex& parent /*= QModelIndex()*/) const
{
  return 2;
}

QVariant DebuggerStackModel::data(const QModelIndex& index, int role /*= Qt::DisplayRole*/) const
{
  if (index.column() < 0 || index.column() > 1)
    return QVariant();

  if (role != Qt::DisplayRole)
    return QVariant();

  const u32 sp = CPU::g_state.regs.sp;
  const VirtualMemoryAddress address =
    (sp - static_cast<u32>(STACK_RANGE * STACK_VALUE_SIZE)) + static_cast<u32>(index.row()) * STACK_VALUE_SIZE;

  if (index.column() == 0)
    return QString::asprintf("0x%08X", address);

  u32 value;
  if (!CPU::SafeReadMemoryWord(address, &value))
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

void DebuggerStackModel::invalidateView()
{
  beginResetModel();
  endResetModel();
}
