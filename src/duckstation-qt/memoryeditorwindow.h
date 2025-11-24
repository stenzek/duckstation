// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_memoryeditorwindow.h"

#include "core/types.h"

#include <QtCore/QTimer>
#include <QtWidgets/QWidget>
#include <optional>

class QShortcut;

class MemoryEditorWindow : public QWidget
{
  Q_OBJECT

public:
  explicit MemoryEditorWindow(QWidget* parent = nullptr);
  ~MemoryEditorWindow();

  bool scrollToMemoryAddress(VirtualMemoryAddress address);

Q_SIGNALS:
  void closed();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void setupAdditionalUi();
  void connectSignals();
  void updateUIEnabled();

  void onSystemStarted();
  void onSystemDestroyed();
  void onSystemPaused();
  void onSystemResumed();

  void timerRefresh();
  void refreshAll();
  void updateMemoryViewRegion();
  void updateDataInspector();

  void onMemoryViewTopAddressChanged(size_t address);

  void onAddressEditingFinished();
  void onDumpAddressTriggered();
  void onMemoryRegionButtonToggled(QAbstractButton*, bool checked);
  void onDataInspectorBaseButtonToggled(QAbstractButton*, bool checked);
  void onDataInspectorEndianButtonToggled(QAbstractButton*, bool checked);
  void onMemorySearchTriggered();
  void onMemorySearchStringChanged(const QString&);

  QString formatNumber(u64 value, bool is_signed, int byte_size) const;

  static QString formatAddress(VirtualMemoryAddress address);
  static void setIfChanged(QLineEdit* const widget, const QString& text);

  Ui::MemoryEditorWindow m_ui;

  QShortcut* m_go_shortcut = nullptr;

  QTimer m_refresh_timer;

  PhysicalMemoryAddress m_next_memory_search_address = 0;
};
