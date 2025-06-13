// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "http_downloader.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/timer.h"

#include <algorithm>

#include "common/windows_headers.h"

#include <winhttp.h>

namespace {
class HTTPDownloaderWinHttp final : public HTTPDownloader
{
public:
  HTTPDownloaderWinHttp();
  ~HTTPDownloaderWinHttp() override;

  bool Initialize(std::string user_agent, Error* error);

protected:
  Request* InternalCreateRequest() override;
  void InternalPollRequests() override;
  bool StartRequest(HTTPDownloader::Request* request) override;
  void CloseRequest(HTTPDownloader::Request* request) override;

private:
  struct Request : HTTPDownloader::Request
  {
    std::wstring object_name;
    HINTERNET hConnection = NULL;
    HINTERNET hRequest = NULL;
    u32 io_position = 0;
  };

  static void CALLBACK HTTPStatusCallback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus,
                                          LPVOID lpvStatusInformation, DWORD dwStatusInformationLength);

  HINTERNET m_hSession = NULL;
};
} // namespace

LOG_CHANNEL(HTTPDownloader);

HTTPDownloaderWinHttp::HTTPDownloaderWinHttp() : HTTPDownloader()
{
}

HTTPDownloaderWinHttp::~HTTPDownloaderWinHttp()
{
  if (m_hSession)
  {
    WinHttpSetStatusCallback(m_hSession, nullptr, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, NULL);
    WinHttpCloseHandle(m_hSession);
  }
}

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent, Error* error)
{
  std::unique_ptr<HTTPDownloaderWinHttp> instance(std::make_unique<HTTPDownloaderWinHttp>());
  if (!instance->Initialize(std::move(user_agent), error))
    instance.reset();

  return instance;
}

bool HTTPDownloaderWinHttp::Initialize(std::string user_agent, Error* error)
{
  static constexpr DWORD dwAccessType = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;

  m_hSession = WinHttpOpen(StringUtil::UTF8StringToWideString(user_agent).c_str(), dwAccessType, nullptr, nullptr,
                           WINHTTP_FLAG_ASYNC);
  if (m_hSession == NULL)
  {
    Error::SetWin32(error, "WinHttpOpen() failed: ", GetLastError());
    return false;
  }

  const DWORD notification_flags = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
                                   WINHTTP_CALLBACK_FLAG_HANDLES | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE;
  if (WinHttpSetStatusCallback(m_hSession, HTTPStatusCallback, notification_flags, NULL) ==
      WINHTTP_INVALID_STATUS_CALLBACK)
  {
    Error::SetWin32(error, "WinHttpSetStatusCallback() failed: ", GetLastError());
    return false;
  }

  return true;
}

void CALLBACK HTTPDownloaderWinHttp::HTTPStatusCallback(HINTERNET hRequest, DWORD_PTR dwContext, DWORD dwInternetStatus,
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

      HTTPDownloaderWinHttp* parent = static_cast<HTTPDownloaderWinHttp*>(req->parent);
      std::unique_lock lock(parent->m_pending_http_request_lock);
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
      ERROR_LOG("WinHttp async function {} returned error {}", res->dwResult, res->dwError);
      req->status_code = HTTP_STATUS_ERROR;
      req->error.SetStringFmt("WinHttp async function {} returned error {}", res->dwResult, res->dwError);
      req->state.store(Request::State::Complete);
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
        req->state.store(Request::State::Complete);
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
        req->state.store(Request::State::Complete);
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
      req->state = Request::State::Receiving;

      // start reading
      if (!WinHttpQueryDataAvailable(hRequest, nullptr) && GetLastError() != ERROR_IO_PENDING)
      {
        const DWORD err = GetLastError();
        ERROR_LOG("WinHttpQueryDataAvailable() failed: {}", err);
        req->status_code = HTTP_STATUS_ERROR;
        req->error.SetWin32("WinHttpQueryDataAvailable() failed: ", err);
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
        DEV_LOG("End of request '{}', {} bytes received", req->url, req->data.size());
        req->state.store(Request::State::Complete);
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
        req->state.store(Request::State::Complete);
      }

      return;
    }
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
    {
      DEBUG_LOG("Read of {} complete", dwStatusInformationLength);

      const u32 new_size = req->io_position + dwStatusInformationLength;
      Assert(new_size <= req->data.size());
      req->data.resize(new_size);
      req->start_time = Timer::GetCurrentValue();

      if (!WinHttpQueryDataAvailable(hRequest, nullptr) && GetLastError() != ERROR_IO_PENDING)
      {
        const DWORD err = GetLastError();
        ERROR_LOG("WinHttpQueryDataAvailable() failed: {}", err);
        req->status_code = HTTP_STATUS_ERROR;
        req->error.SetWin32("WinHttpQueryDataAvailable() failed: ", err);
        req->state.store(Request::State::Complete);
      }

      return;
    }
    default:
      // unhandled, ignore
      return;
  }
}

