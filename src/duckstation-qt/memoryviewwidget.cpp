#include "memoryviewwidget.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QScrollBar>
#include <cstring>

MemoryViewWidget::MemoryViewWidget(QWidget* parent /* = nullptr */, size_t address_offset /* = 0 */,
                                   void* data_ptr /* = nullptr */, size_t data_size /* = 0 */,
                                   bool data_editable /* = false */, EditCallback edit_callback /* = nullptr */)
  : QAbstractScrollArea(parent)
{
  m_bytes_per_line = 16;

  updateMetrics();

  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &MemoryViewWidget::adjustContent);
  connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &MemoryViewWidget::adjustContent);

  if (data_ptr)
    setData(address_offset, data_ptr, data_size, data_editable, edit_callback);
}

MemoryViewWidget::~MemoryViewWidget() = default;

int MemoryViewWidget::addressWidth() const
{
  return (8 * m_char_width) + m_char_width;
}

int MemoryViewWidget::hexWidth() const
{
  return (m_bytes_per_line * 4) * m_char_width;
}

int MemoryViewWidget::asciiWidth() const
{
  return (m_bytes_per_line * 2 + 1) * m_char_width;
}

void MemoryViewWidget::updateMetrics()
{
  const QFontMetrics fm(fontMetrics());
  m_char_width = fm.horizontalAdvance(QChar('0'));
  m_char_height = fm.height();
}

void MemoryViewWidget::setData(size_t address_offset, void* data_ptr, size_t data_size, bool data_editable,
                               EditCallback edit_callback)
{
  m_data = data_ptr;
  m_data_size = data_size;
  m_data_editable = data_editable;
  m_address_offset = address_offset;
  m_selected_address = INVALID_SELECTED_ADDRESS;
  m_edit_callback = edit_callback;
  m_last_data_start_offset = 0;
  m_last_data.clear();
  adjustContent();
  saveCurrentData();
}

void MemoryViewWidget::setHighlightRange(size_t start, size_t end)
{
  m_highlight_start = start;
  m_highlight_end = end;
  viewport()->update();
}

void MemoryViewWidget::clearHighlightRange()
{
  m_highlight_start = 0;
  m_highlight_end = 0;
  viewport()->update();
}

void MemoryViewWidget::scrollToOffset(size_t offset, bool select /* = true */)
{
  const unsigned row = static_cast<unsigned>(offset / m_bytes_per_line);
  verticalScrollBar()->setSliderPosition(static_cast<int>(row));
  horizontalScrollBar()->setSliderPosition(0);
  if (select)
    setSelection(offset, false);
}

void MemoryViewWidget::scrollToAddress(size_t address)
{
  const unsigned row = static_cast<unsigned>((address - m_start_offset) / m_bytes_per_line);
  verticalScrollBar()->setSliderPosition(static_cast<int>(row));
  horizontalScrollBar()->setSliderPosition(0);
}

void MemoryViewWidget::setFont(const QFont& font)
{
  QAbstractScrollArea::setFont(font);
  updateMetrics();
}

void MemoryViewWidget::resizeEvent(QResizeEvent* event)
{
  adjustContent();
}

void MemoryViewWidget::mousePressEvent(QMouseEvent* event)
{
  if ((event->buttons() & Qt::LeftButton) != 0)
    updateSelectedByte(event->pos());

  QAbstractScrollArea::mousePressEvent(event);
}

void MemoryViewWidget::mouseMoveEvent(QMouseEvent* event)
{
  if ((event->buttons() & Qt::LeftButton) != 0)
    updateSelectedByte(event->pos());

  QAbstractScrollArea::mouseMoveEvent(event);
}

