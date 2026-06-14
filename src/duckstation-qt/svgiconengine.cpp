// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Needs to be defined before any includes.
#define QT_STATICPLUGIN

#include "svgiconengine.h"
#include "qtutils.h"

#include <QtCore/QFile>
#include <QtCore/QtPlugin>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmapCache>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOption>

#include <cmath>
#include <limits>
#include <plutosvg.h>

#include "moc_svgiconengine.cpp"

/// Returns the icon color for the given mode and state based on the application palette.
static QColor GetIconColorFromPalette(QIcon::Mode mode, QIcon::State state)
{
  // Thank gosh these are copy-on-write...
  const QPalette palette = QApplication::palette();

  // For the "On" state (e.g., checked icons)
  // For the "Off" state (normal/unchecked)
  // NOTE: Active and Normal are the same in QPalette.
  const QPalette::ColorGroup cgroup =
    (mode == QIcon::Disabled) ? QPalette::Disabled : ((mode == QIcon::Selected) ? QPalette::Current : QPalette::Normal);
  const QPalette::ColorRole crole = (state == QIcon::On) ? QPalette::ButtonText : QPalette::WindowText;
  return palette.color(cgroup, crole);
}

/// Constructs a cache key for the given resource path, pixel size, and RGBA color value.
static QString BuildCacheKey(const QString& resource_path, const QSize& size, qreal dpr, QIcon::Mode mode,
                             const QColor& color)
{
  // Cache key includes path, size, and colour so stale entries are not reused after a palette change.
  return QStringLiteral("%1_%2x%3@%4%5_%6")
    .arg(resource_path)
    .arg(size.width())
    .arg(size.height())
    .arg(dpr)
    .arg(static_cast<u8>(mode))
    .arg(static_cast<unsigned>(color.rgba()), 8, 16, QLatin1Char('0'));
}

/// Callback used when freeing the QImage.
static void CleanupPlutoSVGSurface(void* surface)
{
  plutovg_surface_destroy(static_cast<plutovg_surface_t*>(surface));
}

/// Renders the document at the given pixel dimensions with the given colour and inserts the
/// result into QPixmapCache under cache_key. Returns a null QPixmap on failure.
static bool RenderSVGToPixmap(QPixmap& pm, const plutosvg_document* doc, const QSize& size, const QColor& color)
{
  // Pass the desired icon colour as CSS currentColor so plutosvg tints the monochrome SVG in a
  // single render pass, avoiding a separate SourceIn compositing step.
  const plutovg_color_t current_color = color.isValid() ?
                                          plutovg_color_t{
                                            .r = static_cast<float>(color.redF()),
                                            .g = static_cast<float>(color.greenF()),
                                            .b = static_cast<float>(color.blueF()),
                                            .a = static_cast<float>(color.alphaF()),
                                          } :
                                          plutovg_color_t{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f};

  // Determine SVG intrinsic size and compute a uniform scale that fits inside physical,
  // preserving aspect ratio.
  const float svg_w = plutosvg_document_get_width(doc);
  const float svg_h = plutosvg_document_get_height(doc);
  QSize render_size = size;
  if (svg_w > 0.0f && svg_h > 0.0f)
    render_size = QSizeF(svg_w, svg_h).scaled(size, Qt::KeepAspectRatio).toSize();

  plutovg_surface_t* surface = plutosvg_document_render_to_surface(
    doc, nullptr, render_size.width(), render_size.height(), &current_color, nullptr, nullptr);
  if (!surface)
    return false;

  // plutovg surfaces are premultiplied ARGB (0xAARRGGBB, native-endian), which matches
  // QImage::Format_ARGB32_Premultiplied exactly, no pixel conversion required.
  // The cleanup function ensures the surface is destroyed when the QImage is done with the pixel data.
  const QImage img(plutovg_surface_get_data(surface), plutovg_surface_get_width(surface),
                   plutovg_surface_get_height(surface), plutovg_surface_get_stride(surface),
                   QImage::Format_ARGB32_Premultiplied, CleanupPlutoSVGSurface, surface);

  pm = QPixmap::fromImage(img);
  if (pm.isNull())
    return false;

  // If the rendered size is smaller than the requested size, center it in a transparent pixmap.
  if (render_size != size)
  {
    QPixmap centered_pm(size);
    centered_pm.fill(Qt::transparent);

    QPainter painter(&centered_pm);
    const int offset_x = (size.width() - render_size.width()) / 2;
    const int offset_y = (size.height() - render_size.height()) / 2;
    painter.drawImage(offset_x, offset_y, img);

    pm = std::move(centered_pm);
  }
  else
  {
    pm = QPixmap::fromImage(img);
  }

  return true;
}

