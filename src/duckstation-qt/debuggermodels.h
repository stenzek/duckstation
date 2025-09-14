// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

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

class DebuggerRegistersModel final : public QAbstractListModel
{
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
public:
  explicit DebuggerStackModel(QObject* parent = nullptr);
  ~DebuggerStackModel() override;

  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

  void invalidateView();
};

class DebuggerAddBreakpointDialog final : public QDialog
{
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