void MemoryViewWidget::keyPressEvent(QKeyEvent* event)
{
  if (m_selected_address < m_data_size && m_data_editable)
  {
    const int key = event->key();
    if (key == Qt::Key_Backspace)
    {
      if (m_selected_address > 0)
      {
        m_selected_address--;
        m_editing_nibble = -1;
        forceRefresh();
      }
    }
    else
    {
      const QString key_text = event->text();
      if (key_text.length() == 1)
      {
        const char ch = key_text[0].toLatin1();
        if (m_selection_was_ascii)
        {
          expandCurrentDataToInclude(m_selected_address);

          unsigned char* pdata = static_cast<unsigned char*>(m_data) + m_selected_address;
          if (static_cast<unsigned char>(ch) != *pdata)
          {
            *pdata = static_cast<unsigned char>(ch);
            if (m_edit_callback)
              m_edit_callback(m_selected_address, 1);
          }

          m_selected_address = std::min(m_selected_address + 1, m_data_size - 1);
          forceRefresh();
        }
        else
        {
          int nibble = -1;
          if (ch >= 'a' && ch <= 'f')
            nibble = ch - 'a' + 0xa;
          else if (ch >= 'A' && ch <= 'F')
            nibble = ch - 'A' + 0xa;
          else if (ch >= '0' && ch <= '9')
            nibble = ch - '0';
          if (nibble >= 0)
          {
            m_editing_nibble++;

            expandCurrentDataToInclude(m_selected_address);

            unsigned char* pdata = static_cast<unsigned char*>(m_data) + m_selected_address;
            const unsigned char new_value =
              (*pdata & ~(0xf0 >> (m_editing_nibble * 4))) | (nibble << ((1 - m_editing_nibble) * 4));
            if (*pdata != new_value)
            {
              *pdata = new_value;
              if (m_edit_callback)
                m_edit_callback(m_selected_address, 1);
            }

            if (m_editing_nibble == 1)
            {
              m_editing_nibble = -1;
              m_selected_address = std::min(m_selected_address + 1, m_data_size - 1);
            }

            forceRefresh();
          }
        }
      }
    }
  }

  QAbstractScrollArea::keyPressEvent(event);
}

template<typename T>
static bool RangesOverlap(T x1, T x2, T y1, T y2)
{
  return (x2 >= y1 && x1 < y2);
}

