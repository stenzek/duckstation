#include "http_downloader_curl.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"
#include <algorithm>
#include <functional>
Log_SetChannel(HTTPDownloaderCurl);

namespace FrontendCommon {

HTTPDownloaderCurl::HTTPDownloaderCurl() : HTTPDownloader() {}

HTTPDownloaderCurl::~HTTPDownloaderCurl() = default;

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create()
{
  std::unique_ptr<HTTPDownloaderCurl> instance(std::make_unique<HTTPDownloaderCurl>());
  if (!instance->Initialize())
    return {};

  return instance;
}

static bool s_curl_initialized = false;
static std::once_flag s_curl_initialized_once_flag;

bool HTTPDownloaderCurl::Initialize()
{
  if (!s_curl_initialized)
  {
    std::call_once(s_curl_initialized_once_flag, []() {
      s_curl_initialized = curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK;
      if (s_curl_initialized)
      {
        std::atexit([]() {
          curl_global_cleanup();
          s_curl_initialized = false;
        });
      }
    });
    if (!s_curl_initialized)
    {
      Log_ErrorPrint("curl_global_init() failed");
      return false;
    }
  }
  m_thread_pool = std::make_unique<cb::ThreadPool>(m_max_active_requests);
  return true;
}
/*
void CALLBACK HTTPDownloaderCurl::HTTPStatusCallback(HINTERNET hRequest, DWORD_PTR dwContext, DWORD dwInternetStatus,
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

      HTTPDownloaderCurl* parent = static_cast<HTTPDownloaderCurl*>(req->parent);
      std::unique_lock<std::mutex> lock(parent->m_pending_http_request_lock);
      Assert(std::none_of(parent->m_pending_http_requests.begin(), parent->m_pending_http_requests.end(),
                          [req](HTTPDownloader::Request* it) { return it == req; }));

      // we can clean up the connection as well
      DebugAssert(req->hConnection != NULL);
      WinHttpCloseHandle(req->hConnection);
      delete req;
      return;
    }

    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
    {
      const WINHTTP_ASYNC_RESULT* res = reinterpret_cast<const WINHTTP_ASYNC_RESULT*>(lpvStatusInformation);
      Log_ErrorPrintf("WinHttp async function %p returned error %u", res->dwResult, res->dwError);
      req->status_code = -1;
      req->state.store(Request::State::Complete);
      return;
    }
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    {
      Log_DevPrintf("SendRequest complete");
      if (!WinHttpReceiveResponse(hRequest, nullptr))
      {
        Log_ErrorPrintf("WinHttpReceiveResponse() failed: %u", GetLastError());
        req->status_code = -1;
        req->state.store(Request::State::Complete);
      }

      return;
    }
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
    {
      Log_DevPrintf("Headers available");

      DWORD buffer_size = sizeof(req->status_code);
      if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &req->status_code, &buffer_size, WINHTTP_NO_HEADER_INDEX))
      {
        Log_ErrorPrintf("WinHttpQueryHeaders() for status code failed: %u", GetLastError());
        req->status_code = -1;
        req->state.store(Request::State::Complete);
        return;
      }

      buffer_size = sizeof(req->content_length);
      if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &req->content_length, &buffer_size,
                               WINHTTP_NO_HEADER_INDEX))
      {
        if (GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND)
          Log_WarningPrintf("WinHttpQueryHeaders() for content length failed: %u", GetLastError());

        req->content_length = 0;
      }

      Log_DevPrintf("Status code %d, content-length is %u", req->status_code, req->content_length);
      req->data.reserve(req->content_length);
      req->state = Request::State::Receiving;

      // start reading
      if (!WinHttpQueryDataAvailable(hRequest, nullptr) && GetLastError() != ERROR_IO_PENDING)
      {
        Log_ErrorPrintf("WinHttpQueryDataAvailable() failed: %u", GetLastError());
        req->status_code = -1;
        req->state.store(Request::State::Complete);
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
        Log_DevPrintf("End of request '%s', %zu bytes received", req->url.c_str(), req->data.size());
        req->state.store(Request::State::Complete);
        return;
      }

      // start the transfer
      Log_DevPrintf("%u bytes available", bytes_available);
      req->io_position = static_cast<u32>(req->data.size());
      req->data.resize(req->io_position + bytes_available);
      if (!WinHttpReadData(hRequest, req->data.data() + req->io_position, bytes_available, nullptr) &&
          GetLastError() != ERROR_IO_PENDING)
      {
        Log_ErrorPrintf("WinHttpReadData() failed: %u", GetLastError());
        req->status_code = -1;
        req->state.store(Request::State::Complete);
      }

      return;
    }
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
    {
      Log_DevPrintf("Read of %u complete", dwStatusInformationLength);

      const u32 new_size = req->io_position + dwStatusInformationLength;
      Assert(new_size <= req->data.size());
      req->data.resize(new_size);
      req->start_time = Common::Timer::GetValue();

      if (!WinHttpQueryDataAvailable(hRequest, nullptr) && GetLastError() != ERROR_IO_PENDING)
      {
        Log_ErrorPrintf("WinHttpQueryDataAvailable() failed: %u", GetLastError());
        req->status_code = -1;
        req->state.store(Request::State::Complete);
      }

      return;
    }
    default:
      // unhandled, ignore
      return;
  }
}
*/

size_t HTTPDownloaderCurl::WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  Request* req = static_cast<Request*>(userdata);
  const size_t current_size = req->data.size();
  const size_t transfer_size = size * nmemb;
  const size_t new_size = current_size + transfer_size;
  req->data.resize(new_size);
  std::memcpy(&req->data[current_size], ptr, transfer_size);
  return nmemb;
}

