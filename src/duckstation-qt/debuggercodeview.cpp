// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "debuggercodeview.h"
#include "qtutils.h"

#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/cpu_disasm.h"
#include "core/cpu_types.h"

#include "common/log.h"
#include "common/small_string.h"

#include <QtCore/QTimer>
#include <QtGui/QFontDatabase>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyleOption>

#include <algorithm>

#include "moc_debuggercodeview.cpp"

LOG_CHANNEL(Host);

using namespace Qt::StringLiterals;

DebuggerCodeView::DebuggerCodeView(QWidget* parent) : QAbstractScrollArea(parent)
{
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  updateRowHeight();

  // Load icons
  m_pc_pixmap = QIcon(":/icons/debug-pc.png"_L1).pixmap(12);
  m_breakpoint_pixmap = QIcon(":/icons/media-record.png"_L1).pixmap(12);

  // Connect scroll bar
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
    const VirtualMemoryAddress new_top = getAddressForRow(value);
    // Clamp to valid range to prevent wrapping
    if (new_top >= m_code_region_start && new_top < m_code_region_end)
    {
      m_top_address = new_top;
      updateVisibleRange();
      // Recalculate arrows when scroll position changes
      calculateBranchArrows();
      viewport()->update();
    }
  });

  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_OpaquePaintEvent);

  // Initialize code region
  resetCodeView(0);
}

DebuggerCodeView::~DebuggerCodeView() = default;

void DebuggerCodeView::updateRowHeight()
{
  const QFontMetrics font_metrics(fontMetrics());
  m_row_height = font_metrics.height() + 2;
  m_char_width = font_metrics.horizontalAdvance('M');
}

void DebuggerCodeView::resetCodeView(VirtualMemoryAddress start_address)
{
  updateRegion(start_address);
  calculateBranchArrows(); // Recalculate arrows when region changes
}

void DebuggerCodeView::setPC(VirtualMemoryAddress pc)
{
  if (m_last_pc == pc)
    return;

  m_last_pc = pc;

  if (!updateRegion(pc))
  {
    // Just update the display if region didn't change
    viewport()->update();
  }
  else
  {
    // Region changed, recalculate arrows
    calculateBranchArrows();
  }
}

void DebuggerCodeView::invalidatePC()
{
  // something that will always pass the test above
  m_last_pc = 0xFFFFFFFFu;
  viewport()->update();
}

void DebuggerCodeView::ensureAddressVisible(VirtualMemoryAddress address)
{
  const bool region_changed = updateRegion(address);
  if (region_changed)
  {
    calculateBranchArrows();
  }
}

void DebuggerCodeView::updateBreakpointList(const CPU::BreakpointList& bps)
{
  static constexpr auto pred = [](const CPU::Breakpoint& bp) { return (bp.type == CPU::BreakpointType::Execute); };

  m_breakpoints.clear();
  m_breakpoints.reserve(std::count_if(bps.begin(), bps.end(), pred));

  for (const CPU::Breakpoint& bp : bps)
  {
    if (pred(bp))
      m_breakpoints.push_back(bp.address);
  }

  viewport()->update();
}

void DebuggerCodeView::clearBreakpoints()
{
  m_breakpoints.clear();
  viewport()->update();
}

bool DebuggerCodeView::hasBreakpointAtAddress(VirtualMemoryAddress address) const
{
  return std::find(m_breakpoints.begin(), m_breakpoints.end(), address) != m_breakpoints.end();
}

int DebuggerCodeView::getRowForAddress(VirtualMemoryAddress address) const
{
  if (address < m_code_region_start)
    return 0;
  if (address >= m_code_region_end)
    return static_cast<int>((m_code_region_end - m_code_region_start) / CPU::INSTRUCTION_SIZE) - 1;

  return static_cast<int>((address - m_code_region_start) / CPU::INSTRUCTION_SIZE);
}

