// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

// Needs to be defined before any includes.
#define QT_STATICPLUGIN

#include "themesvgiconengine.h"
#include "qtutils.h"

#include <QtCore/QFile>
#include <QtCore/QtPlugin>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPalette>
#include <QtGui/QPixmapCache>
#include <QtWidgets/QApplication>

#include <limits>
#include <plutosvg.h>

#include "moc_themesvgiconengine.cpp"

/// Returns the icon color for the given mode based on the application palette.
static QColor GetIconColorFromPalette(QIcon::Mode mode)
{
  // Thank gosh these are copy-on-write...
  const QPalette palette = QApplication::palette();

  // NOTE: Active and Normal are the same in QPalette.
  if (mode == QIcon::Disabled)
    return palette.color(QPalette::Disabled, QPalette::WindowText);
  else if (mode == QIcon::Selected)
    return palette.color(QPalette::Current, QPalette::HighlightedText);
  else
    return palette.color(QPalette::Normal, QPalette::WindowText);
}

/// Constructs a cache key for the given resource path, pixel size, and RGBA color value.
static QString BuildCacheKey(const QString& resource_path, const QSize& size, qreal dpr, const QColor& color)
{
  // Cache key includes path, size, and colour so stale entries are not reused after a palette change.
  return QStringLiteral("%1_%2x%3@%4_%5")
    .arg(resource_path)
    .arg(size.width())
    .arg(size.height())
    .arg(dpr)
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
  const plutovg_color_t current_color = {
    .r = static_cast<float>(color.redF()),
    .g = static_cast<float>(color.greenF()),
    .b = static_cast<float>(color.blueF()),
    .a = static_cast<float>(color.alphaF()),
  };

  plutovg_surface_t* surface =
    plutosvg_document_render_to_surface(doc, nullptr, size.width(), size.height(), &current_color, nullptr, nullptr);
  if (!surface)
    return false;

  // plutovg surfaces are premultiplied ARGB (0xAARRGGBB, native-endian), which matches
  // QImage::Format_ARGB32_Premultiplied exactly, no pixel conversion required.
  // The cleanup function ensures the surface is destroyed when the QImage is done with the pixel data.
  const QImage img(plutovg_surface_get_data(surface), plutovg_surface_get_width(surface),
                   plutovg_surface_get_height(surface), plutovg_surface_get_stride(surface),
                   QImage::Format_ARGB32_Premultiplied, CleanupPlutoSVGSurface, surface);

  pm = QPixmap::fromImage(img);
  return !pm.isNull();
}

/// Helper function to read a QFile to a DynamicHeapArray<u8>.
static bool ReadFileToByteArray(QIODevice* dev, DynamicHeapArray<u8>& out_data)
{
  if (qint64 size; !dev->isSequential() && (size = dev->size()) > 0)
  {
    out_data.resize(static_cast<size_t>(
      (sizeof(size_t) == sizeof(qint64)) ? size : std::min<qint64>(size, std::numeric_limits<size_t>::max())));

    if (dev->read(reinterpret_cast<char*>(out_data.data()), size) != size)
    {
      out_data.deallocate();
      return false;
    }
  }
  else
  {
    constexpr size_t chunk_size = 1048576;
    size_t read_so_far = 0;
    for (;;)
    {
      const size_t prev_size = out_data.size();
      const size_t new_size =
        ((read_so_far + chunk_size) < read_so_far) ? std::numeric_limits<size_t>::max() : (read_so_far + chunk_size);
      const size_t space = (new_size - prev_size);
      if (space > 0)
        out_data.resize(new_size);
      const qint64 bytes_read =
        (space > 0) ? dev->read(reinterpret_cast<char*>(out_data.data() + read_so_far), static_cast<qint64>(space)) : 0;
      if (bytes_read < 0)
      {
        out_data.deallocate();
        return false;
      }
      else if (bytes_read == 0)
      {
        out_data.resize(prev_size);
        break;
      }

      read_so_far += static_cast<size_t>(bytes_read);
    }
  }

  return true;
}

ThemeSVGIconEngine::ThemeSVGIconEngine(const QString& resource_path) : m_resource_path(resource_path)
{
}

ThemeSVGIconEngine::~ThemeSVGIconEngine()
{
  if (m_document)
    plutosvg_document_destroy(m_document);
}