void MemoryViewWidget::paintEvent(QPaintEvent* event)
{
  QPainter painter(viewport());
  painter.setFont(font());
  if (!m_data)
    return;

  const QPalette palette = viewport()->palette();
  const bool dark = palette.windowText().color().value() > palette.window().color().value();
  const QColor alt_fill_color =
    dark ? palette.color(QPalette::AlternateBase).darker(130) : palette.color(QPalette::AlternateBase).lighter(100);
  const QColor selected_color = dark ? palette.color(QPalette::Highlight) : QColor(190, 190, 190);
  const QColor text_color = palette.color(QPalette::WindowText);
  const QColor highlight_color(100, 100, 0);
  const QColor edited_color(240, 30, 30);
  const int offsetX = horizontalScrollBar()->value();

  int y = m_char_height;
  QString address;

  painter.setPen(text_color);

  y += m_char_height;

  const unsigned num_rows = static_cast<unsigned>(m_end_offset - m_start_offset) / m_bytes_per_line;
  for (unsigned row = 0; row <= num_rows; row++)
  {
    const size_t data_offset = m_start_offset + (row * m_bytes_per_line);
    const unsigned row_address = static_cast<unsigned>(m_address_offset + data_offset);
    const int draw_x = m_char_width / 2 - offsetX;
    if (RangesOverlap(data_offset, data_offset + m_bytes_per_line, m_selected_address, m_selected_address + 1))
      painter.fillRect(0, y - m_char_height + 3, addressWidth(), m_char_height, selected_color);
    else if (RangesOverlap(data_offset, data_offset + m_bytes_per_line, m_highlight_start, m_highlight_end))
      painter.fillRect(0, y - m_char_height + 3, addressWidth(), m_char_height, highlight_color);

    const QString address_text(QString::asprintf("%08X", row_address));
    painter.drawText(draw_x, y, address_text);
    y += m_char_height;
  }

  int x;
  int lx = addressWidth();
  painter.drawLine(lx - offsetX, 0, lx - offsetX, height());
  y = m_char_height;

  // hex data
  const int HEX_CHAR_WIDTH = 4 * m_char_width;

  x = lx - offsetX;
  for (unsigned col = 0; col < m_bytes_per_line; col++)
  {
    if ((col % 2) != 0)
      painter.fillRect(x, 0, HEX_CHAR_WIDTH, height(), alt_fill_color);

    x += HEX_CHAR_WIDTH;
  }

  y = m_char_height;
  x = lx - offsetX + m_char_width;
  for (unsigned col = 0; col < m_bytes_per_line; col++)
  {
    painter.drawText(x, y, QString::asprintf("%02X", col));
    x += HEX_CHAR_WIDTH;
  }

  painter.drawLine(0, y + 3, width(), y + 3);
  y += m_char_height;

  size_t offset = m_start_offset;
  for (unsigned row = 0; row <= num_rows; row++)
  {
    x = lx - offsetX + m_char_width;
    for (unsigned col = 0; col < m_bytes_per_line && offset < m_data_size; col++, offset++)
    {
      unsigned char value;
      std::memcpy(&value, static_cast<const unsigned char*>(m_data) + offset, sizeof(value));
      if (m_selected_address == offset)
        painter.fillRect(x - m_char_width, y - m_char_height + 4, HEX_CHAR_WIDTH, m_char_height, selected_color);
      else if (offset >= m_highlight_start && offset < m_highlight_end)
        painter.fillRect(x - m_char_width, y - m_char_height + 4, HEX_CHAR_WIDTH, m_char_height, highlight_color);

      if (m_selected_address != offset || m_editing_nibble != 0 || m_selection_was_ascii) [[likely]]
      {
        const QString text = QString::asprintf("%02X", value);
        const size_t view_offset = offset - m_last_data_start_offset; // may underflow, but unsigned so it's okay
        if (view_offset < m_last_data.size() && value != m_last_data[view_offset])
        {
          painter.setPen(edited_color);
          painter.drawText(x, y, text);
          painter.setPen(text_color);
        }
        else
        {
          painter.drawText(x, y, text);
        }
      }
      else
      {
        const QString high = QString::asprintf("%X", value >> 4);
        const QRect low_rc = painter.boundingRect(x, y, HEX_CHAR_WIDTH, m_char_height, 0, high);
        painter.setPen(edited_color);
        painter.drawText(x, y, high);
        painter.setPen(text_color);
        painter.drawText(x + low_rc.width(), y, QString::asprintf("%X", (value & 0xF)));
      }

      x += HEX_CHAR_WIDTH;
    }
    y += m_char_height;
  }

  lx = addressWidth() + hexWidth();
  painter.drawLine(lx - offsetX, 0, lx - offsetX, height());

  lx += m_char_width;

  y = m_char_height;
  x = (lx - offsetX);
  for (unsigned col = 0; col < m_bytes_per_line; col++)
  {
    const QChar ch = (col < 0xA) ? (static_cast<QChar>('0' + col)) : (static_cast<QChar>('A' + (col - 0xA)));
    painter.drawText(x, y, ch);
    x += 2 * m_char_width;
  }

  y += m_char_height;

  offset = m_start_offset;
  for (unsigned row = 0; row <= num_rows; row++)
  {
    x = lx - offsetX;
    for (unsigned col = 0; col < m_bytes_per_line && offset < m_data_size; col++, offset++)
    {
      unsigned char value;
      std::memcpy(&value, static_cast<const unsigned char*>(m_data) + offset, sizeof(value));
      if (m_selected_address == offset)
        painter.fillRect(x, y - m_char_height + 4, 2 * m_char_width, m_char_height, selected_color);
      else if (offset >= m_highlight_start && offset < m_highlight_end)
        painter.fillRect(x, y - m_char_height + 4, 2 * m_char_width, m_char_height, highlight_color);

      const QChar print_char = std::isprint(value) ? static_cast<QChar>(value) : static_cast<QChar>('.');
      const size_t view_offset = offset - m_last_data_start_offset; // may underflow, but unsigned so it's okay
      if (view_offset < m_last_data.size() && value != m_last_data[view_offset])
      {
        painter.setPen(edited_color);
        painter.drawText(x, y, print_char);
        painter.setPen(text_color);
      }
      else
      {
        painter.drawText(x, y, print_char);
      }
      x += 2 * m_char_width;
    }
    y += m_char_height;
  }
}