VirtualMemoryAddress DebuggerCodeView::getAddressForRow(int row) const
{
  const VirtualMemoryAddress address = m_code_region_start + (static_cast<u32>(row) * CPU::INSTRUCTION_SIZE);
  return std::clamp(address, m_code_region_start, m_code_region_end - CPU::INSTRUCTION_SIZE);
}

bool DebuggerCodeView::updateRegion(VirtualMemoryAddress address)
{
  CPU::Segment segment = CPU::GetSegmentForAddress(address);
  std::optional<Bus::MemoryRegion> region = Bus::GetMemoryRegionForAddress(CPU::VirtualAddressToPhysical(address));
  if (!region.has_value() || (address >= m_code_region_start && address < m_code_region_end))
    return false;

  static constexpr unsigned NUM_INSTRUCTIONS_BEFORE = 4096;
  static constexpr unsigned NUM_INSTRUCTIONS_AFTER = 4096;
  static constexpr unsigned NUM_BYTES_BEFORE = NUM_INSTRUCTIONS_BEFORE * sizeof(u32);
  static constexpr unsigned NUM_BYTES_AFTER = NUM_INSTRUCTIONS_AFTER * sizeof(u32);

  const VirtualMemoryAddress start_address =
    CPU::PhysicalAddressToVirtual(Bus::GetMemoryRegionStart(region.value()), segment);
  const VirtualMemoryAddress end_address =
    CPU::PhysicalAddressToVirtual(Bus::GetMemoryRegionEnd(region.value()), segment);

  m_code_region_start = ((address - start_address) < NUM_BYTES_BEFORE) ? start_address : (address - NUM_BYTES_BEFORE);
  m_code_region_end = ((end_address - address) < NUM_BYTES_AFTER) ? end_address : (address + NUM_BYTES_AFTER);
  m_current_segment = segment;
  m_current_code_region = region.value();

  updateScrollBars();
  viewport()->update();
  return true;
}

void DebuggerCodeView::scrollToAddress(VirtualMemoryAddress address, bool center)
{
  ensureAddressVisible(address);

  VirtualMemoryAddress old_top = m_top_address;

  if (center)
  {
    const int visible_rows = getVisibleRowCount();
    const int target_row = getRowForAddress(address);
    const int top_row = std::max(0, target_row - visible_rows / 2);
    m_top_address = getAddressForRow(top_row);
  }
  else
  {
    const VirtualMemoryAddress first_visible = getFirstVisibleAddress();
    const VirtualMemoryAddress last_visible = getLastVisibleAddress();

    if (address < first_visible || address > last_visible)
    {
      m_top_address = address;
    }
  }

  // Only recalculate arrows if scroll position actually changed
  if (m_top_address != old_top)
  {
    calculateBranchArrows();
  }

  updateScrollBars();
  updateVisibleRange();
  viewport()->update();
}

std::optional<VirtualMemoryAddress> DebuggerCodeView::getSelectedAddress() const
{
  if (!m_has_selection)
    return std::nullopt;
  return m_selected_address;
}

void DebuggerCodeView::setSelectedAddress(VirtualMemoryAddress address)
{
  m_selected_address = address;
  m_has_selection = true;
  viewport()->update();
}

VirtualMemoryAddress DebuggerCodeView::getAddressAtPoint(const QPoint& point) const
{
  const int top_row = getRowForAddress(m_top_address);
  const int clicked_row = top_row + (point.y() / m_row_height);
  return getAddressForRow(clicked_row);
}

