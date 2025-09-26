// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_debuggerwindow.h"

#include "core/cpu_core.h"
#include "core/types.h"

#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>
#include <optional>

namespace Bus {
enum class MemoryRegion;
}

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

private:
  void setupAdditionalUi();
  void connectSignals();
  void createModels();
  void setUIEnabled(bool enabled, bool allow_pause);
  void saveCurrentState();
  void setMemoryViewRegion(Bus::MemoryRegion region);
  void toggleBreakpoint(VirtualMemoryAddress address);
  void clearBreakpoints();
  bool tryFollowLoadStore(VirtualMemoryAddress address);
  void scrollToPC(bool center);
  void scrollToCodeAddress(VirtualMemoryAddress address, bool center);
  bool scrollToMemoryAddress(VirtualMemoryAddress address);
  void refreshBreakpointList();
  void refreshBreakpointList(const CPU::BreakpointList& bps);
  void addBreakpoint(CPU::BreakpointType type, u32 address);
  void removeBreakpoint(CPU::BreakpointType type, u32 address);

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
  void onTraceTriggered();
  void onAddBreakpointTriggered();
  void onToggleBreakpointTriggered();
  void onClearBreakpointsTriggered();
  void onBreakpointListContextMenuRequested();
  void onBreakpointListItemChanged(QTreeWidgetItem* item, int column);
  void onStepIntoActionTriggered();
  void onStepOverActionTriggered();
  void onStepOutActionTriggered();
  void onCodeViewAddressActivated(VirtualMemoryAddress address);
  void onCodeViewToggleBreakpointActivated(VirtualMemoryAddress address);
  void onCodeViewCommentActivated(VirtualMemoryAddress address);
  void onCodeViewContextMenuRequested(const QPoint& pt);
  void onMemorySearchTriggered();
  void onMemorySearchStringChanged(const QString&);

  Ui::DebuggerWindow m_ui;

  DebuggerRegistersModel* m_registers_model;
  DebuggerStackModel* m_stack_model;

  QTimer m_refresh_timer;

  Bus::MemoryRegion m_active_memory_region;

  PhysicalMemoryAddress m_next_memory_search_address = 0;
};