SVGIconEngine::SVGIconEngine(const QString& resource_path) : m_resource_path(resource_path)
{
}

SVGIconEngine::~SVGIconEngine()
{
  if (m_document)
    plutosvg_document_destroy(m_document);
}

bool SVGIconEngine::ensureLoaded() const
{
  if (m_document)
    return true;

  // Previous load failed?
  if (m_resource_path.isEmpty())
    return false;

  QFile file(m_resource_path);
  if (!file.open(QFile::ReadOnly) || !QtUtils::ReadFileToByteArray(&file, m_svg_data))
  {
    qCritical() << "Failed to open SVG file: " << m_resource_path;
    m_resource_path = {};
    return false;
  }

  // The document borrows the data pointer; m_svg_data must remain valid for the document's lifetime.
  m_document = plutosvg_document_load_from_data(reinterpret_cast<const char*>(m_svg_data.data()),
                                                static_cast<int>(m_svg_data.size()), -1.0f, -1.0f, nullptr, nullptr);
  if (!m_document)
  {
    qCritical() << "Failed to parse SVG data: " << m_resource_path;
    m_resource_path = {};
    return false;
  }

  // Is this a coloured SVG?
  m_is_colored =
    (std::string_view(reinterpret_cast<const char*>(m_svg_data.data()), m_svg_data.size()).find("currentColor") !=
     std::string_view::npos);

  return true;
}

QPixmap SVGIconEngine::getPixmap(const QSize& size, qreal dpr, QIcon::Mode mode, QIcon::State state)
{
  // Apply device pixel ratio to requested size so we can cache pixmaps at the correct sizes for different DPRs.
  const QSize scaled_size = QtUtils::ApplyDevicePixelRatioToSize(size, dpr);
  const QColor color = GetIconColorFromPalette(mode, state);
  const QString cache_key = BuildCacheKey(m_resource_path, scaled_size, dpr, mode, color);

  QPixmap pm;
  if (!QPixmapCache::find(cache_key, &pm))
  {
    // Don't reload multiple times if we hit the cache.
    if ((m_document || ensureLoaded()) && RenderSVGToPixmap(pm, m_document, scaled_size, color))
    {
      if (!m_is_colored && mode != QIcon::Normal)
      {
        QStyleOption opt;
        opt.palette = QGuiApplication::palette();
        pm = QApplication::style()->generatedIconPixmap(mode, pm, &opt);
      }

      // DPR set must be before inserting into the cache, otherwise it copies.
      pm.setDevicePixelRatio(dpr);
      QPixmapCache::insert(cache_key, pm);
    }
  }

  return pm;
}

void SVGIconEngine::paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state)
{
  if (rect.isEmpty())
    return;

  // Apply device pixel ratio to requested size so we can cache pixmaps at the correct sizes for different DPRs.
  const QPaintDevice* const device = painter->device();
  const qreal dpr = device ? device->devicePixelRatio() : 1.0;
  const QPixmap pm = getPixmap(rect.size(), dpr, mode, state);
  if (!pm.isNull())
    painter->drawPixmap(rect, pm);
}

QPixmap SVGIconEngine::pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state)
{
  if (size.isEmpty())
    return {};

  return getPixmap(size, 1.0, mode, state);
}

QPixmap SVGIconEngine::scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale)
{
  if (size.isEmpty())
    return {};

  return getPixmap(size, scale, mode, state);
}

QIconEngine* SVGIconEngine::clone() const
{
  return new SVGIconEngine(m_resource_path);
}

