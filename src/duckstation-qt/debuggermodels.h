// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_debuggeraddbreakpointdialog.h"

#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/cpu_types.h"
#include "core/cpu_call_stack.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QAbstractListModel>
#include <QtCore/QAbstractTableModel>
#include <QtGui/QPixmap>
#include <QtWidgets/QDialog>
#include <array>
#include <map>
#include <optional>
#include <vector>

class DebuggerRegistersModel final : public QAbstractListModel
{
  Q_OBJECT

public:
  explicit DebuggerRegistersModel(QObject* parent = nullptr);
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
  explicit DebuggerStackModel(QObject* parent = nullptr);
  ~DebuggerStackModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  QModelIndex getIndexForAddress(VirtualMemoryAddress address) const;
  std::optional<VirtualMemoryAddress> getAddressForIndex(const QModelIndex& index) const;
  void invalidateView();
};

class DebuggerCallStackModel final : public QAbstractTableModel
{
  Q_OBJECT

public:
  explicit DebuggerCallStackModel(QObject* parent = nullptr);
  ~DebuggerCallStackModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  const CPU::CallStack::Frame* getFrame(const QModelIndex& index) const;

  void updateValues();
  void clear();

private:
  CPU::CallStack::FrameList m_frames;
};

class DebuggerThreadsModel final : public QAbstractItemModel
{
  Q_OBJECT

public:
  explicit DebuggerThreadsModel(QObject* parent = nullptr);
  ~DebuggerThreadsModel() override;

  QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
  QModelIndex parent(const QModelIndex& child) const override;
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  void updateValues();
  void clear();
  std::optional<VirtualMemoryAddress> getThreadPC(const QModelIndex& index) const;

private:
  static constexpr u32 NUM_REGISTER_VALUES = 36;

  struct Thread
  {
    std::array<u32, NUM_REGISTER_VALUES> registers;
    u32 index;
    u32 handle;
    VirtualMemoryAddress pc;
    bool current;
  };

  std::vector<Thread> m_threads;
};

class DebuggerAddBreakpointDialog final : public QDialog
{
  Q_OBJECT

public:
  explicit DebuggerAddBreakpointDialog(QWidget* parent = nullptr);
  ~DebuggerAddBreakpointDialog() override;

  u32 getAddress() const { return m_address; }
  CPU::BreakpointType getType() const { return m_type; }

private:
  void okClicked();

  Ui::DebuggerAddBreakpointDialog m_ui;
  u32 m_address = 0;
  CPU::BreakpointType m_type = CPU::BreakpointType::Execute;
};
