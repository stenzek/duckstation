// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/heap_array.h"
#include "common/types.h"

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QPixmap>
#include <QtWidgets/QWidget>

struct plutosvg_document;

/**
 * A widget that loads a monochrome SVG file via plutosvg and rasterizes it at the correct device
 * pixel ratio. The SVG is re-rasterized whenever the widget is resized so the image stays crisp
 * at any size. The rendered image is centered inside the widget; if the SVG aspect ratio differs
 * from the widget's aspect ratio the image is letterboxed / pillarboxed with a transparent
 * background.
 */
class SVGWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit SVGWidget(QWidget* parent = nullptr);
  SVGWidget(const QString& resource_path, QWidget* parent = nullptr);
  ~SVGWidget() override;

  const QColor& color() const { return m_color; }
  void setColor(const QColor& color);

  /// Load (or reload) an SVG from the given Qt resource / file path.
  /// Clears any previously loaded document. Triggers a repaint.
  void setSource(const QString& resource_path);

  /// Returns the path that was passed to setSource().
  const QString& source() const { return m_resource_path; }

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void changeEvent(QEvent* event) override;

private:
  /// (Re)render m_document at the current widget size + DPR and store the result in m_pixmap.
  void rasterize();

  /// Free the parsed document and clear associated data.
  void destroyDocument();

  QString m_resource_path;
  DynamicHeapArray<u8> m_svg_data; ///< Raw bytes; plutosvg borrows this pointer.
  plutosvg_document* m_document = nullptr;
  QPixmap m_pixmap;         ///< Last rasterized pixmap (physical pixels, DPR set).
  QSize m_last_raster_size; ///< Physical size used for the last rasterize() call.
  QColor m_color;           ///< Color to use when rasterizing the SVG (overrides currentColor in the SVG).
};