void DebuggerCodeView::paintEvent(QPaintEvent* event)
{
  QPainter painter(viewport());
  painter.setFont(font());

  const QRect visible_rect = event->rect();

  // Calculate which rows are visible based on scroll position
  const int top_row = getRowForAddress(m_top_address);
  const int first_visible_row = top_row + (visible_rect.top() / m_row_height);
  const int last_visible_row = top_row + ((visible_rect.bottom() + m_row_height - 1) / m_row_height);

  painter.fillRect(visible_rect, palette().base());

  for (int row = first_visible_row; row <= last_visible_row; ++row)
  {
    const VirtualMemoryAddress address = getAddressForRow(row);

    // Calculate y position relative to scroll position
    const int y = (row - top_row) * m_row_height;

    if (y + m_row_height < visible_rect.top() || y > visible_rect.bottom())
      continue;

    const bool is_selected = (m_has_selection && address == m_selected_address);
    const bool is_pc = (address == m_last_pc);

    drawInstruction(painter, address, y, is_selected, is_pc);
  }

  drawBranchArrows(painter, visible_rect);
}

void DebuggerCodeView::drawInstruction(QPainter& painter, VirtualMemoryAddress address, int y, bool is_selected,
                                       bool is_pc)
{
  const bool has_breakpoint = hasBreakpointAtAddress(address);

  // Draw background
  if (has_breakpoint || is_pc || is_selected)
  {
    const QRect row_rect(0, y, viewport()->width(), m_row_height);

    QColor bg_color;
    if (has_breakpoint)
      bg_color = QColor(171, 97, 107);
    else if (is_pc)
      bg_color = QColor(100, 100, 0);
    else if (is_selected)
      bg_color = palette().highlight().color();
    else
      bg_color = palette().base().color();

    painter.fillRect(row_rect, bg_color);
  }

  // Set text color
  QColor address_color;
  QColor bytes_color;
  QColor instruction_color;
  QColor register_color;
  QColor immediate_color;
  QColor comment_color;
  if (is_pc || has_breakpoint)
  {
    address_color = bytes_color = instruction_color = register_color = immediate_color = comment_color = Qt::white;
  }
  else if (is_selected)
  {
    address_color = bytes_color = instruction_color = register_color = immediate_color = comment_color =
      palette().highlightedText().color();
  }
  else
  {
    instruction_color = palette().text().color();
    bytes_color = bytes_color.darker(200);
    address_color = instruction_color.darker(230);
    register_color = QColor(0, 150, 255);  // Blue for registers
    immediate_color = QColor(255, 150, 0); // Orange for immediates
    comment_color = QColor(150, 150, 150); // Gray for comments
  }

  // Start from the left edge of the arrow column
  int x = ARROW_COLUMN_WIDTH + 2;

  // Draw breakpoint/PC icon
  if (is_pc)
  {
    const int icon_y = y + 2 + (m_row_height - m_pc_pixmap.height()) / 2;
    painter.drawPixmap(x, icon_y, m_pc_pixmap);
  }
  else if (has_breakpoint)
  {
    const int icon_y = y + 2 + (m_row_height - m_breakpoint_pixmap.height()) / 2;
    painter.drawPixmap(x, icon_y, m_breakpoint_pixmap);
  }
  x += BREAKPOINT_COLUMN_WIDTH;

  // Draw address
  const QFontMetrics font_metrics = painter.fontMetrics();
  y += font_metrics.ascent() + 1;

  painter.setPen(address_color);
  const QString address_text = QString::asprintf("0x%08X", address);
  painter.drawText(x, y, address_text);
  x += ADDRESS_COLUMN_WIDTH;

  // Draw instruction bytes
  SmallString str;
  if (u32 instruction_bits; CPU::SafeReadInstruction(address, &instruction_bits))
  {
    const QString bytes_text = QString::asprintf("%08X", instruction_bits);
    painter.setPen(address_color);
    painter.drawText(x, y, bytes_text);
    x += BYTES_COLUMN_WIDTH;

    CPU::DisassembleInstruction(&str, address, instruction_bits);
    const QString disasm_text = QtUtils::StringViewToQString(str);
    painter.setPen(instruction_color);

    // Highlight registers and immediates in the disassembly
    qsizetype start_pos = 0;
    while (start_pos >= 0)
    {
      qsizetype end_pos = disasm_text.indexOf(' ', start_pos);
      if (end_pos > 0)
        end_pos++; // Include the space in the highlight

      const QString token = disasm_text.mid(start_pos, end_pos - start_pos);
      QColor color;
      if (start_pos == 0)
        color = instruction_color; // Instruction mnemonic
      else if (token[0].isDigit() || (token.length() > 1 && token[0] == '-' && token[1].isDigit()))
        color = immediate_color; // Immediate value
      else
        color = register_color; // Register

      painter.setPen(color);
      painter.drawText(x, y, token);
      x += font_metrics.horizontalAdvance(token);

      start_pos = end_pos;
    }

    if (is_pc)
    {
      str.clear();
      CPU::DisassembleInstructionComment(&str, address, instruction_bits);
      if (!str.empty())
      {
        painter.setPen(address_color);
        painter.drawText(COMMENT_COLUMN_START, y, QtUtils::StringViewToQString(str));
      }
    }
  }
  else
  {
    painter.setPen(instruction_color);
    painter.drawText(x, y, QStringLiteral("<invalid>"));
  }
}

