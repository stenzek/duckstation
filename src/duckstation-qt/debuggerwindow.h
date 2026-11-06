// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_debuggerwindow.h"

#include "core/cpu_core.h"
#include "core/types.h"

#include "util/symbol_file.h"

#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>
#include <memory>
#include <optional>

class INISettingsInterface;
class Error;

namespace Bus {
enum class MemoryRegion;
}

class DebuggerRegistersModel;
class DebuggerStackModel;
class DebuggerCallStackModel;
class DebuggerThreadsModel;

class DebuggerWindow : public QMainWindow
{
  Q_OBJECT

public:
  explicit DebuggerWindow(QWidget* parent = nullptr);
  ~DebuggerWindow();

  void reportMessage(const QString& message);
  void updateBreakpointHitCounts(const CPU::BreakpointList& bps);

  /// Parses a hex address or symbol name.
  std::optional<VirtualMemoryAddress> parseAddress(const QString& str) const;

Q_SIGNALS:
  void closed();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  enum class SymbolSource
  {
    Automatic,
    Manual,
    Disabled,
  };

  void saveWindowState();
  void restoreWindowState();
  void resetLayout();
  void setupAdditionalUi();
  void connectSignals();
  void createModels();
  void setUIEnabled(bool enabled, bool allow_pause);
  void saveCurrentState();
  void setMemoryViewRegion(Bus::MemoryRegion region);
  void startPatchInstruction(VirtualMemoryAddress address);
  void patchInstructions(VirtualMemoryAddress start_address, VirtualMemoryAddress end_address, u32 bits);
  void copySelectedCodeToClipboard();
  void toggleBreakpoint(VirtualMemoryAddress address);
  void clearBreakpoints();
  bool tryFollowLoadStore(VirtualMemoryAddress address);
  void scrollToPC(bool center);
  void scrollToCodeAddress(VirtualMemoryAddress address, bool center);
  bool scrollToMemoryAddress(VirtualMemoryAddress address);
  bool scrollToStackAddress(VirtualMemoryAddress address);
  void refreshBreakpointList();
  void refreshBreakpointList(const CPU::BreakpointList& bps);
  void addBreakpoint(CPU::BreakpointType type, u32 address);
  void removeBreakpoint(CPU::BreakpointType type, u32 address);
  void loadGameSettings(bool clear_existing);
  void saveGameSettings();
  void loadSymbols(const QString& path, const QString& exe_override);
  bool loadSymbolsFile(std::string path, Error* error);
  void updateSymbolUI();
  std::optional<VirtualMemoryAddress> promptForAddress(const QString& label);

  void onSystemStarted(const QString& path, const QString& serial, const QString& title, GameHash hash,
                       const QString& exe_override);
  void onSystemDestroyed();
  void onSystemGameChanged(const QString& path, const QString& serial, const QString& title, GameHash hash,
                           const QString& exe_override);
  void onSystemPaused();
  void onSystemResumed();

  void timerRefresh();
  void refreshAll();

  void onPauseActionTriggered(bool paused);
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
  void onCallStackItemDoubleClicked(const QModelIndex& index);
  void onCallStackContextMenuRequested(const QPoint& pt);
  void showValueContextMenu(QAbstractItemView* view, const QModelIndex& index, const QPoint& pt);
  void onStackItemDoubleClicked(const QModelIndex& index);
  void onStackContextMenuRequested(const QPoint& pt);
  void onRegisterItemDoubleClicked(const QModelIndex& index);
  void onRegisterContextMenuRequested(const QPoint& pt);
  void onThreadItemDoubleClicked(const QModelIndex& index);
  void onMemorySearchTriggered();
  void onMemorySearchStringChanged(const QString&);
  void onAutomaticSymbolsTriggered();
  void onLoadSymbolsTriggered();
  void onReloadSymbolsTriggered();
  void onDisableSymbolsTriggered();

  Ui::DebuggerWindow m_ui;

  DebuggerRegistersModel* m_registers_model;
  DebuggerStackModel* m_stack_model;
  DebuggerCallStackModel* m_call_stack_model;
  DebuggerThreadsModel* m_threads_model;

  QTimer m_refresh_timer;

  Bus::MemoryRegion m_active_memory_region;

  PhysicalMemoryAddress m_next_memory_search_address = 0;

  QString m_game_serial;

  std::unique_ptr<INISettingsInterface> m_settings;
  SymbolFile::SymbolList m_symbols;
  std::string m_symbol_path;
  SymbolSource m_symbol_source = SymbolSource::Automatic;
};
