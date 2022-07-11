#pragma once
#include "core/types.h"
#include "ui_debuggerwindow.h"
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

public Q_SLOTS:
  void onEmulationPaused();
  void onEmulationResumed();

protected:
  void closeEvent(QCloseEvent* event);

private Q_SLOTS:
  void onDebuggerMessageReported(const QString& message);

  void refreshAll();

  void scrollToPC();

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
  void onStepIntoActionTriggered();
  void onStepOverActionTriggered();
  void onStepOutActionTriggered();
  void onCodeViewItemActivated(QModelIndex index);
  void onMemorySearchTriggered();
  void onMemorySearchStringChanged(const QString&);


private:
  void setupAdditionalUi();
  void connectSignals();
  void disconnectSignals();
  void createModels();
  void setUIEnabled(bool enabled);
  void setMemoryViewRegion(Bus::MemoryRegion region);
  void toggleBreakpoint(VirtualMemoryAddress address);
  void clearBreakpoints();
  std::optional<VirtualMemoryAddress> getSelectedCodeAddress();
  bool tryFollowLoadStore(VirtualMemoryAddress address);
  void scrollToCodeAddress(VirtualMemoryAddress address);
  bool scrollToMemoryAddress(VirtualMemoryAddress address);
  void refreshBreakpointList();

  Ui::DebuggerWindow m_ui;

  std::unique_ptr<DebuggerCodeModel> m_code_model;
  std::unique_ptr<DebuggerRegistersModel> m_registers_model;
  std::unique_ptr<DebuggerStackModel> m_stack_model;

  Bus::MemoryRegion m_active_memory_region;

  PhysicalMemoryAddress m_next_memory_search_address = 0;
};
