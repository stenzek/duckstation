// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "http_downloader.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/progress_callback.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "common/timer.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)

#define USE_WINHTTP

#include "common/windows_headers.h"

#include <winhttp.h>

#else

#define USE_CURL

#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>

#endif

LOG_CHANNEL(HTTPDownloader);

namespace HTTPDownloader {
static constexpr u16 DEFAULT_TIMEOUT_IN_SECONDS = 30;

// Chrome uses 10 server calls per domain, seems reasonable.
// TODO: Check this per domain when polling...
static constexpr u16 DEFAULT_MAX_ACTIVE_REQUESTS = 10;

static constexpr u64 WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_MS = 16;
static constexpr u64 WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_NS = WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_MS * 1000000;

namespace {
struct Request
{
  enum class Type : u8
  {
    Get,
    Post,
  };

  /// Lifecycle of a request. Transitions flow forward only; Cancelled may be set
  /// from Pending, Started, or Receiving by the main thread via CancelRequestsForOwner().
  /// All state changes are made with release semantics and read with acquire semantics.
  enum class State : u8
  {
    Pending,   ///< Created, not yet submitted to the HTTP backend.
    Cancelled, ///< Cancelled by the caller; callback will be fired with HTTP_STATUS_CANCELLED.
    Started,   ///< Submitted to the backend; waiting for response headers.
    Receiving, ///< Headers received; body data is being accumulated in `data`.
    Complete,  ///< Transfer finished (success or error); ready for callback dispatch.
  };

  Request(const void* owner, Type type, std::string url, std::string post_data, RequestCallback callback,
          ProgressCallback* progress, u16 timeout_seconds);
  ~Request();

  const void* owner; ///< Opaque caller token; never dereferenced (used for owner-based cancel/wait).
  RequestCallback callback;
  ProgressCallback* progress;
  std::string url;
  std::string post_data;
  std::string content_type;
  RequestData data;
  Error error;
  u64 start_time = 0;       ///< Timer value recorded when the request was submitted to the backend.
  u64 last_update_time = 0; ///< Timer value of the most recent data activity; used for timeout detection.
  s32 status_code = 0;
  u32 content_length = 0;
  u32 last_progress_update = 0;
  Type type = Type::Get;
  std::atomic<State> state{State::Pending};
  u16 timeout_seconds = 0;

#if defined(USE_WINHTTP)

  std::wstring object_name;
  std::wstring additional_headers;
  HINTERNET hConnection = NULL;
  HINTERNET hRequest = NULL;
  u32 io_position = 0; ///< Write offset into `data` for the current async WinHttpReadData() call.

#elif defined(USE_CURL)

  CURL* handle = nullptr;
  curl_slist* header_list = nullptr;

#endif
};
} // namespace

static void StartOrAddRequest(Request* req);
static u32 LockedGetActiveRequestCount();
static void LockedPollRequests(std::unique_lock<std::mutex>& lock);

// Platform specific implementations

static bool EnsureInitialized(Error* error);
static void InternalCreateRequest(Request::Type type, std::string url, std::string post_data, const void* owner,
                                  RequestCallback callback, ProgressCallback* progress, u16 timeout_seconds,
                                  HeaderList additional_headers);

static bool StartRequest(Request* req);
static void CloseRequest(Request* req);
static void DeleteRequest(Request* req);

#if defined(USE_WINHTTP)

static void CALLBACK HTTPStatusCallback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus,
                                        LPVOID lpvStatusInformation, DWORD dwStatusInformationLength);

#elif defined(USE_CURL)

/// Actions dispatched from the main thread to the curl worker thread via worker_queue.
enum class QueueAction
{
  Add,             ///< Add request->handle to the curl multi handle.
  RemoveAndDelete, ///< Remove request->handle from the multi handle, clean it up, and delete the request.
};

static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

static void WorkerThreadEntryPoint();

static void ProcessQueuedActions();
static void ReadMultiResults();

#endif

namespace {
struct ALIGN_TO_CACHE_LINE Locals
{
  u16 default_timeout = DEFAULT_TIMEOUT_IN_SECONDS;
  u16 max_active_requests = DEFAULT_MAX_ACTIVE_REQUESTS;

  /// Guards pending_http_requests. Also used to serialise callback dispatch in
  /// LockedPollRequests() — the lock is released around each callback invocation.
  std::mutex pending_http_request_lock;

  /// All requests that are either queued (Pending) or actively in-flight
  /// (Started / Receiving). Requests are removed just before their callback fires.
  std::vector<Request*> pending_http_requests;

#if defined(USE_WINHTTP)

  std::once_flag init_flag; ///< Ensures WinHttpOpen() is called exactly once.
  HINTERNET hSession = NULL;

#elif defined(USE_CURL)

  std::once_flag init_flag; ///< Ensures curl global/multi init runs exactly once.
  CURLM* multi_handle = nullptr;
  std::string user_agent;

  /// Background thread that drives all curl I/O (curl_multi_poll / perform loop).
  Threading::Thread worker_thread;

  /// Producer-consumer queue from the main thread to the worker thread.
  /// Protected by worker_queue_mutex; woken with curl_multi_wakeup().
  ALIGN_TO_CACHE_LINE std::deque<std::pair<QueueAction, Request*>> worker_queue;
  std::atomic_bool worker_thread_shutdown{false}; ///< Set to true to signal the worker to exit.
  std::mutex worker_queue_mutex;

#endif
};
} // namespace

static Locals s_locals;

} // namespace HTTPDownloader

HTTPDownloader::Request::Request(const void* owner, Type type, std::string url, std::string post_data,
                                 RequestCallback callback, ProgressCallback* progress, u16 timeout_seconds)
  : owner(owner), callback(std::move(callback)), progress(progress), url(std::move(url)),
    post_data(std::move(post_data)), type(type), timeout_seconds(timeout_seconds)
{
  // set progress state to indeterminate until we know the size
  if (progress)
    progress->SetState(0, 0);
}

