// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "asyncpixmaploader.h"
#include "qtutils.h"

#include "core/host.h"

#include "util/http_cache.h"
#include "util/object_archive.h"

#include "common/error.h"
#include "common/heap_array.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include "moc_asyncpixmaploader.cpp"

LOG_CHANNEL(Host);

AsyncPixmapLoader::AsyncPixmapLoader(QObject* parent /*= nullptr*/) : QObject(parent)
{
}

AsyncPixmapLoader::~AsyncPixmapLoader() = default;

bool AsyncPixmapLoader::isQueueNeeded(std::string_view url_or_path)
{
  if (!HTTPCache::IsHTTPURL(url_or_path))
    return false;

  // Don't try to async load when we don't have cache.
  const auto cache = HTTPCache::GetCacheArchive();
  return (cache->IsOpen() && !cache->Contains(url_or_path));
}

static std::string_view GetExtensionFromURL(std::string_view url)
{
  return Path::GetExtension(HTTPCache::GetURLFilename(url));
}

static QPixmap LoadPixmapFromSpan(std::string_view extension, std::span<const u8> data)
{
  QPixmap ret;
  TinyString extension_cstr(extension);
  if (!ret.loadFromData(data.data(), static_cast<uint>(data.size()),
                        extension_cstr.empty() ? nullptr : extension_cstr.c_str()))
  {
    ERROR_LOG("Failed to load pixmap from data for extension '{}'", extension);
  }

  return ret;
}

QPixmap AsyncPixmapLoader::load(std::string_view url_or_path)
{
  if (!HTTPCache::IsHTTPURL(url_or_path))
    return QPixmap(QtUtils::StringViewToQString(url_or_path));

  Error error;
  const HTTPCache::LookupResult result = HTTPCache::Lookup(url_or_path, &error);
  if (result.status() != HTTPCache::LookupStatus::Hit)
  {
    ERROR_LOG("Failed to lookup pixmap for URL '{}': {}", url_or_path, error.GetDescription());
    return QPixmap();
  }

  return LoadPixmapFromSpan(GetExtensionFromURL(url_or_path), result->cspan());
}

void AsyncPixmapLoader::enqueue(std::string_view url_or_path)
{
  if (!HTTPCache::IsHTTPURL(url_or_path))
  {
    QPixmap pm(QtUtils::StringViewToQString(url_or_path));
    emit pixmapLoaded(pm);
    deleteLater();
    return;
  }

  Error error;
  HTTPCache::LookupResult result = HTTPCache::Lookup(url_or_path, &error);

  // gotta load it, this is the yuck part.
  if (result.status() == HTTPCache::LookupStatus::Miss)
  {
    m_extension = GetExtensionFromURL(url_or_path);
    result = HTTPCache::LookupOrFetch(url_or_path, &error, [this](std::span<const u8> data) mutable {
      if (!data.empty())
        m_data.assign(data);
      QMetaObject::invokeMethod(this, &AsyncPixmapLoader::finishLoad, Qt::QueuedConnection);
    });
  }

  if (result.status() == HTTPCache::LookupStatus::Hit)
  {
    QPixmap pm = LoadPixmapFromSpan(GetExtensionFromURL(url_or_path), result->cspan());
    emit pixmapLoaded(pm);
    deleteLater();
    return;
  }
  else if (result.status() != HTTPCache::LookupStatus::Miss)
  {
    ERROR_LOG("Failed to lookup pixmap for URL '{}': {}", url_or_path, error.GetDescription());
    QPixmap pm;
    emit pixmapLoaded(pm);
    deleteLater();
    return;
  }
}

void AsyncPixmapLoader::finishLoad()
{
  QPixmap pm = LoadPixmapFromSpan(m_extension, m_data);
  emit pixmapLoaded(pm);
  deleteLater();
}