void MemoryViewWidget::updateSelectedByte(const QPoint& pos)
{
  const int xpos = pos.x() + horizontalScrollBar()->value();
  const int ypos = pos.y();

  size_t new_selection = INVALID_SELECTED_ADDRESS;
  bool new_ascii = false;

  // to left or above hex view
  const int addr_width = addressWidth();
  if (xpos >= addr_width && ypos >= m_char_height)
  {
    const int row = (ypos - m_char_height) / m_char_height;
    const size_t row_address = m_start_offset + (static_cast<unsigned>(row) * m_bytes_per_line);

    // out of Y range
    if (row_address < m_end_offset)
    {
      // in hex view?
      const int hex_end = addr_width + hexWidth() + m_char_width;
      if (xpos < hex_end)
      {
        const int hex_char_width = 4 * m_char_width;
        const int hex_offset = (xpos - addr_width) / hex_char_width;
        new_selection = row_address + static_cast<size_t>(hex_offset);
      }
      else
      {
        // in ascii view?
        const int ascii_char_width = 2 * m_char_width;
        const int ascii_end = hex_end + (m_bytes_per_line * ascii_char_width);

        // might be offscreen again
        if (xpos < ascii_end)
        {
          const int ascii_offset = (xpos - hex_end) / ascii_char_width;
          new_selection = row_address + static_cast<size_t>(ascii_offset);
          new_ascii = true;
        }
      }
    }
  }

  setSelection(new_selection, new_ascii);
}

void MemoryViewWidget::setSelection(size_t new_selection, bool new_ascii)
{
  if (new_selection != m_selected_address || new_ascii != m_selection_was_ascii)
  {
    m_selected_address = new_selection;
    m_selection_was_ascii = new_ascii;
    m_editing_nibble = -1;
    forceRefresh();
  }
}

void MemoryViewWidget::expandCurrentDataToInclude(size_t offset)
{
  offset = std::min(offset, m_data_size - 1);
  if (offset < m_last_data_start_offset)
  {
    const size_t add_bytes = m_last_data_start_offset - offset;
    const size_t old_size = m_last_data.size();
    m_last_data.resize(old_size + add_bytes);
    std::memmove(&m_last_data[add_bytes], &m_last_data[0], old_size);
    std::memcpy(&m_last_data[0], static_cast<const u8*>(m_data) + offset, add_bytes);
    m_last_data_start_offset = offset;
  }
  else if (offset >= (m_last_data_start_offset + m_last_data.size()))
  {
    const size_t new_size = m_last_data_start_offset + offset + 1;
    const size_t old_size = m_last_data.size();
    m_last_data.resize(new_size);
    std::memcpy(&m_last_data[old_size], static_cast<const u8*>(m_data) + m_last_data_start_offset + old_size,
                new_size - old_size);
  }
}

void MemoryViewWidget::saveCurrentData()
{
  if (!m_data)
  {
    m_last_data.clear();
    return;
  }

  const size_t size = m_end_offset - m_start_offset;
  m_last_data_start_offset = m_start_offset;
  m_last_data.resize(size);
  std::memcpy(m_last_data.data(), static_cast<const u8*>(m_data) + m_start_offset, size);
  forceRefresh();
}

void MemoryViewWidget::forceRefresh()
{
  viewport()->update();
}

void MemoryViewWidget::adjustContent()
{
  if (!m_data)
  {
    setEnabled(false);
    return;
  }

  setEnabled(true);

  int w = addressWidth() + hexWidth() + asciiWidth();
  horizontalScrollBar()->setRange(0, w - viewport()->width());
  horizontalScrollBar()->setPageStep(viewport()->width());

  m_rows_visible = viewport()->height() / m_char_height;
  int val = verticalScrollBar()->value();
  m_start_offset = (size_t)val * m_bytes_per_line;
  m_end_offset = m_start_offset + m_rows_visible * m_bytes_per_line - 1;
  if (m_end_offset >= m_data_size)
    m_end_offset = m_data_size - 1;

  const int lineCount = static_cast<int>(m_data_size / m_bytes_per_line);
  verticalScrollBar()->setRange(0, lineCount - m_rows_visible);
  verticalScrollBar()->setPageStep(m_rows_visible);

  expandCurrentDataToInclude(m_start_offset);
  expandCurrentDataToInclude(m_end_offset);

  forceRefresh();
}