void DebuggerCodeView::calculateBranchArrows()
{
  m_branch_arrows.clear();

  const VirtualMemoryAddress first_visible = getFirstVisibleAddress();
  const VirtualMemoryAddress last_visible = getLastVisibleAddress();

  // Expand search range to include arrows that might be partially visible
  const VirtualMemoryAddress search_start =
    (first_visible > 64 * CPU::INSTRUCTION_SIZE) ? first_visible - (64 * CPU::INSTRUCTION_SIZE) : m_code_region_start;
  const VirtualMemoryAddress search_end = std::min(last_visible + (64 * CPU::INSTRUCTION_SIZE), m_code_region_end);

  for (VirtualMemoryAddress addr = search_start; addr < search_end; addr += CPU::INSTRUCTION_SIZE)
  {
    u32 instruction_bits;
    if (!CPU::SafeReadInstruction(addr, &instruction_bits))
      continue;

    const CPU::Instruction instruction{instruction_bits};

    if (CPU::IsDirectBranchInstruction(instruction) && !CPU::IsCallInstruction(instruction))
    {
      const VirtualMemoryAddress target = CPU::GetDirectBranchTarget(instruction, addr);

      // Only include arrows where at least one end (source or target) is in the current region
      if ((addr >= search_start && addr < search_end) && (target >= search_start && target < search_end))
      {
        BranchArrow arrow;
        arrow.source = addr;
        arrow.target = target;
        arrow.is_conditional = !CPU::IsUnconditionalBranchInstruction(instruction);
        arrow.is_forward = (target > addr);
        m_branch_arrows.push_back(arrow);
      }
    }
  }
}

