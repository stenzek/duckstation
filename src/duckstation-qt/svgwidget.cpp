// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "svgwidget.h"
#include "qtutils.h"

#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>

#include <limits>
#include <plutosvg.h>

#include "moc_svgwidget.cpp"

static void CleanupSurface(void* surface)
{
  plutovg_surface_destroy(static_cast<plutovg_surface_t*>(surface));
}

SVGWidget::SVGWidget(QWidget* parent) : QWidget(parent)
{
  setAttribute(Qt::WA_OpaquePaintEvent, false);
}

SVGWidget::SVGWidget(const QString& resource_path, QWidget* parent /*= nullptr*/) : SVGWidget(parent)
{
  setSource(resource_path);
}

SVGWidget::~SVGWidget()
{
  destroyDocument();
}

void SVGWidget::setColor(const QColor& color)
{
  m_pixmap = QPixmap();
  rasterize();
  update();
}

void SVGWidget::setSource(const QString& resource_path)
{
  destroyDocument();
  m_resource_path = resource_path;
  m_pixmap = {};
  m_last_raster_size = {};

  if (resource_path.isEmpty())
  {
    update();
    return;
  }

  QFile file(m_resource_path);
  if (!file.open(QFile::ReadOnly) || !QtUtils::ReadFileToByteArray(&file, m_svg_data))
  {
    qCritical() << "Failed to open SVG file: " << m_resource_path;
    update();
    return;
  }

  // plutosvg borrows the raw pointer; m_svg_data must outlive m_document.
  m_document = plutosvg_document_load_from_data(reinterpret_cast<const char*>(m_svg_data.data()),
                                                static_cast<int>(m_svg_data.size()), -1.0f, -1.0f, nullptr, nullptr);
  if (!m_document)
  {
    qCritical() << "PlutoSVGWidget: failed to parse SVG" << resource_path;
    m_svg_data.deallocate();
    m_resource_path.clear();
    update();
    return;
  }

  rasterize();
  update();
}

void SVGWidget::destroyDocument()
{
  if (m_document)
  {
    plutosvg_document_destroy(m_document);
    m_document = nullptr;
  }
  m_svg_data.deallocate();
}

void SVGWidget::rasterize()
{
  m_pixmap = {};

  if (!m_document || size().isEmpty())
    return;

  const qreal dpr = devicePixelRatioF();

  // Physical pixel dimensions to render at.
  const QSize physical = QtUtils::ApplyDevicePixelRatioToSize(size(), dpr);

  // Avoid redundant re-renders when size hasn't actually changed.
  if (physical == m_last_raster_size && !m_pixmap.isNull())
    return;

  m_last_raster_size = physical;

  // Determine SVG intrinsic size and compute a uniform scale that fits inside physical,
  // preserving aspect ratio.
  const float svg_w = plutosvg_document_get_width(m_document);
  const float svg_h = plutosvg_document_get_height(m_document);

  int render_w = physical.width();
  int render_h = physical.height();

  if (svg_w > 0.0f && svg_h > 0.0f)
  {
    // Scale to fit, preserving aspect ratio (letterbox / pillarbox).
    const float scale_x = static_cast<float>(physical.width()) / svg_w;
    const float scale_y = static_cast<float>(physical.height()) / svg_h;
    const float scale = std::min(scale_x, scale_y);
    render_w = std::max(1, static_cast<int>(svg_w * scale));
    render_h = std::max(1, static_cast<int>(svg_h * scale));
  }

  // Use white as currentColor so the SVG's own colours are preserved.
  // Swap in a palette colour here if tinting is ever desired.
  const plutovg_color_t current_color = {.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f};

  plutovg_surface_t* const surface =
    plutosvg_document_render_to_surface(m_document, nullptr, render_w, render_h, &current_color, nullptr, nullptr);
  if (!surface)
  {
    qCritical() << "PlutoSVGWidget: render failed for" << m_resource_path;
    return;
  }

  // plutovg surfaces are premultiplied ARGB (native-endian) == QImage::Format_ARGB32_Premultiplied.
  const QImage img(plutovg_surface_get_data(surface), plutovg_surface_get_width(surface),
                   plutovg_surface_get_height(surface), plutovg_surface_get_stride(surface),
                   QImage::Format_ARGB32_Premultiplied, CleanupSurface, surface);

  m_pixmap = QPixmap::fromImage(img);
  m_pixmap.setDevicePixelRatio(dpr);
}

