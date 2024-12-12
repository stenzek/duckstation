// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "http_downloader.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/progress_callback.h"
#include "common/string_util.h"
#include "common/timer.h"

LOG_CHANNEL(HTTPDownloader);

static constexpr float DEFAULT_TIMEOUT_IN_SECONDS = 30;
static constexpr u32 DEFAULT_MAX_ACTIVE_REQUESTS = 4;

const char HTTPDownloader::DEFAULT_USER_AGENT[] =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:85.0) Gecko/20100101 Firefox/85.0";

HTTPDownloader::HTTPDownloader()
  : m_timeout(DEFAULT_TIMEOUT_IN_SECONDS), m_max_active_requests(DEFAULT_MAX_ACTIVE_REQUESTS)
{
}

HTTPDownloader::~HTTPDownloader() = default;

void HTTPDownloader::SetTimeout(float timeout)
{
  m_timeout = timeout;
}

void HTTPDownloader::SetMaxActiveRequests(u32 max_active_requests)
{
  Assert(max_active_requests > 0);
  m_max_active_requests = max_active_requests;
}

void HTTPDownloader::CreateRequest(std::string url, Request::Callback callback, ProgressCallback* progress)
{
  Request* req = InternalCreateRequest();
  req->parent = this;
  req->type = Request::Type::Get;
  req->url = std::move(url);
  req->callback = std::move(callback);
  req->progress = progress;
  req->start_time = Timer::GetCurrentValue();

  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  if (LockedGetActiveRequestCount() < m_max_active_requests)
  {
    if (!StartRequest(req))
      return;
  }

  LockedAddRequest(req);
}

void HTTPDownloader::CreatePostRequest(std::string url, std::string post_data, Request::Callback callback,
                                       ProgressCallback* progress)
{
  Request* req = InternalCreateRequest();
  req->parent = this;
  req->type = Request::Type::Post;
  req->url = std::move(url);
  req->post_data = std::move(post_data);
  req->callback = std::move(callback);
  req->progress = progress;
  req->start_time = Timer::GetCurrentValue();

  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  if (LockedGetActiveRequestCount() < m_max_active_requests)
  {
    if (!StartRequest(req))
      return;
  }

  LockedAddRequest(req);
}

void HTTPDownloader::LockedPollRequests(std::unique_lock<std::mutex>& lock)
{
  if (m_pending_http_requests.empty())
    return;

  InternalPollRequests();

  const Timer::Value current_time = Timer::GetCurrentValue();
  u32 active_requests = 0;
  u32 unstarted_requests = 0;

  for (size_t index = 0; index < m_pending_http_requests.size();)
  {
    Request* req = m_pending_http_requests[index];
    if (req->state == Request::State::Pending)
    {
      unstarted_requests++;
      index++;
      continue;
    }

    if ((req->state == Request::State::Started || req->state == Request::State::Receiving) &&
        current_time >= req->start_time && Timer::ConvertValueToSeconds(current_time - req->start_time) >= m_timeout)
    {
      // request timed out
      ERROR_LOG("Request for '{}' timed out", req->url);

      req->state.store(Request::State::Cancelled);
      m_pending_http_requests.erase(m_pending_http_requests.begin() + index);
      lock.unlock();

      req->error.SetStringFmt("Request timed out after {} seconds.", m_timeout);
      req->callback(HTTP_STATUS_TIMEOUT, req->error, std::string(), Request::Data());

      CloseRequest(req);

      lock.lock();
      continue;
    }
    else if ((req->state == Request::State::Started || req->state == Request::State::Receiving) && req->progress &&
             req->progress->IsCancelled())
    {
      // request timed out
      ERROR_LOG("Request for '{}' cancelled", req->url);

      req->state.store(Request::State::Cancelled);
      m_pending_http_requests.erase(m_pending_http_requests.begin() + index);
      lock.unlock();

      req->error.SetStringView("Request was cancelled.");
      req->callback(HTTP_STATUS_CANCELLED, req->error, std::string(), Request::Data());

      CloseRequest(req);

      lock.lock();
      continue;
    }

    if (req->state != Request::State::Complete)
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
    VERBOSE_LOG("Request for '{}' complete, returned status code {} and {} bytes", req->url, req->status_code,
                req->data.size());
    m_pending_http_requests.erase(m_pending_http_requests.begin() + index);

    // run callback with lock unheld
    lock.unlock();
    if (req->status_code >= 0 && req->status_code != HTTP_STATUS_OK)
      req->error.SetStringFmt("Request failed with HTTP status code {}", req->status_code);
    else if (req->status_code < 0)
      DEV_LOG("Request failed with error {}", req->error.GetDescription());

    req->callback(req->status_code, req->error, req->content_type, std::move(req->data));
    CloseRequest(req);
    lock.lock();
  }

  // start new requests when we finished some
  if (unstarted_requests > 0 && active_requests < m_max_active_requests)
  {
    for (size_t index = 0; index < m_pending_http_requests.size();)
    {
      Request* req = m_pending_http_requests[index];
      if (req->state != Request::State::Pending)
      {
        index++;
        continue;
      }

      if (!StartRequest(req))
      {
        m_pending_http_requests.erase(m_pending_http_requests.begin() + index);
        continue;
      }

      active_requests++;
      index++;

      if (active_requests >= m_max_active_requests)
        break;
    }
  }
}

void HTTPDownloader::PollRequests()
{
  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  LockedPollRequests(lock);
}

void HTTPDownloader::WaitForAllRequests()
{
  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  while (!m_pending_http_requests.empty())
  {
    // Don't burn too much CPU.
    Timer::NanoSleep(1000000);
    LockedPollRequests(lock);
  }
}

void HTTPDownloader::LockedAddRequest(Request* request)
{
  m_pending_http_requests.push_back(request);
}

u32 HTTPDownloader::LockedGetActiveRequestCount()
{
  u32 count = 0;
  for (Request* req : m_pending_http_requests)
  {
    if (req->state == Request::State::Started || req->state == Request::State::Receiving)
      count++;
  }
  return count;
}

bool HTTPDownloader::HasAnyRequests()
{
  std::unique_lock<std::mutex> lock(m_pending_http_request_lock);
  return !m_pending_http_requests.empty();
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