HTTPDownloader::Request::~Request() = default;

void HTTPDownloader::SetDefaultTimeout(u16 timeout_seconds)
{
  s_locals.default_timeout = timeout_seconds;
}

void HTTPDownloader::SetMaxActiveRequests(u16 max_active_requests)
{
  Assert(max_active_requests > 0);
  s_locals.max_active_requests = max_active_requests;
}

void HTTPDownloader::CreateRequest(std::string url, const void* owner, RequestCallback callback,
                                   ProgressCallback* progress /* = nullptr */, HeaderList additional_headers /* =  */,
                                   std::optional<u16> timeout_seconds /* = */)
{
  InternalCreateRequest(Request::Type::Get, std::move(url), {}, owner, std::move(callback), progress,
                        timeout_seconds.value_or(s_locals.default_timeout), additional_headers);
}

void HTTPDownloader::CreatePostRequest(std::string url, std::string post_data, const void* owner,
                                       RequestCallback callback, ProgressCallback* progress /* = nullptr */,
                                       HeaderList additional_headers /* =  */,
                                       std::optional<u16> timeout_seconds /* = */)
{
  InternalCreateRequest(Request::Type::Post, std::move(url), std::move(post_data), owner, std::move(callback), progress,
                        timeout_seconds.value_or(s_locals.default_timeout), additional_headers);
}

// If the number of active requests is below the limit, starts the request immediately;
// otherwise leaves it in Pending state to be promoted by LockedPollRequests().
// Notifies the host when the queue transitions from empty to non-empty.
// Called without pending_http_request_lock held; acquires it internally.
void HTTPDownloader::StartOrAddRequest(Request* req)
{
  std::unique_lock lock(s_locals.pending_http_request_lock);
  if (LockedGetActiveRequestCount() < s_locals.max_active_requests)
  {
    if (!StartRequest(req))
      return;
  }

  const bool was_empty = s_locals.pending_http_requests.empty();
  s_locals.pending_http_requests.push_back(req);
  if (was_empty)
    Host::OnHTTPDownloaderActiveChanged(true);
}

// Drives the request queue: handles timeouts, progress-based cancellations, and
// completed requests (firing their callbacks), then promotes Pending requests to
// Started when active slots are available.
//
// Must be called with pending_http_request_lock held. The lock is released and
// re-acquired around every callback invocation so that callbacks may safely call
// back into the HTTPDownloader API. The queue index is rewound after each
// re-acquisition to handle requests added or removed by the callback.
//
// Notifies the host when the queue transitions from non-empty to empty.
void HTTPDownloader::LockedPollRequests(std::unique_lock<std::mutex>& lock)
{
  if (s_locals.pending_http_requests.empty())
    return;

  const Timer::Value current_time = Timer::GetCurrentValue();
  u32 active_requests = 0;
  u32 unstarted_requests = 0;

  for (size_t index = 0; index < s_locals.pending_http_requests.size();)
  {
    Request* req = s_locals.pending_http_requests[index];
    const Request::State req_state = req->state.load(std::memory_order_acquire);
    if (req_state == Request::State::Pending)
    {
      unstarted_requests++;
      index++;
      continue;
    }

    if ((req_state == Request::State::Started || req_state == Request::State::Receiving) &&
        current_time >= req->last_update_time &&
        Timer::ConvertValueToSeconds(current_time - req->last_update_time) >= static_cast<float>(req->timeout_seconds))
    {
      // request timed out
      ERROR_LOG("Request for '{}' timed out", req->url);

      req->state.store(Request::State::Cancelled, std::memory_order_release);
      s_locals.pending_http_requests.erase(s_locals.pending_http_requests.begin() + index);
      lock.unlock();

      req->error.SetStringFmt("Request timed out after {} seconds.", req->timeout_seconds);
      req->callback(HTTP_STATUS_TIMEOUT, req->error, req->content_type, req->data);

      CloseRequest(req);

      lock.lock();
      continue;
    }
    else if ((req_state == Request::State::Started || req_state == Request::State::Receiving) && req->progress &&
             req->progress->IsCancelled())
    {
      // request timed out
      ERROR_LOG("Request for '{}' cancelled", req->url);

      req->state.store(Request::State::Cancelled, std::memory_order_release);
      s_locals.pending_http_requests.erase(s_locals.pending_http_requests.begin() + index);
      lock.unlock();

      req->error.SetStringView("Request was cancelled.");
      req->callback(HTTP_STATUS_CANCELLED, req->error, req->content_type, req->data);

      CloseRequest(req);

      lock.lock();
      continue;
    }

    if (req_state != Request::State::Complete)
    {
      if (req->progress)
      {
        const u32 size = static_cast<u32>(req->data.size());
        if (size != req->last_progress_update)
        {
          req->last_progress_update = size;
          req->progress->SetProgressRange(req->content_length);
          req->progress->SetProgressValue(req->last_progress_update);
        }
      }

      active_requests++;
      index++;
      continue;
    }

    // request complete
    VERBOSE_LOG("Request for '{}' complete, returned status code {} and {} bytes, took {:.0f} ms", req->url,
                req->status_code, req->data.size(), Timer::ConvertValueToMilliseconds(current_time - req->start_time));
    s_locals.pending_http_requests.erase(s_locals.pending_http_requests.begin() + index);

    // run callback with lock unheld
    lock.unlock();
    if (req->status_code >= 0 && req->status_code != HTTP_STATUS_OK)
      req->error.SetStringFmt("Request failed with HTTP status code {}", req->status_code);
    else if (req->status_code < 0)
      DEV_LOG("Request failed with error {}", req->error.GetDescription());

    req->callback(req->status_code, req->error, req->content_type, req->data);
    CloseRequest(req);
    lock.lock();
  }

  // start new requests when we finished some
  if (unstarted_requests > 0 && active_requests < s_locals.max_active_requests)
  {
    for (size_t index = 0; index < s_locals.pending_http_requests.size();)
    {
      Request* req = s_locals.pending_http_requests[index];
      if (req->state != Request::State::Pending)
      {
        index++;
        continue;
      }

      if (!StartRequest(req))
      {
        s_locals.pending_http_requests.erase(s_locals.pending_http_requests.begin() + index);
        continue;
      }

      active_requests++;
      index++;

      if (active_requests >= s_locals.max_active_requests)
        break;
    }
  }

  // notify host
  if (s_locals.pending_http_requests.empty())
    Host::OnHTTPDownloaderActiveChanged(false);
}