bool ThemeSVGIconEngine::ensureLoaded() const
{
  // Previous load failed?
  if (m_resource_path.isEmpty())
    return false;

  QFile file(m_resource_path);
  if (!file.open(QFile::ReadOnly) || !ReadFileToByteArray(&file, m_svg_data))
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

  return true;
}

void ThemeSVGIconEngine::paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state)
{
  Q_UNUSED(state);

  if (rect.isEmpty())
    return;

  // Apply device pixel ratio to requested size so we can cache pixmaps at the correct sizes for different DPRs.
  const QPaintDevice* const device = painter->device();
  const qreal dpr = device ? device->devicePixelRatio() : 1.0;
  const QSize size = QtUtils::ApplyDevicePixelRatioToSize(rect.size(), dpr);
  const QColor color = GetIconColorFromPalette(mode);
  const QString cache_key = BuildCacheKey(m_resource_path, size, dpr, color);

  QPixmap pm;
  if (!QPixmapCache::find(cache_key, &pm))
  {
    // Don't reload multiple times if we hit the cache.
    if (ensureLoaded() && RenderSVGToPixmap(pm, m_document, size, color))
    {
      // DPR set must be before inserting into the cache, otherwise it copies.
      pm.setDevicePixelRatio(dpr);
      QPixmapCache::insert(cache_key, pm);
    }
  }

  painter->drawPixmap(rect, pm);
}

QPixmap ThemeSVGIconEngine::pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state)
{
  Q_UNUSED(state);

  if (size.isEmpty())
    return {};

  const QColor color = GetIconColorFromPalette(mode);
  const QString cache_key = BuildCacheKey(m_resource_path, size, 1.0, color);

  QPixmap pm;
  if (!QPixmapCache::find(cache_key, &pm))
  {
    // Don't reload multiple times if we hit the cache.
    if (ensureLoaded() && RenderSVGToPixmap(pm, m_document, size, color))
      QPixmapCache::insert(cache_key, pm);
  }

  return pm;
}

QPixmap ThemeSVGIconEngine::scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale)
{
  Q_UNUSED(state);

  const QSize scaled_size = QtUtils::ApplyDevicePixelRatioToSize(size, scale);
  if (scaled_size.isEmpty())
    return {};

  const QColor color = GetIconColorFromPalette(mode);
  const QString cache_key = BuildCacheKey(m_resource_path, scaled_size, scale, color);

  QPixmap pm;
  if (!QPixmapCache::find(cache_key, &pm))
  {
    // Don't reload multiple times if we hit the cache.
    if (ensureLoaded() && RenderSVGToPixmap(pm, m_document, scaled_size, color))
    {
      pm.setDevicePixelRatio(scale);
      QPixmapCache::insert(cache_key, pm);
    }
  }

  return pm;
}

QIconEngine* ThemeSVGIconEngine::clone() const
{
  return new ThemeSVGIconEngine(m_resource_path);
}

QString ThemeSVGIconEngine::key() const
{
  return QStringLiteral("svg");
}

QString ThemeSVGIconEngine::iconName()
{
  return m_resource_path;
}

QIconEngine* ThemeSVGIconEnginePlugin::create(const QString& resource_path)
{
  if (resource_path.endsWith(".svg", Qt::CaseInsensitive))
    return new ThemeSVGIconEngine(resource_path);

  return nullptr;
}

bool PlutoSVGImageHandler::canRead() const
{
  if (m_document)
    return m_document;

  // Read all data once and keep it alive; plutosvg_document borrows the raw pointer.
  QIODevice* const dev = device();
  if (!dev || !ReadFileToByteArray(dev, m_svg_data))
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

bool PlutoSVGImageHandler::read(QImage* image)
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

bool PlutoSVGImageHandler::supportsOption(ImageOption option) const
{
  return (option == Size || option == ScaledSize);
}

QVariant PlutoSVGImageHandler::option(ImageOption option) const
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

void PlutoSVGImageHandler::setOption(ImageOption option, const QVariant& value)
{
  if (option == ScaledSize)
    m_scaled_size = value.toSize();
}

QImageIOPlugin::Capabilities PlutoSVGImagePlugin::capabilities(QIODevice* device, const QByteArray& format) const
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

QImageIOHandler* PlutoSVGImagePlugin::create(QIODevice* device, const QByteArray& format) const
{
  PlutoSVGImageHandler* const handler = new PlutoSVGImageHandler();
  handler->setDevice(device);
  handler->setFormat(format.isEmpty() ? QByteArray("svg") : format);
  return handler;
}
