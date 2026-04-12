// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/locked_ptr.h"
#include "common/optional_with_status.h"
#include "common/types.h"
#include "common/heap_array.h"

#include <functional>
#include <mutex>
#include <string_view>

class Error;
class HTTPDownloader;
class ObjectArchive;

namespace HTTPCache {

/// Thread-safe locked pointer to the shared HTTP downloader. Holds the downloader mutex for its lifetime.
using DownloaderPtr = LockedPtr<HTTPDownloader, std::recursive_mutex>;

/// Thread-safe locked pointer to the shared cache archive. Holds the cache mutex for its lifetime.
using CacheArchivePtr = LockedPtr<ObjectArchive, std::mutex>;

/// Data returned from a successful lookup.
using LookupData = DynamicHeapArray<u8>;

/// Result of a cache lookup operation.
enum class LookupStatus
{
  Hit,   ///< URL was found in the cache; data is returned in the LookupResult.
  Miss,  ///< URL was not cached; a download has been queued and callback will fire asynchronously.
  Error, ///< A cache or downloader error occurred; callback will not be invoked.
};

/// A lookup result: carries the LookupStatus and, on a Hit, the cached data.
using LookupResult = OptionalWithStatus<LookupData, LookupStatus>;

/// Callback invoked with the downloaded or cached data. Receives an empty span on download failure.
using FetchCallback = std::function<void(std::span<const u8> data)>;

/// Callback invoked when prefetching for a given completes.
using PrefetchCallback = std::function<void(bool success)>;

/// Returns true if @p url uses the HTTP or HTTPS scheme.
bool IsHTTPURL(std::string_view url);

/// Returns the filename portion of @p url, stripping any query string or fragment.
std::string_view GetURLFilename(std::string_view url);

/// Returns the user agent to use for HTTP requests.
std::string GetUserAgent();

/// Shuts down the HTTP cache, releasing the downloader and cache archive.
void Shutdown();

/// Returns true if idle updates are necessary (e.g. outstanding requests).
bool HasAnyRequests();

/// Processes completed HTTP requests and invokes their callbacks. Should be called regularly on the main thread.
void PollRequests();

/// Waits for all requests to finish.
void WaitForAllRequests();

/// Waits for all requests to finish, periodically yielding to allow other tasks to run.
void WaitForAllRequestsWithYield(std::function<void()> before_sleep_cb, std::function<void()> after_sleep_cb);

/// Returns a locked pointer to the shared HTTP downloader, creating it on first use.
/// If @p create_error is non-null, creation details are written there on failure; if null, creation
/// is skipped after the first failed attempt.
DownloaderPtr GetDownloader(Error* create_error = nullptr);

/// Returns a locked pointer to the shared cache archive, opening it on first use.
CacheArchivePtr GetCacheArchive();

/// Looks up @p url in the cache.
/// On a Hit, the returned LookupResult holds the cached data.
/// On a Miss/Error, the result holds no value.
LookupResult Lookup(std::string_view url, Error* error);

/// Looks up @p url in the cache, and fetches it if it does not exist.
/// On a Hit, the returned LookupResult holds the cached data; @p callback is not called.
/// On a Miss, a download is queued and @p callback will be invoked asynchronously upon completion.
/// On an Error, @p callback is not called and the result holds no value.
LookupResult LookupOrFetch(std::string_view url, Error* error, FetchCallback callback);

/// Returns true if the given URL is already cached.
bool Contains(std::string_view url);

/// Queues a download for @p url if it is not already cached, with no callback.
/// Use this to warm the cache ahead of an anticipated lookup.
void Prefetch(std::string_view url);

/// Queues a download for @p url if it is not already cached, and invokes the callback when it is ready.
/// Use this to warm the cache ahead of an anticipated lookup.
void Prefetch(std::string_view url, PrefetchCallback callback);

/// Removes all entries currently in the cache.
bool Clear(Error* error);

} // namespace HTTPCache