void HTTPDownloader::PollRequests()
{
  std::unique_lock lock(s_locals.pending_http_request_lock);
  LockedPollRequests(lock);
}

void HTTPDownloader::WaitForAllRequests()
{
  std::unique_lock lock(s_locals.pending_http_request_lock);
  while (!s_locals.pending_http_requests.empty())
  {
    // Don't burn too much CPU.
    Timer::NanoSleep(WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_NS);
    LockedPollRequests(lock);
  }
}

void HTTPDownloader::WaitForAllRequestsWithYield(std::function<void()> before_sleep_cb,
                                                 std::function<void()> after_sleep_cb)
{
  std::unique_lock lock(s_locals.pending_http_request_lock);
  while (!s_locals.pending_http_requests.empty())
  {
    // Don't burn too much CPU.
    if (before_sleep_cb)
    {
      lock.unlock();
      before_sleep_cb();
    }
    Timer::NanoSleep(WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_NS);
    if (after_sleep_cb)
    {
      after_sleep_cb();
      lock.lock();
    }
    LockedPollRequests(lock);
  }
}

void HTTPDownloader::WaitForAllRequestsFromOwner(const void* owner)
{
  if (!owner)
  {
    WaitForAllRequests();
    return;
  }

  std::unique_lock lock(s_locals.pending_http_request_lock);
  while (
    std::ranges::any_of(s_locals.pending_http_requests, [owner](const Request* req) { return req->owner == owner; }))
  {
    // Don't burn too much CPU.
    Timer::NanoSleep(WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_NS);
    LockedPollRequests(lock);
  }
}

void HTTPDownloader::WaitForAllRequestsFromOwnerWithYield(const void* owner, std::function<void()> before_sleep_cb,
                                                          std::function<void()> after_sleep_cb)
{
  if (!owner)
  {
    WaitForAllRequestsWithYield(std::move(before_sleep_cb), std::move(after_sleep_cb));
    return;
  }

  std::unique_lock lock(s_locals.pending_http_request_lock);
  while (
    std::ranges::any_of(s_locals.pending_http_requests, [owner](const Request* req) { return req->owner == owner; }))
  {
    // Don't burn too much CPU.
    if (before_sleep_cb)
    {
      lock.unlock();
      before_sleep_cb();
    }
    Timer::NanoSleep(WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_NS);
    if (after_sleep_cb)
    {
      after_sleep_cb();
      lock.lock();
    }
    LockedPollRequests(lock);
  }
}

// Returns the number of requests currently in Started or Receiving state.
// Must be called with pending_http_request_lock held.
u32 HTTPDownloader::LockedGetActiveRequestCount()
{
  u32 count = 0;
  for (const Request* const req : s_locals.pending_http_requests)
  {
    const Request::State req_state = req->state.load(std::memory_order_acquire);
    if (req_state == Request::State::Started || req_state == Request::State::Receiving)
      count++;
  }
  return count;
}

bool HTTPDownloader::HasAnyRequests()
{
  std::unique_lock lock(s_locals.pending_http_request_lock);
  return !s_locals.pending_http_requests.empty();
}

bool HTTPDownloader::HasAnyRequestsFromOwner(const void* owner)
{
  if (!owner)
    return HasAnyRequests();

  std::unique_lock lock(s_locals.pending_http_request_lock);
  return std::ranges::any_of(s_locals.pending_http_requests,
                             [owner](const Request* req) { return req->owner == owner; });
}

void HTTPDownloader::CancelAllRequests()
{
  CancelRequestsForOwner(nullptr);
}

void HTTPDownloader::CancelRequestsForOwner(const void* owner)
{
  std::unique_lock lock(s_locals.pending_http_request_lock);

  if (s_locals.pending_http_requests.empty())
    return;

  // one request might start another, so loop multiple times until we match none
  bool had_matching_requests;
  do
  {
    had_matching_requests = false;

    for (size_t index = 0; index < s_locals.pending_http_requests.size();)
    {
      Request* req = s_locals.pending_http_requests[index];
      if (owner && req->owner != owner)
      {
        index++;
        continue;
      }

      // Should never be cancelled at this point.
      const Request::State req_state = req->state.load(std::memory_order_acquire);
      DebugAssert(req_state != Request::State::Cancelled);

      // Cancel even completed requests.
      ERROR_LOG("Request for '{}' cancelled", req->url);

      req->state.store(Request::State::Cancelled, std::memory_order_release);
      s_locals.pending_http_requests.erase(s_locals.pending_http_requests.begin() + index);
      lock.unlock();

      req->error.SetStringView("Request was cancelled.");
      req->callback(HTTP_STATUS_CANCELLED, req->error, req->content_type, req->data);

      // If pending, we can delete it immediately since it won't be processed by the worker thread.
      // Otherwise, we need to close it so the worker thread can clean up properly.
      if (req_state == Request::State::Pending)
        DeleteRequest(req);
      else
        CloseRequest(req);

      lock.lock();
      had_matching_requests = true;
    }
  } while (had_matching_requests);

  // notify host
  if (s_locals.pending_http_requests.empty())
    Host::OnHTTPDownloaderActiveChanged(false);
}

