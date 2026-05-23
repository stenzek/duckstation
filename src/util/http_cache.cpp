// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "http_cache.h"
#include "http_downloader.h"
#include "object_archive.h"

#include "core/settings.h" // eww

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>

#include <fmt/format.h>

using namespace std::string_view_literals;

LOG_CHANNEL(HTTPCache);

namespace HTTPCache {

static constexpr u32 CACHE_VERSION = 1;

static bool QueueDownload(std::string_view url, FetchCallback callback, Error* error);
static void DownloadCallback(const std::string& url, s32 status_code, const Error& error,
                             const std::string& content_type, const HTTPDownloader::RequestData& data);

namespace {

struct Locals
{
  ObjectArchive cache_archive;
  std::deque<std::pair<std::string, FetchCallback>> pending_downloads;
  std::mutex cache_mutex;
  bool tried_initialize_cache_archive = false;
};

} // namespace

ALIGN_TO_CACHE_LINE static Locals s_locals;

} // namespace HTTPCache

bool HTTPCache::IsHTTPURL(std::string_view url)
{
  // Download HTTP/HTTPS URLs on demand.
  static constexpr const std::array HTTP_DOWNLOAD_URL_PREFIXES = {
    "http://"sv,
    "https://"sv,
  };

  return std::ranges::any_of(HTTP_DOWNLOAD_URL_PREFIXES, [url](const std::string_view& prefix) {
    return StringUtil::StartsWithNoCase(url, prefix);
  });
}

std::string_view HTTPCache::GetURLFilename(std::string_view url)
{
  // Remove any query string or fragment from the URL before extracting the filename.
  const std::string_view::size_type query_pos = url.find_first_of("?#");
  if (query_pos != std::string_view::npos)
    url = url.substr(0, query_pos);

  const std::string_view::size_type pos = url.rfind('/');
  return (pos != std::string_view::npos) ? url.substr(pos + 1) : url;
}

void HTTPCache::Shutdown()
{
  // awkward situation where a request callback could create another downloader...
  HTTPDownloader::CancelRequestsForOwner(&s_locals);

  const std::unique_lock cache_lock(s_locals.cache_mutex);
  for (auto iter = s_locals.pending_downloads.begin(); iter != s_locals.pending_downloads.end();)
  {
    if (iter->second)
      iter->second({});
    iter = s_locals.pending_downloads.erase(iter);
  }

  if (s_locals.cache_archive.IsOpen())
    s_locals.cache_archive.Close();
}

HTTPCache::CacheArchivePtr HTTPCache::GetCacheArchive()
{
  std::unique_lock lock(s_locals.cache_mutex);
  if (!s_locals.cache_archive.IsOpen()) [[unlikely]]
  {
    if (!s_locals.tried_initialize_cache_archive)
    {
      s_locals.tried_initialize_cache_archive = true;

      Error error;
      std::string cache_path = Path::Combine(EmuFolders::Cache, "http_cache");
      if (!s_locals.cache_archive.OpenPath(cache_path, CACHE_VERSION, &error))
        ERROR_LOG("Failed to initialize HTTP cache: {}", error.GetDescription());
    }
  }

  return CacheArchivePtr(std::move(lock), &s_locals.cache_archive);
}

HTTPCache::LookupResult HTTPCache::Lookup(std::string_view url, Error* error)
{
  const auto cache = GetCacheArchive();

  Error lookup_error;
  std::optional<ObjectArchive::ObjectData> image_data = cache->Lookup(url, &lookup_error);
  if (image_data.has_value())
  {
    return LookupResult(LookupStatus::Hit, std::move(*image_data));
  }
  else if (lookup_error.GetDescription() != ObjectArchive::ERROR_DESCRIPTION_DOES_NOT_EXIST) [[unlikely]]
  {
    ERROR_LOG("Failed to read cached texture data for URL '{}': {}", url, lookup_error.GetDescription());
    if (error)
      *error = std::move(lookup_error);

    return LookupResult(LookupStatus::Error);
  }
  else
  {
    return LookupResult(LookupStatus::Miss);
  }
}

