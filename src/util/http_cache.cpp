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
#include "common/thirdparty/SmallVector.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include <fmt/format.h>

using namespace std::string_view_literals;

LOG_CHANNEL(HTTPCache);

namespace HTTPCache {

static constexpr u32 CACHE_VERSION = 1;

static void QueueDownload(std::string_view url, FetchCallback callback, Error* error,
                          std::unique_lock<std::mutex>&& lock);
static void DownloadCallback(const std::string& url, s32 status_code, const Error& error,
                             const std::string& content_type, const HTTPDownloader::RequestData& data);

namespace {

struct Locals
{
  ObjectArchive cache_archive;
  std::deque<std::pair<std::string, FetchCallback>> pending_downloads;
  std::mutex pending_downloads_lock;
  std::once_flag cache_open_flag;
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

  {
    const std::unique_lock cache_lock(s_locals.pending_downloads_lock);
    for (auto iter = s_locals.pending_downloads.begin(); iter != s_locals.pending_downloads.end();)
    {
      if (iter->second)
        iter->second({});
      iter = s_locals.pending_downloads.erase(iter);
    }
  }

  s_locals.cache_archive.Close();
}

std::span<const u8> HTTPCache::URLToCacheKey(std::string_view key)
{
  return std::span<const u8>(reinterpret_cast<const u8*>(key.data()), key.size());
}

ObjectArchive& HTTPCache::GetCacheArchive()
{
  // Opens once, never closes. Therefore this is safe to skip the once_flag in the fast path.
  if (!s_locals.cache_archive.IsOpen()) [[unlikely]]
  {
    std::call_once(s_locals.cache_open_flag, []() {
      Error error;
      std::string cache_path = Path::Combine(EmuFolders::Cache, "http_cache");
      if (!s_locals.cache_archive.OpenPath(cache_path, CACHE_VERSION, &error))
        ERROR_LOG("Failed to initialize HTTP cache: {}", error.GetDescription());
    });
  }

  return s_locals.cache_archive;
}

HTTPCache::LookupResult HTTPCache::Lookup(std::string_view url, Error* error)
{
  Error lookup_error;
  std::optional<ObjectArchive::ObjectData> image_data = GetCacheArchive().Lookup(URLToCacheKey(url), &lookup_error);
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

  Error lookup_error;
  image_data = GetCacheArchive().Lookup(URLToCacheKey(url), &lookup_error);

  // did we find it? return the data directly without invoking the callback
  if (image_data.has_value())
  {
    return LookupResult(LookupStatus::Hit, std::move(*image_data));
  }
  else if (lookup_error.GetDescription() != ObjectArchive::ERROR_DESCRIPTION_DOES_NOT_EXIST) [[unlikely]]
  {
    // Errors are unrecoverable.
    ERROR_LOG("Failed to read cached texture data for URL '{}': {}", url, lookup_error.GetDescription());
    if (error)
      *error = std::move(lookup_error);

    return LookupResult(LookupStatus::Error);
  }

  // Try the lookup again with the lock held, core thread could have completed in the meantime.
  std::unique_lock lock(s_locals.pending_downloads_lock);
  image_data = GetCacheArchive().Lookup(URLToCacheKey(url), &lookup_error);
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

  QueueDownload(url, std::move(callback), error, std::move(lock));
  return LookupResult(LookupStatus::Miss);
}

void HTTPCache::QueueDownload(std::string_view url, FetchCallback callback, Error* error,
                              std::unique_lock<std::mutex>&& lock)
{
  // do we already have a request?
  const bool has_request =
    std::ranges::any_of(s_locals.pending_downloads, [url](const auto& pair) { return pair.first == url; });

  // add to pending callbacks so that it will be invoked when the request completes
  // don't add multiple dummy entries for the same url
  if (!has_request || callback)
    s_locals.pending_downloads.emplace_back(url, std::move(callback));

  // don't queue it twice
  if (has_request)
    return;

  DEV_LOG("Cache miss for URL '{}', downloading...", url);

  // release lock because CreateRequest() can fire the callback immediately
  lock.unlock();
  HTTPDownloader::CreateRequest(std::string(url), &s_locals,
                                [url = std::string(url)](s32 status_code, Error& error, std::string& content_type,
                                                         HTTPDownloader::RequestData& data) {
                                  DownloadCallback(url, status_code, error, content_type, std::move(data));
                                });
}

void HTTPCache::DownloadCallback(const std::string& url, s32 status_code, const Error& error,
                                 const std::string& content_type, const HTTPDownloader::RequestData& data)
{
  // hold the lock for the insertion, so we don't create a duplicate request as described in Lookup()
  std::unique_lock lock(s_locals.pending_downloads_lock);

  // don't insert into cache on failure
  const bool success = (status_code == HTTPDownloader::HTTP_STATUS_OK);
  if (success)
  {
    VERBOSE_LOG("Adding URL '{}' to cache ({} bytes)", url, data.size());

    // TODO: only compress if it's not images
    Error insert_error;
    if (!GetCacheArchive().Insert(URLToCacheKey(url), data, ObjectArchive::CompressType::Uncompressed, &insert_error))
    {
      if (insert_error.GetDescription() != ObjectArchive::ERROR_DESCRIPTION_ALREADY_EXISTS)
        ERROR_LOG("Failed to insert downloaded data for URL '{}' into cache: {}", url, insert_error.GetDescription());
    }
  }
  else
  {
    ERROR_LOG("Failed to download '{}': HTTP status code {}, error: {}", url, status_code, error.GetDescription());
  }

  // invoke all callbacks. uses indexing in case something gets added in the callback
  for (auto iter = s_locals.pending_downloads.begin(); iter != s_locals.pending_downloads.end();)
  {
    if (iter->first != url)
    {
      ++iter;
      continue;
    }

    if (iter->second)
    {
      // callback could queue another download and invalidate the iterator, so shove all callbacks for
      // the same url into a temporary list before executing them.
      llvm::SmallVector<FetchCallback> pending_callbacks;
      for (; iter != s_locals.pending_downloads.end();)
      {
        if (iter->first == url)
        {
          if (iter->second)
            pending_callbacks.push_back(std::move(iter->second));

          iter = s_locals.pending_downloads.erase(iter);
        }
        else
        {
          ++iter;
        }
      }

      lock.unlock();
      for (FetchCallback& callback : pending_callbacks)
        callback(success ? data : std::span<const u8>());

      // all requests for this url have been processed, so we don't need to do anything else here
      return;
    }
    else
    {
      iter = s_locals.pending_downloads.erase(iter);
    }
  }
}

bool HTTPCache::Contains(std::string_view url)
{
  return GetCacheArchive().Contains(URLToCacheKey(url));
}

void HTTPCache::Prefetch(std::string_view url)
{
  // skip early if already cached, or cannot prefetch
  const ObjectArchive& cache = GetCacheArchive();
  if (!cache.IsOpen() || cache.Contains(URLToCacheKey(url))) [[unlikely]]
    return;

  // queue a download with no callback, which will cause it to be cached when it completes
  std::unique_lock lock(s_locals.pending_downloads_lock);

  // see Lookup() for why we check again.
  if (cache.Contains(URLToCacheKey(url)))
    return;

  QueueDownload(url, {}, nullptr, std::move(lock));
}

void HTTPCache::Prefetch(std::string_view url, PrefetchCallback callback)
{
  const ObjectArchive& cache = GetCacheArchive();

  if (!cache.IsOpen()) [[unlikely]]
  {
    callback(false);
    return;
  }

  // skip early if already cached
  if (cache.Contains(URLToCacheKey(url)))
  {
    callback(true);
    return;
  }

  // queue a download with no callback, which will cause it to be cached when it completes
  std::unique_lock lock(s_locals.pending_downloads_lock);

  // see Lookup() for why we check again.
  if (cache.Contains(URLToCacheKey(url)))
  {
    lock.unlock();
    callback(true);
    return;
  }

  QueueDownload(
    url, [callback = std::move(callback)](std::span<const u8> data) { callback(!data.empty()); }, nullptr,
    std::move(lock));
}

void HTTPCache::WaitForAllPrefetchRequests()
{
  // there's a small window before the request has been created, handle it by checking against the pending list
  for (;;)
  {
    HTTPDownloader::WaitForAllRequestsFromOwner(&s_locals);

    std::unique_lock lock(s_locals.pending_downloads_lock);
    if (s_locals.pending_downloads.empty())
      break;
  }
}

bool HTTPCache::Clear(Error* error)
{
  return GetCacheArchive().Clear(error);
}