std::string HTTPDownloader::GetExtensionForContentType(const std::string& content_type)
{
  // Based on https://developer.mozilla.org/en-US/docs/Web/HTTP/Basics_of_HTTP/MIME_types/Common_types
  static constexpr const char* table[][2] = {
    {"audio/aac", "aac"},
    {"application/x-abiword", "abw"},
    {"application/x-freearc", "arc"},
    {"image/avif", "avif"},
    {"video/x-msvideo", "avi"},
    {"application/vnd.amazon.ebook", "azw"},
    {"application/octet-stream", "bin"},
    {"image/bmp", "bmp"},
    {"application/x-bzip", "bz"},
    {"application/x-bzip2", "bz2"},
    {"application/x-cdf", "cda"},
    {"application/x-csh", "csh"},
    {"text/css", "css"},
    {"text/csv", "csv"},
    {"application/msword", "doc"},
    {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx"},
    {"application/vnd.ms-fontobject", "eot"},
    {"application/epub+zip", "epub"},
    {"application/gzip", "gz"},
    {"image/gif", "gif"},
    {"text/html", "htm"},
    {"image/vnd.microsoft.icon", "ico"},
    {"text/calendar", "ics"},
    {"application/java-archive", "jar"},
    {"image/jpeg", "jpg"},
    {"text/javascript", "js"},
    {"application/json", "json"},
    {"application/ld+json", "jsonld"},
    {"audio/midi audio/x-midi", "mid"},
    {"text/javascript", "mjs"},
    {"audio/mpeg", "mp3"},
    {"video/mp4", "mp4"},
    {"video/mpeg", "mpeg"},
    {"application/vnd.apple.installer+xml", "mpkg"},
    {"application/vnd.oasis.opendocument.presentation", "odp"},
    {"application/vnd.oasis.opendocument.spreadsheet", "ods"},
    {"application/vnd.oasis.opendocument.text", "odt"},
    {"audio/ogg", "oga"},
    {"video/ogg", "ogv"},
    {"application/ogg", "ogx"},
    {"audio/opus", "opus"},
    {"font/otf", "otf"},
    {"image/png", "png"},
    {"application/pdf", "pdf"},
    {"application/x-httpd-php", "php"},
    {"application/vnd.ms-powerpoint", "ppt"},
    {"application/vnd.openxmlformats-officedocument.presentationml.presentation", "pptx"},
    {"application/vnd.rar", "rar"},
    {"application/rtf", "rtf"},
    {"application/x-sh", "sh"},
    {"image/svg+xml", "svg"},
    {"application/x-tar", "tar"},
    {"image/tiff", "tif"},
    {"video/mp2t", "ts"},
    {"font/ttf", "ttf"},
    {"text/plain", "txt"},
    {"application/vnd.visio", "vsd"},
    {"audio/wav", "wav"},
    {"audio/webm", "weba"},
    {"video/webm", "webm"},
    {"image/webp", "webp"},
    {"font/woff", "woff"},
    {"font/woff2", "woff2"},
    {"application/xhtml+xml", "xhtml"},
    {"application/vnd.ms-excel", "xls"},
    {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "xlsx"},
    {"application/xml", "xml"},
    {"text/xml", "xml"},
    {"application/vnd.mozilla.xul+xml", "xul"},
    {"application/zip", "zip"},
    {"video/3gpp", "3gp"},
    {"audio/3gpp", "3gp"},
    {"video/3gpp2", "3g2"},
    {"audio/3gpp2", "3g2"},
    {"application/x-7z-compressed", "7z"},
  };

  std::string ret;
  for (size_t i = 0; i < std::size(table); i++)
  {
    if (StringUtil::Strncasecmp(table[i][0], content_type.data(), content_type.length()) == 0)
    {
      ret = table[i][1];
      break;
    }
  }
  return ret;
}

#if defined(USE_WINHTTP)

// Lazily initialises the WinHTTP session handle exactly once via std::call_once.
// Subsequent calls return immediately using the cached hSession check.
// Returns false (and populates error) if initialisation failed or previously failed.
bool HTTPDownloader::EnsureInitialized(Error* error)
{
  if (s_locals.hSession != NULL) [[likely]]
    return true;

  Error::Clear(error);

  std::call_once(s_locals.init_flag, [&error]() {
    static constexpr DWORD dwAccessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;

    const HINTERNET hSession = WinHttpOpen(StringUtil::UTF8StringToWideString(Host::GetHTTPUserAgent()).c_str(),
                                           dwAccessType, nullptr, nullptr, WINHTTP_FLAG_ASYNC);
    if (hSession == NULL)
    {
      Error::SetWin32(error, "WinHttpOpen() failed: ", GetLastError());
      return;
    }

    const DWORD notification_flags = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
                                     WINHTTP_CALLBACK_FLAG_HANDLES | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE;
    if (WinHttpSetStatusCallback(hSession, HTTPStatusCallback, notification_flags, NULL) ==
        WINHTTP_INVALID_STATUS_CALLBACK)
    {
      Error::SetWin32(error, "WinHttpSetStatusCallback() failed: ", GetLastError());
      WinHttpCloseHandle(hSession);
      return;
    }

    s_locals.hSession = hSession;
  });

  if (s_locals.hSession != NULL) [[likely]]
    return true;

  if (error && !error->IsValid())
    error->SetStringView(error, "WinHttp previously failed to initialize.");

  return false;
}

// Cancels all in-flight requests, deregisters the status callback, and closes the
// WinHTTP session handle. The callback must be removed before closing hSession to
// avoid callbacks arriving after the session is destroyed.
void HTTPDownloader::Shutdown()
{
  CancelRequestsForOwner(nullptr);

  if (s_locals.hSession)
  {
    WinHttpSetStatusCallback(s_locals.hSession, nullptr, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL);
    WinHttpCloseHandle(s_locals.hSession);
    s_locals.hSession = NULL;
  }
}

// Async I/O completion callback invoked on a WinHTTP internal thread pool thread.
// Drives the full request lifecycle: send → receive response → read body.
// The Request pointer is stored as the dwContext value on the request handle.
//
// LIFETIME NOTE: The Request object is freed inside the HANDLE_CLOSING case, which
// fires after WinHttpCloseHandle(hRequest) completes all pending async operations.
// No Request member may be accessed after that point.
void CALLBACK HTTPDownloader::HTTPStatusCallback(HINTERNET hRequest, DWORD_PTR dwContext, DWORD dwInternetStatus,
                                                 LPVOID lpvStatusInformation, DWORD dwStatusInformationLength)
{
  Request* req = reinterpret_cast<Request*>(dwContext);
  switch (dwInternetStatus)
  {
    case WINHTTP_CALLBACK_STATUS_HANDLE_CREATED:
      return;

    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
    {
      if (!req)
        return;

      DebugAssert(hRequest == req->hRequest);

      std::unique_lock lock(s_locals.pending_http_request_lock);
      Assert(
        std::ranges::none_of(s_locals.pending_http_requests, [req](HTTPDownloader::Request* it) { return it == req; }));

      // we can clean up the connection as well
      DebugAssert(req->hConnection != NULL);
      WinHttpCloseHandle(req->hConnection);
      DeleteRequest(req);
      return;
    }

    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
    {
      const WINHTTP_ASYNC_RESULT* res = reinterpret_cast<const WINHTTP_ASYNC_RESULT*>(lpvStatusInformation);
      ERROR_LOG("WinHttp async function {} returned error {}", res->dwResult, res->dwError);
      req->status_code = HTTP_STATUS_ERROR;
      req->error.SetStringFmt("WinHttp async function {} returned error {}", res->dwResult, res->dwError);
      req->state.store(Request::State::Complete, std::memory_order_release);
      return;
    }
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    {
      DEBUG_LOG("SendRequest complete");
      if (!WinHttpReceiveResponse(hRequest, nullptr))
      {
        const DWORD err = GetLastError();
        ERROR_LOG("WinHttpReceiveResponse() failed: {}", err);
        req->status_code = HTTP_STATUS_ERROR;
        req->error.SetWin32("WinHttpReceiveResponse() failed: ", err);
        req->state.store(Request::State::Complete, std::memory_order_release);
      }

      return;
    }
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
    {
      DEBUG_LOG("Headers available");

      DWORD buffer_size = sizeof(req->status_code);
      if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &req->status_code, &buffer_size, WINHTTP_NO_HEADER_INDEX))
      {
        const DWORD err = GetLastError();
        ERROR_LOG("WinHttpQueryHeaders() for status code failed: {}", err);
        req->status_code = HTTP_STATUS_ERROR;
        req->error.SetWin32("WinHttpQueryHeaders() failed: ", err);
        req->state.store(Request::State::Complete, std::memory_order_release);
        return;
      }

      buffer_size = sizeof(req->content_length);
      if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &req->content_length, &buffer_size,
                               WINHTTP_NO_HEADER_INDEX))
      {
        const DWORD err = GetLastError();
        if (err != ERROR_WINHTTP_HEADER_NOT_FOUND)
          WARNING_LOG("WinHttpQueryHeaders() for content length failed: {}", err);

        req->content_length = 0;
      }

      DWORD content_type_length = 0;
      if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                               WINHTTP_NO_OUTPUT_BUFFER, &content_type_length, WINHTTP_NO_HEADER_INDEX) &&
          GetLastError() == ERROR_INSUFFICIENT_BUFFER && content_type_length >= sizeof(content_type_length))
      {
        std::wstring content_type_wstring;
        content_type_wstring.resize((content_type_length / sizeof(wchar_t)) - 1);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                content_type_wstring.data(), &content_type_length, WINHTTP_NO_HEADER_INDEX))
        {
          req->content_type = StringUtil::WideStringToUTF8String(content_type_wstring);
        }
      }

      DEV_LOG("Status code {}, content-length is {}", req->status_code, req->content_length);
      req->data.reserve(req->content_length);
      req->state.store(Request::State::Receiving, std::memory_order_release);

      // start reading
      if (!WinHttpQueryDataAvailable(hRequest, nullptr) && GetLastError() != ERROR_IO_PENDING)
      {
        const DWORD err = GetLastError();
        ERROR_LOG("WinHttpQueryDataAvailable() failed: {}", err);
        req->status_code = HTTP_STATUS_ERROR;
        req->error.SetWin32("WinHttpQueryDataAvailable() failed: ", err);
        req->state.store(Request::State::Complete, std::memory_order_release);
      }

      return;
    }
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
    {
      DWORD bytes_available;
      std::memcpy(&bytes_available, lpvStatusInformation, sizeof(bytes_available));
      if (bytes_available == 0)
      {
        // end of request
        DEV_LOG("End of request '{}', {} bytes received", req->url, req->data.size());
        req->state.store(Request::State::Complete, std::memory_order_release);
        return;
      }

      // start the transfer
      DEBUG_LOG("{} bytes available", bytes_available);
      req->io_position = static_cast<u32>(req->data.size());
      req->data.resize(req->io_position + bytes_available);
      if (!WinHttpReadData(hRequest, req->data.data() + req->io_position, bytes_available, nullptr) &&
          GetLastError() != ERROR_IO_PENDING)
      {
        const DWORD err = GetLastError();
        ERROR_LOG("WinHttpReadData() failed: {}", err);
        req->status_code = HTTP_STATUS_ERROR;
        req->error.SetWin32("WinHttpReadData() failed: ", err);
        req->state.store(Request::State::Complete, std::memory_order_release);
      }

      return;
    }
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
    {
      DEBUG_LOG("Read of {} complete", dwStatusInformationLength);

      const u32 new_size = req->io_position + dwStatusInformationLength;
      Assert(new_size <= req->data.size());
      req->data.resize(new_size);
      req->last_update_time = Timer::GetCurrentValue();

      if (!WinHttpQueryDataAvailable(hRequest, nullptr) && GetLastError() != ERROR_IO_PENDING)
      {
        const DWORD err = GetLastError();
        ERROR_LOG("WinHttpQueryDataAvailable() failed: {}", err);
        req->status_code = HTTP_STATUS_ERROR;
        req->error.SetWin32("WinHttpQueryDataAvailable() failed: ", err);
        req->state.store(Request::State::Complete, std::memory_order_release);
      }

      return;
    }
    default:
      // unhandled, ignore
      return;
  }
}