HTTPCache::LookupResult HTTPCache::LookupOrFetch(std::string_view url, Error* error, FetchCallback callback)
{
  std::optional<ObjectArchive::ObjectData> image_data;

  const auto cache = GetCacheArchive();

  Error lookup_error;
  image_data = cache->Lookup(url, &lookup_error);
  if (!image_data.has_value() && lookup_error.GetDescription() != ObjectArchive::ERROR_DESCRIPTION_DOES_NOT_EXIST)
    [[unlikely]]
  {
    ERROR_LOG("Failed to read cached texture data for URL '{}': {}", url, lookup_error.GetDescription());
    if (error)
      *error = std::move(lookup_error);

    return LookupResult(LookupStatus::Error);
  }

  // did we find it? return the data directly without invoking the callback
  if (image_data.has_value())
    return LookupResult(LookupStatus::Hit, std::move(*image_data));

  return QueueDownload(url, std::move(callback), error) ? LookupResult(LookupStatus::Miss) :
                                                          LookupResult(LookupStatus::Error);
}

bool HTTPCache::QueueDownload(std::string_view url, FetchCallback callback, Error* error)
{
  // NOTE: Assumes that the lock is held

  // do we already have a request?
  const bool has_request =
    std::ranges::any_of(s_locals.pending_downloads, [url](const auto& pair) { return pair.first == url; });

  // add to pending callbacks so that it will be invoked when the request completes
  // don't add multiple dummy entries for the same url
  if (!has_request || callback)
    s_locals.pending_downloads.emplace_back(url, std::move(callback));

  // don't queue it twice
  if (has_request)
    return true;

  DEV_LOG("Cache miss for URL '{}', downloading...", url);

  HTTPDownloader::CreateRequest(std::string(url), &s_locals,
                                [url = std::string(url)](s32 status_code, Error& error, std::string& content_type,
                                                         HTTPDownloader::RequestData& data) {
                                  DownloadCallback(url, status_code, error, content_type, std::move(data));
                                });

  return true;
}

void HTTPCache::DownloadCallback(const std::string& url, s32 status_code, const Error& error,
                                 const std::string& content_type, const HTTPDownloader::RequestData& data)
{
  const bool success = (status_code == HTTPDownloader::HTTP_STATUS_OK);
  if (!success)
    ERROR_LOG("Failed to download '{}': HTTP status code {}, error: {}", url, status_code, error.GetDescription());

  const auto cache = GetCacheArchive();
  DebugAssert(cache);

  // invoke all callbacks
  for (auto iter = s_locals.pending_downloads.begin(); iter != s_locals.pending_downloads.end();)
  {
    if (iter->first != url)
    {
      ++iter;
      continue;
    }

    if (iter->second)
      iter->second(success ? data : std::span<const u8>());

    iter = s_locals.pending_downloads.erase(iter);
  }

  // don't insert into cache on failure
  if (!success)
    return;

  // NOTE: we're not doing this on a worker thread because if we queue it, another request can come in
  // for the same url, which will re-trigger a download...
  VERBOSE_LOG("Adding URL '{}' to cache ({} bytes)", url, data.size());

  // TODO: only compress if it's images
  Error insert_error;
  if (!cache->Insert(url, data, ObjectArchive::CompressType::Uncompressed, &insert_error))
  {
    if (insert_error.GetDescription() != ObjectArchive::ERROR_DESCRIPTION_ALREADY_EXISTS)
      ERROR_LOG("Failed to insert downloaded data for URL '{}' into cache: {}", url, insert_error.GetDescription());
  }
}

bool HTTPCache::Contains(std::string_view url)
{
  return GetCacheArchive()->Contains(url);
}

void HTTPCache::Prefetch(std::string_view url)
{
  // skip early if already cached, or cannot prefetch
  const auto cache = GetCacheArchive();
  if (!cache->IsOpen() || cache->Contains(url)) [[unlikely]]
    return;

  // queue a download with no callback, which will cause it to be cached when it completes
  QueueDownload(url, {}, nullptr);
}

void HTTPCache::Prefetch(std::string_view url, PrefetchCallback callback)
{
  const auto cache = GetCacheArchive();

  if (!cache->IsOpen()) [[unlikely]]
  {
    callback(false);
    return;
  }

  // skip early if already cached
  if (cache->Contains(url))
  {
    callback(true);
    return;
  }

  // queue a download with no callback, which will cause it to be cached when it completes
  QueueDownload(url, [callback = std::move(callback)](std::span<const u8> data) { callback(!data.empty()); }, nullptr);
}

void HTTPCache::WaitForAllPrefetchRequests()
{
  HTTPDownloader::WaitForAllRequestsFromOwner(&s_locals);
}

bool HTTPCache::Clear(Error* error)
{
  return GetCacheArchive()->Clear(error);
}
