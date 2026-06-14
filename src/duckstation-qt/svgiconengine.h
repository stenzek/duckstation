// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/heap_array.h"
#include "common/types.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtGui/QColor>
#include <QtGui/QIcon>
#include <QtGui/QIconEngine>
#include <QtGui/QIconEnginePlugin>
#include <QtGui/QImageIOHandler>
#include <QtGui/QImageIOPlugin>

struct plutosvg_document;

/**
 * Custom icon engine that renders SVG icons tinted to the current palette's WindowText colour.
 * This allows a single set of monochrome SVG files to serve both light and dark themes without
 * duplication. The SVG is parsed once into a plutosvg_document and cached on the engine instance.
 * At paint/pixmap time plutosvg renders directly to a premultiplied-ARGB surface using the desired
 * palette colour as CSS currentColor, so no secondary SourceIn compositing pass is required.
 *
 * Pixmaps are cached in QPixmapCache keyed by resource path, size (in device pixels), and the RGBA
 * value of the resolved colour, so the cache is automatically invalidated when the palette changes.
 */
class SVGIconEngine final : public QIconEngine
{
public:
  explicit SVGIconEngine(const QString& resource_path);
  SVGIconEngine(const SVGIconEngine&) = delete;
  ~SVGIconEngine() override;

  void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override;
  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override;
  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override;
  QIconEngine* clone() const override;
  QString key() const override;
  QString iconName() override;

private:
  /// Ensures the SVG file has been loaded.
  bool ensureLoaded() const;

  /// Returns a pixmap for the specified configuration for drawing or caching.
  QPixmap getPixmap(const QSize& size, qreal dpr, QIcon::Mode mode, QIcon::State state);

  mutable QString m_resource_path;
  mutable DynamicHeapArray<u8> m_svg_data;
  mutable plutosvg_document* m_document = nullptr;
  mutable bool m_is_colored = false;
};

class SVGIconEnginePlugin : public QIconEnginePlugin
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID QIconEngineFactoryInterface_iid FILE "svgiconengine.json")

public:
  QIconEngine* create(const QString& resource_path) override;
};

/**
 * Qt image I/O handler — replaces the QtSvg SVG image loader.
 *
 * Registered statically for "svg" so that any Qt code that loads SVG images
 * through QImageReader (e.g. QPixmap::load, QImage::load) will use plutosvg
 * instead of QtSvg's built-in rasteriser.
 *
 * Callers can influence the output size through the standard QImageIOHandler
 * ScaledSize option; if not set the SVG's intrinsic dimensions are used.
 */
class SVGImageHandler final : public QImageIOHandler
{
public:
  SVGImageHandler();
  ~SVGImageHandler() override;

  /// Lazily load and parse the SVG data from device(). Returns false on failure.
  bool canRead() const override;
  bool read(QImage* image) override;

  bool supportsOption(ImageOption option) const override;
  QVariant option(ImageOption option) const override;
  void setOption(ImageOption option, const QVariant& value) override;

  static bool canRead(QIODevice* device);

private:
  mutable DynamicHeapArray<u8> m_svg_data;
  mutable plutosvg_document* m_document = nullptr;
  QSize m_scaled_size;
};

/// QImageIOPlugin that installs PlutoSVGImageHandler for SVG files.
class SVGImageHandlerPlugin final : public QImageIOPlugin
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID QImageIOHandlerFactoryInterface_iid FILE "svgiconengine.json")

public:
  Capabilities capabilities(QIODevice* device, const QByteArray& format) const override;
  QImageIOHandler* create(QIODevice* device, const QByteArray& format = QByteArray()) const override;
};