void HTTPDownloader::InternalCreateRequest(Request::Type type, std::string url, std::string post_data,
                                           const void* owner, RequestCallback callback, ProgressCallback* progress,
                                           u16 timeout_seconds, HeaderList additional_headers)
{
  Request* req =
    new Request(owner, type, std::move(url), std::move(post_data), std::move(callback), progress, timeout_seconds);

  if (!EnsureInitialized(&req->error))
  {
    callback(HTTP_STATUS_ERROR, req->error, req->content_type, req->data);
    DeleteRequest(req);
    return;
  }

  if (!additional_headers.empty())
  {
    // Pre-reserve assuming 1 wchar per UTF-8 byte, plus 2 per header for \r\n.
    size_t total_length = 0;
    for (const char* header : additional_headers)
      total_length += std::strlen(header) + 2;
    req->additional_headers.reserve(total_length);

    for (const char* header : additional_headers)
    {
      StringUtil::AppendUTF8ToWideString(req->additional_headers, header);
      req->additional_headers += L"\r\n";
    }
  }

  StartOrAddRequest(req);
}

// Cracks the URL, opens a WinHTTP connection and request handle, and fires
// WinHttpSendRequest() to begin the async transfer. On success returns true and
// leaves the request in Started state; the remainder of the transfer is driven by
// HTTPStatusCallback(). On failure fires the callback directly and returns false.
bool HTTPDownloader::StartRequest(Request* req)
{
  std::wstring host_name;
  host_name.resize(req->url.size());
  req->object_name.resize(req->url.size());

  // TOOD: Put in InternalCreateRequest()? to avoid saving the url?
  URL_COMPONENTSW uc = {};
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host_name.data();
  uc.dwHostNameLength = static_cast<DWORD>(host_name.size());
  uc.lpszUrlPath = req->object_name.data();
  uc.dwUrlPathLength = static_cast<DWORD>(req->object_name.size());

  const std::wstring url_wide(StringUtil::UTF8StringToWideString(req->url));
  if (!WinHttpCrackUrl(url_wide.c_str(), static_cast<DWORD>(url_wide.size()), 0, &uc))
  {
    const DWORD err = GetLastError();
    ERROR_LOG("WinHttpCrackUrl() failed: {}", err);
    req->error.SetWin32("WinHttpCrackUrl() failed: ", err);
    req->callback(HTTP_STATUS_ERROR, req->error, req->content_type, req->data);
    DeleteRequest(req);
    return false;
  }

  host_name.resize(uc.dwHostNameLength);
  req->object_name.resize(uc.dwUrlPathLength);

  req->hConnection = WinHttpConnect(s_locals.hSession, host_name.c_str(), uc.nPort, 0);
  if (!req->hConnection)
  {
    const DWORD err = GetLastError();
    ERROR_LOG("Failed to start HTTP request for '{}': {}", req->url, err);
    req->error.SetWin32("WinHttpConnect() failed: ", err);
    req->callback(HTTP_STATUS_ERROR, req->error, req->content_type, req->data);
    DeleteRequest(req);
    return false;
  }

  const DWORD request_flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  req->hRequest = WinHttpOpenRequest(req->hConnection, (req->type == Request::Type::Post) ? L"POST" : L"GET",
                                     req->object_name.c_str(), NULL, NULL, NULL, request_flags);
  if (!req->hRequest)
  {
    const DWORD err = GetLastError();
    ERROR_LOG("WinHttpOpenRequest() failed: {}", err);
    req->error.SetWin32("WinHttpOpenRequest() failed: ", err);
    req->callback(HTTP_STATUS_ERROR, req->error, req->content_type, req->data);
    WinHttpCloseHandle(req->hConnection);
    DeleteRequest(req);
    return false;
  }

  if (!req->additional_headers.empty() &&
      !WinHttpAddRequestHeaders(req->hRequest, req->additional_headers.data(),
                                static_cast<DWORD>(req->additional_headers.size()),
                                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
  {
    const DWORD err = GetLastError();
    ERROR_LOG("WinHttpAddRequestHeaders() failed: {}", err);
    req->error.SetWin32("WinHttpAddRequestHeaders() failed: ", err);
    req->callback(HTTP_STATUS_ERROR, req->error, req->content_type, req->data);
    WinHttpCloseHandle(req->hRequest);
    WinHttpCloseHandle(req->hConnection);
    DeleteRequest(req);
    return false;
  }

  BOOL result;
  if (req->type == Request::Type::Post)
  {
    const std::wstring_view additional_headers(L"Content-Type: application/x-www-form-urlencoded\r\n");
    result = WinHttpSendRequest(req->hRequest, additional_headers.data(), static_cast<DWORD>(additional_headers.size()),
                                req->post_data.data(), static_cast<DWORD>(req->post_data.size()),
                                static_cast<DWORD>(req->post_data.size()), reinterpret_cast<DWORD_PTR>(req));
  }
  else
  {
    result = WinHttpSendRequest(req->hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                                reinterpret_cast<DWORD_PTR>(req));
  }

  if (DWORD err; !result && ((err = GetLastError()) != ERROR_IO_PENDING))
  {
    ERROR_LOG("WinHttpSendRequest() failed: {}", err);
    req->status_code = HTTP_STATUS_ERROR;
    req->error.SetWin32("WinHttpSendRequest() failed: ", err);
    req->state.store(Request::State::Complete);
  }

  DEV_LOG("Started HTTP request for '{}'", req->url);
  req->state = Request::State::Started;
  req->start_time = Timer::GetCurrentValue();
  req->last_update_time = req->start_time;
  return true;
}

// Initiates async teardown of the request. Closing hRequest causes WinHTTP to
// complete all pending I/O and then fire HANDLE_CLOSING, where the Request is freed.
// If hRequest was never opened (early failure path), falls back to closing hConnection
// and deleting the request directly.
void HTTPDownloader::CloseRequest(Request* req)
{
  if (req->hRequest != NULL)
  {
    // req will be freed by the callback.
    // the callback can fire immediately here if there's nothing running async, so don't touch req afterwards
    WinHttpCloseHandle(req->hRequest);
    return;
  }

  if (req->hConnection != NULL)
    WinHttpCloseHandle(req->hConnection);

  DeleteRequest(req);
}

void HTTPDownloader::DeleteRequest(Request* req)
{
  delete req;
}

#elif defined(USE_CURL)

// Lazily initialises the curl global state and multi handle exactly once via
// std::call_once, then starts the worker thread. Subsequent calls return
// immediately using the cached multi_handle check.
// Returns false (and populates error) if initialisation failed or previously failed.
bool HTTPDownloader::EnsureInitialized(Error* error)
{
  if (s_locals.multi_handle)
    return true;

  Error::Clear(error);

  std::call_once(s_locals.init_flag, [&error]() {
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
    {
      Error::SetStringView(error, "curl_global_init() failed");
      return;
    }

    s_locals.multi_handle = curl_multi_init();
    if (!s_locals.multi_handle)
    {
      Error::SetStringView(error, "curl_multi_init() failed");
      curl_global_cleanup();
      return;
    }

    s_locals.user_agent = Host::GetHTTPUserAgent();

    // Start the worker thread
    s_locals.worker_thread.Start(&HTTPDownloader::WorkerThreadEntryPoint);
  });

  if (!s_locals.multi_handle)
  {
    if (error && !error->IsValid())
      error->SetStringView(error, "cURL previously failed to initialize.");

    return false;
  }

  return true;
}

// Cancels all in-flight requests, then signals and joins the worker thread before
// cleaning up the curl multi handle. Order matters: requests must be cancelled
// (and their CloseRequest actions enqueued) before the worker is asked to exit,
// so it can drain the action queue and free all easy handles cleanly.
void HTTPDownloader::Shutdown()
{
  CancelRequestsForOwner(nullptr);

  // Signal worker thread to shutdown
  if (s_locals.worker_thread.Joinable())
  {
    {
      const std::unique_lock lock(s_locals.worker_queue_mutex);
      s_locals.worker_thread_shutdown.store(true, std::memory_order_release);

      // Should break the curl_multi_poll wait.
      if (s_locals.multi_handle)
        curl_multi_wakeup(s_locals.multi_handle);
    }

    s_locals.worker_thread.Join();
  }

  if (s_locals.multi_handle)
  {
    curl_multi_cleanup(s_locals.multi_handle);
    s_locals.multi_handle = nullptr;
  }
}

// CURLOPT_WRITEFUNCTION callback. Appends received bytes to the request buffer and
// lazily reads the Content-Length on the first invocation (curl does not guarantee
// it is available before data begins arriving).
size_t HTTPDownloader::WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  Request* req = static_cast<Request*>(userdata);
  const size_t current_size = req->data.size();
  const size_t transfer_size = size * nmemb;
  const size_t new_size = current_size + transfer_size;
  req->data.resize(new_size);
  req->last_update_time = Timer::GetCurrentValue();
  std::memcpy(&req->data[current_size], ptr, transfer_size);

  if (req->content_length == 0)
  {
    curl_off_t length;
    if (curl_easy_getinfo(req->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK)
      req->content_length = static_cast<u32>(length);
  }

  return transfer_size;
}

void HTTPDownloader::InternalCreateRequest(Request::Type type, std::string url, std::string post_data,
                                           const void* owner, RequestCallback callback, ProgressCallback* progress,
                                           u16 timeout_seconds, HeaderList additional_headers)
{
  Request* req =
    new Request(owner, type, std::move(url), std::move(post_data), std::move(callback), progress, timeout_seconds);

  if (!EnsureInitialized(&req->error))
  {
    callback(HTTP_STATUS_ERROR, req->error, req->content_type, req->data);
    DeleteRequest(req);
    return;
  }

  for (const char* header : additional_headers)
    req->header_list = curl_slist_append(req->header_list, header);

  StartOrAddRequest(req);
}

void HTTPDownloader::DeleteRequest(Request* req)
{
  if (req->header_list)
    curl_slist_free_all(req->header_list);
  delete req;
}

// Worker thread entry point. Runs the curl_multi_poll → ProcessQueuedActions →
// curl_multi_perform → ReadMultiResults loop until worker_thread_shutdown is set.
// SIGPIPE is blocked because OpenSSL may raise it on broken connections.
void HTTPDownloader::WorkerThreadEntryPoint()
{
  Threading::SetNameOfCurrentThread("HTTPDownloaderCurl Worker Thread");

  // Apparently OpenSSL can fire SIGPIPE...
  sigset_t block_mask = {};
  sigemptyset(&block_mask);
  sigaddset(&block_mask, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &block_mask, nullptr) != 0)
    WARNING_LOG("Failed to block SIGPIPE");

  while (!s_locals.worker_thread_shutdown.load(std::memory_order_acquire))
  {
    // Wait for activity with curl_multi_poll
    CURLMcode err = curl_multi_poll(s_locals.multi_handle, nullptr, 0, std::numeric_limits<int>::max(), nullptr);
    if (err != CURLM_OK)
      ERROR_LOG("curl_multi_poll() returned {}", static_cast<int>(err));

    // Process any queued actions
    ProcessQueuedActions();

    // Perform curl operations
    int running_handles;
    err = curl_multi_perform(s_locals.multi_handle, &running_handles);
    if (err != CURLM_OK)
      ERROR_LOG("curl_multi_perform() returned {}", static_cast<int>(err));

    // Read any results
    ReadMultiResults();
  }
}

// Consumer side of the worker_queue. Drains all pending Add / RemoveAndDelete
// actions posted by the main thread, applying them to the curl multi handle.
// Must only be called from the worker thread.
void HTTPDownloader::ProcessQueuedActions()
{
  const std::unique_lock lock(s_locals.worker_queue_mutex);
  while (!s_locals.worker_queue.empty())
  {
    const auto& [action, request] = s_locals.worker_queue.front();
    switch (action)
    {
      case QueueAction::Add:
      {
        const CURLMcode err = curl_multi_add_handle(s_locals.multi_handle, request->handle);
        if (err != CURLM_OK)
        {
          ERROR_LOG("curl_multi_add_handle() returned {}", static_cast<int>(err));
          request->error.SetStringFmt("curl_multi_add_handle() failed: {}", curl_multi_strerror(err));
          request->state.store(Request::State::Complete, std::memory_order_release);
        }
      }
      break;

      case QueueAction::RemoveAndDelete:
      {
        curl_multi_remove_handle(s_locals.multi_handle, request->handle);
        curl_easy_cleanup(request->handle);
        DeleteRequest(request);
      }
      break;
    }

    s_locals.worker_queue.pop_front();
  }
}

// Harvests completed transfers from curl_multi_info_read() and marks their
// Request objects as Complete so LockedPollRequests() can dispatch their callbacks.
// Must only be called from the worker thread.
void HTTPDownloader::ReadMultiResults()
{
  for (;;)
  {
    int msgq;
    struct CURLMsg* msg = curl_multi_info_read(s_locals.multi_handle, &msgq);
    if (!msg)
      break;

    if (msg->msg != CURLMSG_DONE)
    {
      WARNING_LOG("Unexpected multi message {}", static_cast<int>(msg->msg));
      continue;
    }

    Request* req;
    if (curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &req) != CURLE_OK)
    {
      ERROR_LOG("curl_easy_getinfo() failed");
      continue;
    }

    if (msg->data.result == CURLE_OK)
    {
      long response_code = 0;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &response_code);
      req->status_code = static_cast<s32>(response_code);

      char* content_type = nullptr;
      if (curl_easy_getinfo(req->handle, CURLINFO_CONTENT_TYPE, &content_type) == CURLE_OK && content_type)
        req->content_type = content_type;

      DEV_LOG("Request for '{}' returned status code {} and {} bytes", req->url, req->status_code, req->data.size());
    }
    else
    {
      ERROR_LOG("Request for '{}' returned error {}", req->url, static_cast<int>(msg->data.result));
      req->error.SetStringFmt("Request failed: {}", curl_easy_strerror(msg->data.result));
      req->status_code = HTTP_STATUS_ERROR;
    }

    req->state.store(Request::State::Complete, std::memory_order_release);
  }
}

