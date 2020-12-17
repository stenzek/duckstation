#include "memoryviewwidget.h"
#include <QtGui/QPainter>
#include <QtWidgets/QScrollBar>
#include <cstring>

MemoryViewWidget::MemoryViewWidget(QWidget* parent /* = nullptr */, size_t address_offset /* = 0 */,
                                   const void* data_ptr /* = nullptr */, size_t data_size /* = 0 */)
  : QAbstractScrollArea(parent)
{
  m_bytes_per_line = 16;

  updateMetrics();

  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &MemoryViewWidget::adjustContent);
  connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &MemoryViewWidget::adjustContent);

  if (data_ptr)
    setData(address_offset, data_ptr, data_size);
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
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
  m_char_width = fm.horizontalAdvance(QChar('0'));
#else
  m_char_width = fm.boundingRect(QChar('0')).width();
#endif
  m_char_height = fm.height();
}

void MemoryViewWidget::setData(size_t address_offset, const void* data_ptr, size_t data_size)
{
  m_data = data_ptr;
  m_data_size = data_size;
  m_address_offset = address_offset;
  adjustContent();
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

void MemoryViewWidget::scrolltoOffset(size_t offset)
{
  const unsigned row = static_cast<unsigned>(offset / m_bytes_per_line);
  verticalScrollBar()->setSliderPosition(static_cast<int>(row));
  horizontalScrollBar()->setSliderPosition(0);
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

void MemoryViewWidget::resizeEvent(QResizeEvent*)
{
  adjustContent();
}

template<typename T>
static bool RangesOverlap(T x1, T x2, T y1, T y2)
{
  return (x2 >= y1 && x1 < y2);
}

void MemoryViewWidget::paintEvent(QPaintEvent*)
{
  QPainter painter(viewport());
  painter.setFont(font());
  if (!m_data)
    return;

  const QColor highlight_color(100, 100, 0);
  const int offsetX = horizontalScrollBar()->value();

  int y = m_char_height;
  QString address;

  painter.setPen(viewport()->palette().color(QPalette::WindowText));

  y += m_char_height;

  const unsigned num_rows = static_cast<unsigned>(m_end_offset - m_start_offset) / m_bytes_per_line;
  for (unsigned row = 0; row <= num_rows; row++)
  {
    const size_t data_offset = m_start_offset + (row * m_bytes_per_line);
    const unsigned row_address = static_cast<unsigned>(m_address_offset + data_offset);
    const int draw_x = m_char_width / 2 - offsetX;
    if (RangesOverlap(data_offset, data_offset + m_bytes_per_line, m_highlight_start, m_highlight_end))
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
      painter.fillRect(x, 0, HEX_CHAR_WIDTH, height(), viewport()->palette().color(QPalette::AlternateBase));

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
      if (offset >= m_highlight_start && offset < m_highlight_end)
        painter.fillRect(x - m_char_width, y - m_char_height + 3, HEX_CHAR_WIDTH, m_char_height, highlight_color);

      painter.drawText(x, y, QString::asprintf("%02X", value));
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
      if (offset >= m_highlight_start && offset < m_highlight_end)
        painter.fillRect(x, y - m_char_height + 3, 2 * m_char_width, m_char_height, highlight_color);

      if (!std::isprint(value))
        value = '.';
      painter.drawText(x, y, static_cast<QChar>(value));
      x += 2 * m_char_width;
    }
    y += m_char_height;
  }
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

  viewport()->update();
}