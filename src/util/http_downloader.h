// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/types.h"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Error;
class ProgressCallback;

/// Global HTTP downloading subsystem. Provides a platform-specific backend
/// (WinHTTP on Windows, libcurl elsewhere) managed as a singleton. All public
/// functions are thread-safe: requests may be created from any thread, and
/// callbacks are dispatched on whichever thread calls PollRequests().
namespace HTTPDownloader {

/// Span of NUL-terminated C-string HTTP header lines, e.g. {"X-Foo: bar"}.
using HeaderList = std::span<const char* const>;

/// Raw response body data.
using RequestData = std::vector<u8>;

/// Callback fired when a request completes, times out, or is cancelled.
/// Invoked on the thread that calls PollRequests(), with no internal locks held.
///
/// @param status_code  HTTP status code, or one of the negative HTTP_STATUS_* sentinels on failure.
/// @param error        Populated with a description when status_code < HTTP_STATUS_OK.
/// @param content_type Value of the response Content-Type header; empty if unavailable.
/// @param data         Response body; empty if the request did not succeed.
using RequestCallback =
  std::function<void(s32 status_code, Error& error, std::string& content_type, RequestData& data)>;

/// Synthetic status codes used in place of a real HTTP status on failure.
enum : s32
{
  HTTP_STATUS_CANCELLED = -3, ///< Request was cancelled via CancelRequests*().
  HTTP_STATUS_TIMEOUT = -2,   ///< Request exceeded its timeout duration.
  HTTP_STATUS_ERROR = -1,     ///< Network or protocol-level error; see the error argument.
  HTTP_STATUS_OK = 200        ///< Standard HTTP 200 OK.
};

/// Returns the file extension (without leading dot) for the given MIME type,
/// or an empty string if the type is not recognised.
std::string GetExtensionForContentType(const std::string& content_type);

/// Sets the default timeout applied to new requests when none is explicitly provided.
/// The initial default is 30 seconds.
void SetDefaultTimeout(u16 timeout_seconds);

/// Sets the maximum number of concurrently active (in-flight) requests.
/// Additional requests are queued and promoted as active slots become free.
/// Must be greater than zero. The initial default is 10.
void SetMaxActiveRequests(u16 max_active_requests);

/// Cancels all outstanding requests and tears down the HTTP backend.
/// Each cancelled request still has its callback invoked before this returns.
/// Safe to call even if the downloader was never initialised.
void Shutdown();

/// Queues an HTTP GET request. The callback is invoked from the thread that calls
/// PollRequests() when the request completes, times out, or is cancelled.
///
/// @param owner  Opaque pointer identifying the owner, used to cancel or wait on groups
///               of requests via CancelRequestsForOwner() / WaitForAllRequestsFromOwner().
///               May be null. The pointer is never dereferenced.
void CreateRequest(std::string url, const void* owner, RequestCallback callback, ProgressCallback* progress = nullptr,
                   HeaderList additional_headers = {}, std::optional<u16> timeout_seconds = {});

/// Queues an HTTP POST request with a URL-encoded body.
/// See CreateRequest() for parameter documentation.
void CreatePostRequest(std::string url, std::string post_data, const void* owner, RequestCallback callback,
                       ProgressCallback* progress = nullptr, HeaderList additional_headers = {},
                       std::optional<u16> timeout_seconds = {});

/// Checks all in-flight requests for completion, timeout, or cancellation, and
/// fires callbacks for any that have finished. Must be called periodically from
/// the thread that should receive callbacks.
void PollRequests();

/// Blocks the calling thread, polling until all queued and in-flight requests
/// have completed.
void WaitForAllRequests();

/// Like WaitForAllRequests(), but invokes before_sleep_cb / after_sleep_cb around
/// each sleep interval. The internal lock is released before before_sleep_cb and
/// re-acquired inside after_sleep_cb, allowing the caller to yield to other work.
void WaitForAllRequestsWithYield(std::function<void()> before_sleep_cb, std::function<void()> after_sleep_cb);

/// Blocks until all requests associated with owner have completed.
/// Passing nullptr is equivalent to WaitForAllRequests().
void WaitForAllRequestsFromOwner(const void* owner);

/// Like WaitForAllRequestsFromOwner(), but invokes yield callbacks around each sleep.
void WaitForAllRequestsFromOwnerWithYield(const void* owner, std::function<void()> before_sleep_cb,
                                          std::function<void()> after_sleep_cb);

/// Returns true if there are any requests currently queued or in-flight.
bool HasAnyRequests();

/// Returns true if there are any requests associated with owner queued or in-flight.
/// Passing nullptr is equivalent to HasAnyRequests().
bool HasAnyRequestsFromOwner(const void* owner);

/// Cancels all outstanding requests. Equivalent to CancelRequestsForOwner(nullptr).
void CancelAllRequests();

/// Cancels all outstanding requests belonging to owner.
/// Passing nullptr cancels every pending request (same as CancelAllRequests()).
/// Each cancelled request still has its callback invoked with HTTP_STATUS_CANCELLED.
/// The function loops until no matching requests remain, because a cancellation
/// callback may itself queue new requests for the same owner.
void CancelRequestsForOwner(const void* owner);

} // namespace HTTPDownloader

namespace Host {

/// Returns the user agent to use for HTTP requests.
std::string GetHTTPUserAgent();

/// Called by the HTTPDownloader implementation when the active state of the downloader changes.
/// The active parameter is set if there are any requests in-progress.
/// NOTE: Can be called from any thread, since any thread can queue requests.
void OnHTTPDownloaderActiveChanged(bool active);

} // namespace Host