void HTTPDownloaderCurl::ProcessRequest(Request* req)
{
  std::unique_lock<std::mutex> cancel_lock(m_cancel_mutex);
  if (req->closed.load())
    return;

  cancel_lock.unlock();

  req->start_time = Common::Timer::GetValue();
  int ret = curl_easy_perform(req->handle);
  if (ret == CURLE_OK)
  {
    long response_code = 0;
    curl_easy_getinfo(req->handle, CURLINFO_RESPONSE_CODE, &response_code);
    req->status_code = static_cast<s32>(response_code);
    Log_DevPrintf("Request for '%s' returned status code %d and %zu bytes",
                  req->url.c_str(), req->status_code, req->data.size());
  }
  else
  {
    Log_ErrorPrintf("Request for '%s' returned %d", req->url.c_str(), ret);
  }

  curl_easy_cleanup(req->handle);

  cancel_lock.lock();
  req->state = Request::State::Complete;
  if (req->closed.load())
    delete req;
  else
    req->closed.store(true);
}

HTTPDownloader::Request* HTTPDownloaderCurl::InternalCreateRequest()
{
  Request* req = new Request();
  req->handle = curl_easy_init();
  if (!req->handle)
  {
    delete req;
    return nullptr;
  }

  return req;
}

void HTTPDownloaderCurl::InternalPollRequests()
{
  // noop - uses thread pool
}

bool HTTPDownloaderCurl::StartRequest(HTTPDownloader::Request* request)
{
  Request* req = static_cast<Request*>(request);
  curl_easy_setopt(req->handle, CURLOPT_URL, request->url.c_str());
  // curl_easy_setopt(req->handle, CURLOPT_USERAGENT, m_user_agent.c_str());
  curl_easy_setopt(req->handle, CURLOPT_WRITEFUNCTION, &HTTPDownloaderCurl::WriteCallback);
  curl_easy_setopt(req->handle, CURLOPT_WRITEDATA, req);

  if (request->type == Request::Type::Post)
  {
    curl_easy_setopt(req->handle, CURLOPT_POST, 1L);
    curl_easy_setopt(req->handle, CURLOPT_POSTFIELDS, request->post_data.c_str());
  }

  Log_DevPrintf("Started HTTP request for '%s'", req->url.c_str());
  req->state = Request::State::Started;
  req->start_time = Common::Timer::GetValue();
  m_thread_pool->Schedule(std::bind(&HTTPDownloaderCurl::ProcessRequest, this, req));
  return true;
}

void HTTPDownloaderCurl::CloseRequest(HTTPDownloader::Request* request)
{
  std::unique_lock<std::mutex> cancel_lock(m_cancel_mutex);
  Request* req = static_cast<Request*>(request);
  if (req->closed.load())
    delete req;
  else
    req->closed.store(true);
}

} // namespace FrontendCommon
