// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "debuggermodels.h"
#include "qtutils.h"

#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/cpu_disasm.h"

#include "common/small_string.h"

#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QPushButton>

#include "moc_debuggermodels.cpp"

static constexpr int STACK_RANGE = 128;
static constexpr u32 STACK_VALUE_SIZE = sizeof(u32);

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
      QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                               tr("DebuggerWindow", "Invalid address. It should be in hex (0x12345678 or 12345678)"));
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