void DebuggerCodeView::drawBranchArrows(QPainter& painter, const QRect& visible_rect)
{
  if (m_branch_arrows.empty())
    return;

  const int top_row = getRowForAddress(m_top_address);
  const int arrow_left = 2;
  const int arrow_right = ARROW_COLUMN_WIDTH - 2;
  const int viewport_height = viewport()->height();

  static constexpr const QColor colors[] = {
    QColor(0, 150, 255), QColor(255, 0, 150),   QColor(0, 255, 0), QColor(255, 255, 0),
    QColor(150, 0, 255), QColor(150, 255, 255), QColor(0, 255, 0), QColor(0, 150, 255),
  };

  for (const BranchArrow& arrow : m_branch_arrows)
  {
    const int source_row = getRowForAddress(arrow.source);
    const int target_row = getRowForAddress(arrow.target);

    // Calculate y positions relative to scroll position
    const int source_y = (source_row - top_row) * m_row_height + m_row_height / 2;
    const int target_y = (target_row - top_row) * m_row_height + m_row_height / 2;

    // Only draw if at least part of the arrow is visible
    const int min_y = std::min(source_y, target_y);
    const int max_y = std::max(source_y, target_y);

    // Skip if completely outside viewport
    if (max_y < 0 || min_y > viewport_height)
      continue;

    // Clamp to viewport bounds for drawing
    const int clamped_source_y = std::clamp(source_y, 0, viewport_height);
    const int clamped_target_y = std::clamp(target_y, 0, viewport_height);

    // Choose color based on arrow type
    QColor arrow_color = colors[source_row % std::size(colors)];
    painter.setPen(QPen(arrow_color, 1)); // Extra thick for debugging

    int nest_level = 8 - (source_row % 8); // Adjust nesting level based on row

#if 0
    for (const BranchArrow& other : m_branch_arrows)
    {
      if (&other == &arrow) // Skip self-comparison
        break;

      const int other_source_row = getRowForAddress(other.source);
      const int other_target_row = getRowForAddress(other.target);
      if ((other_source_row < top_row || other_source_row >= end_row) &&
          (other_target_row < top_row || other_target_row >= end_row))
      {
        continue; // Skip arrows outside the visible range
      }

      const int min_row = std::min(source_row, target_row);
      const int max_row = std::max(source_row, target_row);
      const int other_min_row = std::min(other_source_row, other_target_row);
      const int other_max_row = std::max(other_source_row, other_target_row);

      // Check if ranges overlap
      if (!(max_row < other_min_row || min_row > other_max_row))
      {
        nest_level = std::max(nest_level - 1, 0);
      }
    }
#endif

    const int arrow_x = arrow_left + (nest_level * 4);

    // Draw debug circles at source and target positions
    painter.setBrush(arrow_color);
    painter.drawEllipse(arrow_right - 1, source_y - 1, 3, 3);

    // Draw straight line arrow with right angles
    if (source_y == target_y)
    {
      // Horizontal line for same-row branches
      painter.drawLine(arrow_x, source_y, arrow_x + 15, target_y);
    }
    else
    {
      // Horizontal line from left edge to source
      if (source_y >= 0 && source_y < viewport_height)
        painter.drawLine(arrow_x, source_y, arrow_right, source_y);

      // Vertical line from source to target
      painter.drawLine(arrow_x, clamped_source_y, arrow_x, clamped_target_y);

      // Horizontal line to target
      if (target_y >= 0 && target_y < viewport_height)
        painter.drawLine(arrow_x, target_y, arrow_right, target_y);

      // Draw arrowhead with simple lines
      const int arrow_size = 3;
      const bool pointing_down = (target_y > source_y);

      if (pointing_down)
      {
        painter.drawLine(arrow_right, target_y, arrow_right - arrow_size, target_y - arrow_size);
        painter.drawLine(arrow_right, target_y, arrow_right - arrow_size, target_y + arrow_size);
      }
      else
      {
        painter.drawLine(arrow_right, target_y, arrow_right - arrow_size, target_y - arrow_size);
        painter.drawLine(arrow_right, target_y, arrow_right - arrow_size, target_y + arrow_size);
      }
    }
  }
}

void DebuggerCodeView::mousePressEvent(QMouseEvent* event)
{
  if (event->button() == Qt::LeftButton)
  {
    const VirtualMemoryAddress address = getAddressAtPoint(event->pos());
    setSelectedAddress(address);
  }

  QAbstractScrollArea::mousePressEvent(event);
}

void DebuggerCodeView::mouseMoveEvent(QMouseEvent* event)
{
  if (event->buttons() & Qt::LeftButton)
  {
    const VirtualMemoryAddress address = getAddressAtPoint(event->pos());
    setSelectedAddress(address);
  }
  QAbstractScrollArea::mouseMoveEvent(event);
}

