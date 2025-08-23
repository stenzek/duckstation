// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core/bus.h"
#include "core/cpu_types.h"
#include "core/types.h"

#include <QtCore/QPoint>
#include <QtGui/QPixmap>
#include <QtWidgets/QWidget>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QAbstractScrollArea>

#include <memory>
#include <optional>
#include <vector>

class DebuggerCodeView : public QAbstractScrollArea
{
  Q_OBJECT

public:
  explicit DebuggerCodeView(QWidget* parent = nullptr);
  ~DebuggerCodeView();

  // Call when font or theme changes
  void updateRowHeight();

  void scrollToAddress(VirtualMemoryAddress address, bool center = false);
  std::optional<VirtualMemoryAddress> getSelectedAddress() const;
  void setSelectedAddress(VirtualMemoryAddress address);
  VirtualMemoryAddress getAddressAtPoint(const QPoint& point) const;

  // Code model functionality integrated
  void resetCodeView(VirtualMemoryAddress start_address);
  void setPC(VirtualMemoryAddress pc);
  void ensureAddressVisible(VirtualMemoryAddress address);
  void setBreakpointState(VirtualMemoryAddress address, bool enabled);
  void clearBreakpoints();
  VirtualMemoryAddress getPC() const { return m_last_pc; }
  bool hasBreakpointAtAddress(VirtualMemoryAddress address) const;

Q_SIGNALS:
  void toggleBreakpointActivated(VirtualMemoryAddress address);
  void addressActivated(VirtualMemoryAddress address);
  void commentActivated(VirtualMemoryAddress address);
  void contextMenuRequested(const QPoint& point, VirtualMemoryAddress address);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  // Column positions
  static constexpr int ARROW_COLUMN_WIDTH = 60;
  static constexpr int BREAKPOINT_COLUMN_WIDTH = 20;
  static constexpr int ADDRESS_COLUMN_WIDTH = 100;
  static constexpr int BYTES_COLUMN_WIDTH = 90;
  static constexpr int INSTRUCTION_COLUMN_WIDTH = 250;

  static constexpr int BREAKPOINT_COLUMN_START = ARROW_COLUMN_WIDTH;
  static constexpr int ADDRESS_COLUMN_START = BREAKPOINT_COLUMN_START + BREAKPOINT_COLUMN_WIDTH;
  static constexpr int BYTES_COLUMN_START = ADDRESS_COLUMN_START + ADDRESS_COLUMN_WIDTH;
  static constexpr int INSTRUCTION_COLUMN_START = BYTES_COLUMN_START + BYTES_COLUMN_WIDTH;
  static constexpr int COMMENT_COLUMN_START = INSTRUCTION_COLUMN_START + INSTRUCTION_COLUMN_WIDTH;

  struct BranchArrow
  {
    VirtualMemoryAddress source;
    VirtualMemoryAddress target;
    bool is_conditional;
    bool is_forward;
  };

  // Address/row conversion methods
  int getRowForAddress(VirtualMemoryAddress address) const;
  VirtualMemoryAddress getAddressForRow(int row) const;
  
  // Memory region management
  bool updateRegion(VirtualMemoryAddress address);
  
  // Drawing methods
  void updateScrollBars();
  void updateVisibleRange();
  void calculateBranchArrows();
  void drawBranchArrows(QPainter& painter, const QRect& visible_rect);
  void drawInstruction(QPainter& painter, VirtualMemoryAddress address, int y, bool is_selected, bool is_pc);
  
  int getVisibleRowCount() const;
  VirtualMemoryAddress getFirstVisibleAddress() const;
  VirtualMemoryAddress getLastVisibleAddress() const;

  int m_row_height = 1;
  int m_char_width = 0;
   
  VirtualMemoryAddress m_selected_address = 0;
  bool m_has_selection = false;
  
  std::vector<BranchArrow> m_branch_arrows;
  
  QPixmap m_pc_pixmap;
  QPixmap m_breakpoint_pixmap;
  
  // Scroll state
  VirtualMemoryAddress m_top_address = 0;
  int m_visible_rows = 0;
  
  // Code region state (from DebuggerCodeModel)
  Bus::MemoryRegion m_current_code_region = Bus::MemoryRegion::Count;
  CPU::Segment m_current_segment = CPU::Segment::KUSEG;
  VirtualMemoryAddress m_code_region_start = 0;
  VirtualMemoryAddress m_code_region_end = 0;
  VirtualMemoryAddress m_last_pc = 0;
  std::vector<VirtualMemoryAddress> m_breakpoints;
};