void SVGWidget::paintEvent(QPaintEvent* event)
{
  Q_UNUSED(event);

  QPainter painter(this);

  if (m_pixmap.isNull())
    return;

  // Center the (possibly smaller, aspect-ratio-corrected) pixmap inside the widget.
  // m_pixmap.size() is in physical pixels; divide by DPR to get logical pixels.
  const QSizeF logical_size = m_pixmap.size() / m_pixmap.devicePixelRatioF();
  const QPointF top_left((width() - logical_size.width()) / 2.0, (height() - logical_size.height()) / 2.0);

  painter.drawPixmap(QRectF(top_left, logical_size), m_pixmap, QRectF(QPointF(0, 0), m_pixmap.size()));
}

void SVGWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  rasterize();
}

void SVGWidget::changeEvent(QEvent* event)
{
  QWidget::changeEvent(event);

  // Re-rasterize when the screen changes (e.g. window moved to a display with a different DPR).
  if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal)
  {
    m_last_raster_size = {}; // force re-render
    rasterize();
    update();
  }
}

QPixmap SVGWidget::renderSVGToPixmap(const QString& resource_path, const QSize& size, qreal device_pixel_ratio,
                                     const QColor& color)
{
  DynamicHeapArray<u8> svg_data;
  QFile file(resource_path);
  if (!file.open(QFile::ReadOnly) || !QtUtils::ReadFileToByteArray(&file, svg_data))
  {
    qCritical() << "Failed to open SVG file: " << resource_path;
    return {};
  }

  // plutosvg borrows the raw pointer; svg_data must outlive m_document.
  plutosvg_document* const document = plutosvg_document_load_from_data(
    reinterpret_cast<const char*>(svg_data.data()), static_cast<int>(svg_data.size()), -1.0f, -1.0f, nullptr, nullptr);
  if (!document)
  {
    qCritical() << "PlutoSVGWidget: failed to parse SVG" << resource_path;
    return {};
  }

  // Determine SVG intrinsic size and compute a uniform scale that fits inside physical,
  // preserving aspect ratio.
  const float svg_w = plutosvg_document_get_width(document);
  const float svg_h = plutosvg_document_get_height(document);

  const QSize physical = QtUtils::ApplyDevicePixelRatioToSize(size, device_pixel_ratio);
  int render_w = physical.width();
  int render_h = physical.height();

  if (svg_w > 0.0f && svg_h > 0.0f)
  {
    // Scale to fit, preserving aspect ratio (letterbox / pillarbox).
    const float scale_x = static_cast<float>(physical.width()) / svg_w;
    const float scale_y = static_cast<float>(physical.height()) / svg_h;
    const float scale = std::min(scale_x, scale_y);
    render_w = std::max(1, static_cast<int>(svg_w * scale));
    render_h = std::max(1, static_cast<int>(svg_h * scale));
  }

  // Use white as currentColor so the SVG's own colours are preserved.
  // Swap in a palette colour here if tinting is ever desired.
  const plutovg_color_t current_color = {.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f};

  plutovg_surface_t* const surface =
    plutosvg_document_render_to_surface(document, nullptr, render_w, render_h, &current_color, nullptr, nullptr);
  if (!surface)
  {
    qCritical() << "PlutoSVGWidget: render failed for" << resource_path;
    plutosvg_document_destroy(document);
    return {};
  }

  // plutovg surfaces are premultiplied ARGB (native-endian) == QImage::Format_ARGB32_Premultiplied.
  const QImage img(plutovg_surface_get_data(surface), plutovg_surface_get_width(surface),
                   plutovg_surface_get_height(surface), plutovg_surface_get_stride(surface),
                   QImage::Format_ARGB32_Premultiplied, CleanupSurface, surface);

  QPixmap pm = QPixmap::fromImage(img);
  pm.setDevicePixelRatio(device_pixel_ratio);
  plutosvg_document_destroy(document);
  return pm;
}
