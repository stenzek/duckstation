// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "ui_debuggeraddbreakpointdialog.h"

#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/cpu_types.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QAbstractTableModel>
#include <QtGui/QPixmap>
#include <QtWidgets/QDialog>
#include <map>

class DebuggerCodeModel final : public QAbstractTableModel
{
  Q_OBJECT

public:
  DebuggerCodeModel(QObject* parent = nullptr);
  ~DebuggerCodeModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  // Returns the row for this instruction pointer
  void resetCodeView(VirtualMemoryAddress start_address);
  int getRowForAddress(VirtualMemoryAddress address) const;
  int getRowForPC() const;
  VirtualMemoryAddress getAddressForRow(int row) const;
  VirtualMemoryAddress getAddressForIndex(QModelIndex index) const;
  void setPC(VirtualMemoryAddress pc);
  void ensureAddressVisible(VirtualMemoryAddress address);
  void setBreakpointList(std::vector<VirtualMemoryAddress> bps);
  void setBreakpointState(VirtualMemoryAddress address, bool enabled);
  void clearBreakpoints();

private:
  bool updateRegion(VirtualMemoryAddress address);
  bool emitDataChangedForAddress(VirtualMemoryAddress address);
  bool hasBreakpointAtAddress(VirtualMemoryAddress address) const;

  Bus::MemoryRegion m_current_code_region = Bus::MemoryRegion::Count;
  CPU::Segment m_current_segment = CPU::Segment::KUSEG;
  VirtualMemoryAddress m_code_region_start = 0;
  VirtualMemoryAddress m_code_region_end = 0;
  VirtualMemoryAddress m_last_pc = 0;
  std::vector<VirtualMemoryAddress> m_breakpoints;

  QPixmap m_pc_pixmap;
  QPixmap m_breakpoint_pixmap;
};

class DebuggerRegistersModel final : public QAbstractListModel
{
  Q_OBJECT

public:
  DebuggerRegistersModel(QObject* parent = nullptr);
  ~DebuggerRegistersModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  void updateValues();
  void saveCurrentValues();

private:
  std::array<u32, CPU::NUM_DEBUGGER_REGISTER_LIST_ENTRIES> m_reg_values = {};
  std::array<u32, CPU::NUM_DEBUGGER_REGISTER_LIST_ENTRIES> m_old_reg_values = {};
};

class DebuggerStackModel final : public QAbstractListModel
{
  Q_OBJECT

public:
  DebuggerStackModel(QObject* parent = nullptr);
  ~DebuggerStackModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  void invalidateView();
};

class DebuggerAddBreakpointDialog final : public QDialog
{
  Q_OBJECT

public:
  DebuggerAddBreakpointDialog(QWidget* parent = nullptr);
  ~DebuggerAddBreakpointDialog() override;

  u32 getAddress() const { return m_address; }
  CPU::BreakpointType getType() const { return m_type; }

private Q_SLOTS:
  void okClicked();

private:
  Ui::DebuggerAddBreakpointDialog m_ui;
  u32 m_address = 0;
  CPU::BreakpointType m_type = CPU::BreakpointType::Execute;
};