HTTPDownloader::Request* HTTPDownloaderWinHttp::InternalCreateRequest()
{
  Request* req = new Request();
  return req;
}

void HTTPDownloaderWinHttp::InternalPollRequests()
{
  // noop - it uses windows's worker threads
}

bool HTTPDownloaderWinHttp::StartRequest(HTTPDownloader::Request* request)
{
  Request* req = static_cast<Request*>(request);

  std::wstring host_name;
  host_name.resize(req->url.size());
  req->object_name.resize(req->url.size());

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
    req->callback(HTTP_STATUS_ERROR, req->error, std::string(), req->data);
    delete req;
    return false;
  }

  host_name.resize(uc.dwHostNameLength);
  req->object_name.resize(uc.dwUrlPathLength);

  req->hConnection = WinHttpConnect(m_hSession, host_name.c_str(), uc.nPort, 0);
  if (!req->hConnection)
  {
    const DWORD err = GetLastError();
    ERROR_LOG("Failed to start HTTP request for '{}': {}", req->url, err);
    req->error.SetWin32("WinHttpConnect() failed: ", err);
    req->callback(HTTP_STATUS_ERROR, req->error, std::string(), req->data);
    delete req;
    return false;
  }

  const DWORD request_flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  req->hRequest =
    WinHttpOpenRequest(req->hConnection, (req->type == HTTPDownloader::Request::Type::Post) ? L"POST" : L"GET",
                       req->object_name.c_str(), NULL, NULL, NULL, request_flags);
  if (!req->hRequest)
  {
    const DWORD err = GetLastError();
    ERROR_LOG("WinHttpOpenRequest() failed: {}", err);
    req->error.SetWin32("WinHttpSendRequest() failed: ", err);
    WinHttpCloseHandle(req->hConnection);
    return false;
  }

  BOOL result;
  if (req->type == HTTPDownloader::Request::Type::Post)
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

  if (!result && GetLastError() != ERROR_IO_PENDING)
  {
    const DWORD err = GetLastError();
    ERROR_LOG("WinHttpSendRequest() failed: {}", err);
    req->status_code = HTTP_STATUS_ERROR;
    req->error.SetWin32("WinHttpSendRequest() failed: ", err);
    req->state.store(Request::State::Complete);
  }

  DEV_LOG("Started HTTP request for '{}'", req->url);
  req->state = Request::State::Started;
  req->start_time = Timer::GetCurrentValue();
  return true;
}

void HTTPDownloaderWinHttp::CloseRequest(HTTPDownloader::Request* request)
{
  Request* req = static_cast<Request*>(request);

  if (req->hRequest != NULL)
  {
    // req will be freed by the callback.
    // the callback can fire immediately here if there's nothing running async, so don't touch req afterwards
    WinHttpCloseHandle(req->hRequest);
    return;
  }

  if (req->hConnection != NULL)
    WinHttpCloseHandle(req->hConnection);

  delete req;
}