// Configures a new curl easy handle for the request and enqueues an Add action
// for the worker thread. The worker thread owns the easy handle from this point;
// the main thread must not touch it directly.
bool HTTPDownloader::StartRequest(Request* req)
{
  req->handle = curl_easy_init();
  if (!req->handle)
  {
    ERROR_LOG("curl_easy_init() failed");
    req->error.SetStringView("curl_easy_init() failed");
    req->callback(HTTP_STATUS_ERROR, req->error, req->content_type, req->data);
    DeleteRequest(req);
    return false;
  }

  curl_easy_setopt(req->handle, CURLOPT_URL, req->url.c_str());
  curl_easy_setopt(req->handle, CURLOPT_WRITEFUNCTION, &HTTPDownloader::WriteCallback);
  curl_easy_setopt(req->handle, CURLOPT_WRITEDATA, req);
  curl_easy_setopt(req->handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(req->handle, CURLOPT_PRIVATE, req);
  curl_easy_setopt(req->handle, CURLOPT_FOLLOWLOCATION, 1L);

  if (req->header_list)
  {
    curl_easy_setopt(req->handle, CURLOPT_HTTPHEADER, req->header_list);

    // Only set the default user agent if the header list doesn't contain a User-Agent override.
    bool has_user_agent = false;
    for (const curl_slist* p = req->header_list; p; p = p->next)
    {
      if (StringUtil::Strncasecmp(p->data, "User-Agent:", 11) == 0)
      {
        has_user_agent = true;
        break;
      }
    }

    if (!has_user_agent)
      curl_easy_setopt(req->handle, CURLOPT_USERAGENT, s_locals.user_agent.c_str());
  }
  else
  {
    curl_easy_setopt(req->handle, CURLOPT_USERAGENT, s_locals.user_agent.c_str());
  }

  if (req->type == Request::Type::Post)
  {
    curl_easy_setopt(req->handle, CURLOPT_POST, 1L);
    curl_easy_setopt(req->handle, CURLOPT_POSTFIELDS, req->post_data.c_str());
  }

  DEV_LOG("Started HTTP request for '{}'", req->url);
  req->state.store(Request::State::Started, std::memory_order_release);
  req->start_time = Timer::GetCurrentValue();
  req->last_update_time = req->start_time;

  // Add to action queue for worker thread to process
  const std::unique_lock lock(s_locals.worker_queue_mutex);
  s_locals.worker_queue.emplace_back(QueueAction::Add, req);

  // Wake up worker thread
  curl_multi_wakeup(s_locals.multi_handle);

  return true;
}

// Enqueues a RemoveAndDelete action so the worker thread removes the easy handle
// from the multi handle, cleans it up, and frees the Request. The worker is then
// woken via curl_multi_wakeup() to process the action promptly.
void HTTPDownloader::CloseRequest(Request* req)
{
  DebugAssert(req->handle);

  // Add to action queue for worker thread to process
  const std::unique_lock lock(s_locals.worker_queue_mutex);
  s_locals.worker_queue.emplace_back(QueueAction::RemoveAndDelete, req);

  // Wake up worker thread
  curl_multi_wakeup(s_locals.multi_handle);
}

#endif