void DebuggerCodeView::mouseDoubleClickEvent(QMouseEvent* event)
{
  if (event->button() == Qt::LeftButton)
  {
    const VirtualMemoryAddress address = getAddressAtPoint(event->pos());

    const int x = event->pos().x();

    if ((x >= BREAKPOINT_COLUMN_START && x < ADDRESS_COLUMN_START) ||
        (x >= INSTRUCTION_COLUMN_START && x < COMMENT_COLUMN_START))
    {
      emit toggleBreakpointActivated(address);
    }
    else if (x >= ADDRESS_COLUMN_START && x < INSTRUCTION_COLUMN_START)
    {
      emit addressActivated(address);
    }
    else
    {
      emit commentActivated(address);
    }
  }

  QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void DebuggerCodeView::contextMenuEvent(QContextMenuEvent* event)
{
  const VirtualMemoryAddress address = getAddressAtPoint(event->pos());
  emit contextMenuRequested(event->pos(), address);
}

void DebuggerCodeView::keyPressEvent(QKeyEvent* event)
{
  if (!m_has_selection)
  {
    QAbstractScrollArea::keyPressEvent(event);
    return;
  }

  VirtualMemoryAddress new_address = m_selected_address;

  switch (event->key())
  {
    case Qt::Key_Up:
      new_address -= CPU::INSTRUCTION_SIZE;
      break;
    case Qt::Key_Down:
      new_address += CPU::INSTRUCTION_SIZE;
      break;
    case Qt::Key_PageUp:
      new_address -= CPU::INSTRUCTION_SIZE * getVisibleRowCount();
      break;
    case Qt::Key_PageDown:
      new_address += CPU::INSTRUCTION_SIZE * getVisibleRowCount();
      break;
    default:
      QAbstractScrollArea::keyPressEvent(event);
      return;
  }

  scrollToAddress(new_address, false);
  setSelectedAddress(new_address);
}

void DebuggerCodeView::wheelEvent(QWheelEvent* event)
{
  const int delta = event->angleDelta().y();
  const int steps = delta / 120; // Standard wheel step

  VirtualMemoryAddress old_top = m_top_address;
  VirtualMemoryAddress new_top = m_top_address - (steps * CPU::INSTRUCTION_SIZE * 3);

  // Clamp to valid range
  new_top = std::clamp(new_top, m_code_region_start, m_code_region_end - CPU::INSTRUCTION_SIZE);
  m_top_address = new_top;

  // Only recalculate arrows if scroll position actually changed
  if (m_top_address != old_top)
  {
    calculateBranchArrows();
  }

  updateScrollBars();
  updateVisibleRange();
  viewport()->update();

  event->accept();
}

void DebuggerCodeView::resizeEvent(QResizeEvent* event)
{
  QAbstractScrollArea::resizeEvent(event);
  updateScrollBars();
  updateVisibleRange();
  // Recalculate arrows on resize since visible range changes
  calculateBranchArrows();
}

void DebuggerCodeView::updateScrollBars()
{
  const int total_rows = static_cast<int>((m_code_region_end - m_code_region_start) / CPU::INSTRUCTION_SIZE);
  const int visible_rows = getVisibleRowCount();
  const int max_value = std::max(0, total_rows - visible_rows);

  verticalScrollBar()->setRange(0, max_value);
  verticalScrollBar()->setPageStep(visible_rows);

  const int current_row = getRowForAddress(m_top_address);
  verticalScrollBar()->setValue(current_row);
}

void DebuggerCodeView::updateVisibleRange()
{
  m_visible_rows = getVisibleRowCount();
}

int DebuggerCodeView::getVisibleRowCount() const
{
  return viewport()->height() / m_row_height;
}

VirtualMemoryAddress DebuggerCodeView::getFirstVisibleAddress() const
{
  return m_top_address;
}

VirtualMemoryAddress DebuggerCodeView::getLastVisibleAddress() const
{
  const int visible_rows = getVisibleRowCount();
  return m_top_address + (visible_rows * CPU::INSTRUCTION_SIZE);
}