QString SVGIconEngine::key() const
{
  return QStringLiteral("svg");
}

QString SVGIconEngine::iconName()
{
  return m_resource_path;
}

QIconEngine* SVGIconEnginePlugin::create(const QString& resource_path)
{
  if (resource_path.endsWith(".svg", Qt::CaseInsensitive))
    return new SVGIconEngine(resource_path);

  return nullptr;
}

SVGImageHandler::SVGImageHandler() = default;

SVGImageHandler::~SVGImageHandler()
{
  if (m_document)
    plutosvg_document_destroy(m_document);
}

bool SVGImageHandler::canRead() const
{
  if (m_document)
    return m_document;

  // Read all data once and keep it alive; plutosvg_document borrows the raw pointer.
  QIODevice* const dev = device();
  if (!dev || !QtUtils::ReadFileToByteArray(dev, m_svg_data))
  {
    qCritical() << "Failed to read SVG data from device";
    return false;
  }

  m_document = plutosvg_document_load_from_data(reinterpret_cast<const char*>(m_svg_data.data()),
                                                static_cast<int>(m_svg_data.size()), -1.0f, -1.0f, nullptr, nullptr);
  if (!m_document)
  {
    qCritical() << "Failed to parse SVG data";
    m_svg_data = {};
    return false;
  }

  return true;
}

bool SVGImageHandler::read(QImage* image)
{
  if (!canRead())
    return false;

  // Determine render size: honour caller's ScaledSize, fall back to intrinsic SVG dimensions.
  QSize render_size = m_scaled_size;
  if (!render_size.isValid())
  {
    render_size = QSize(qMax(1, qRound(plutosvg_document_get_width(m_document))),
                        qMax(1, qRound(plutosvg_document_get_height(m_document))));
  }

  // Render with a solid currentColor so the SVG's own fill/stroke colours are preserved.
  // Callers that want palette tinting should use ThemeSVGIconEngine instead.
  const plutovg_color_t current_color = {.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f};

  plutovg_surface_t* surface = plutosvg_document_render_to_surface(
    m_document, nullptr, render_size.width(), render_size.height(), &current_color, nullptr, nullptr);
  if (!surface)
    return false;

  // Wrap the surface pixels in a QImage; the cleanup function destroys the surface once Qt is done.
  // plutovg uses premultiplied ARGB (0xAARRGGBB, native-endian) == QImage::Format_ARGB32_Premultiplied.
  *image =
    QImage(plutovg_surface_get_data(surface), plutovg_surface_get_width(surface), plutovg_surface_get_height(surface),
           plutovg_surface_get_stride(surface), QImage::Format_ARGB32_Premultiplied, CleanupPlutoSVGSurface, surface);

  return !image->isNull();
}

bool SVGImageHandler::supportsOption(ImageOption option) const
{
  return (option == Size || option == ScaledSize);
}

QVariant SVGImageHandler::option(ImageOption option) const
{
  if (option == ScaledSize)
    return m_scaled_size;

  if (option == Size)
  {
    if (!canRead())
      return {};

    return QSize(qMax(1, qRound(plutosvg_document_get_width(m_document))),
                 qMax(1, qRound(plutosvg_document_get_height(m_document))));
  }

  return {};
}

void SVGImageHandler::setOption(ImageOption option, const QVariant& value)
{
  if (option == ScaledSize)
    m_scaled_size = value.toSize();
}

QImageIOPlugin::Capabilities SVGImageHandlerPlugin::capabilities(QIODevice* device, const QByteArray& format) const
{
  if (format == "svg")
    return CanRead;

  if (device)
  {
    // Require the byte sequence to contain an XML or SVG marker within a reasonable prefix.
    const QByteArray prefix = device->peek(256);
    if (prefix.contains("<svg") || prefix.contains("<?xml"))
      return CanRead;
  }

  return {};
}

QImageIOHandler* SVGImageHandlerPlugin::create(QIODevice* device, const QByteArray& format) const
{
  SVGImageHandler* const handler = new SVGImageHandler();
  handler->setDevice(device);
  handler->setFormat(format.isEmpty() ? QByteArray("svg") : format);
  return handler;
}
