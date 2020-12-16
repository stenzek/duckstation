#pragma once
#include <QtWidgets/QAbstractScrollArea>

// Based on https://stackoverflow.com/questions/46375673/how-can-realize-my-own-memory-viewer-by-qt

class MemoryViewWidget : public QAbstractScrollArea
{
public:
  Q_OBJECT
public:
  MemoryViewWidget(QWidget* parent = nullptr, size_t address_offset = 0, const void* data_ptr = nullptr,
                   size_t data_size = 0);
  ~MemoryViewWidget();

  size_t addressOffset() const { return m_address_offset; }

  void setData(size_t address_offset, const void* data_ptr, size_t data_size);
  void setHighlightRange(size_t start, size_t end);
  void clearHighlightRange();
  void scrolltoOffset(size_t offset);
  void scrollToAddress(size_t address);
  void setFont(const QFont& font);

protected:
  void paintEvent(QPaintEvent*);
  void resizeEvent(QResizeEvent*);

private Q_SLOTS:
  void adjustContent();

private:
  int addressWidth() const;
  int hexWidth() const;
  int asciiWidth() const;
  void updateMetrics();

  const void* m_data;
  size_t m_data_size;
  size_t m_address_offset;

  size_t m_start_offset;
  size_t m_end_offset;

  size_t m_highlight_start = 0;
  size_t m_highlight_end = 0;

  unsigned m_bytes_per_line;

  int m_char_width;
  int m_char_height;

  int m_rows_visible;
};