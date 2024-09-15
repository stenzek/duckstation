// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_debuggerwindow.h"

#include "core/cpu_core.h"
#include "core/types.h"

#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>
#include <memory>
#include <optional>

namespace Bus {
enum class MemoryRegion;
}

class DebuggerCodeModel;
class DebuggerRegistersModel;
class DebuggerStackModel;

class DebuggerWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit DebuggerWindow(QWidget* parent = nullptr);
  ~DebuggerWindow();

Q_SIGNALS:
  void closed();

protected:
  void closeEvent(QCloseEvent* event);

private Q_SLOTS:
  void onSystemStarted();
  void onSystemDestroyed();
  void onSystemPaused();
  void onSystemResumed();
  void onDebuggerMessageReported(const QString& message);

  void timerRefresh();
  void refreshAll();

  void onPauseActionToggled(bool paused);
  void onRunToCursorTriggered();
  void onGoToPCTriggered();
  void onGoToAddressTriggered();
  void onDumpAddressTriggered();
  void onFollowAddressTriggered();
  void onTraceTriggered();
  void onAddBreakpointTriggered();
  void onToggleBreakpointTriggered();
  void onClearBreakpointsTriggered();
  void onBreakpointListContextMenuRequested();
  void onBreakpointListItemChanged(QTreeWidgetItem* item, int column);
  void onStepIntoActionTriggered();
  void onStepOverActionTriggered();
  void onStepOutActionTriggered();
  void onCodeViewItemActivated(QModelIndex index);
  void onCodeViewContextMenuRequested(const QPoint& pt);
  void onMemorySearchTriggered();
  void onMemorySearchStringChanged(const QString&);

private:
  void setupAdditionalUi();
  void connectSignals();
  void disconnectSignals();
  void createModels();
  void setUIEnabled(bool enabled, bool allow_pause);
  void saveCurrentState();
  void setMemoryViewRegion(Bus::MemoryRegion region);
  void toggleBreakpoint(VirtualMemoryAddress address);
  void clearBreakpoints();
  std::optional<VirtualMemoryAddress> getSelectedCodeAddress();
  bool tryFollowLoadStore(VirtualMemoryAddress address);
  void scrollToPC(bool center);
  void scrollToCodeAddress(VirtualMemoryAddress address, bool center);
  bool scrollToMemoryAddress(VirtualMemoryAddress address);
  void refreshBreakpointList();
  void refreshBreakpointList(const CPU::BreakpointList& bps);
  void addBreakpoint(CPU::BreakpointType type, u32 address);
  void removeBreakpoint(CPU::BreakpointType type, u32 address);

  Ui::DebuggerWindow m_ui;

  std::unique_ptr<DebuggerCodeModel> m_code_model;
  std::unique_ptr<DebuggerRegistersModel> m_registers_model;
  std::unique_ptr<DebuggerStackModel> m_stack_model;

  QTimer m_refresh_timer;

  Bus::MemoryRegion m_active_memory_region;

  PhysicalMemoryAddress m_next_memory_search_address = 0;
};
