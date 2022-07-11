#include "http_downloader_uwp.h"
#include "assert.h"
#include "log.h"
#include "string_util.h"
#include "timer.h"
#include <algorithm>
Log_SetChannel(HTTPDownloaderWinHttp);

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Web::Http;

namespace Common {

HTTPDownloaderUWP::HTTPDownloaderUWP(std::string user_agent) : HTTPDownloader(), m_user_agent(std::move(user_agent)) {}

HTTPDownloaderUWP::~HTTPDownloaderUWP() = default;

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(const char* user_agent)
{
  std::string user_agent_str;
  if (user_agent)
    user_agent_str = user_agent;

  return std::make_unique<HTTPDownloaderUWP>(user_agent ? std::string(user_agent) : std::string());
}

HTTPDownloader::Request* HTTPDownloaderUWP::InternalCreateRequest()
{
  Request* req = new Request();
  return req;
}

void HTTPDownloaderUWP::InternalPollRequests()
{
  // noop - uses async
}

bool HTTPDownloaderUWP::StartRequest(HTTPDownloader::Request* request)
{
  Request* req = static_cast<Request*>(request);

  try
  {
    const std::wstring url_wide(StringUtil::UTF8StringToWideString(req->url));
    const Uri uri(url_wide);

    if (!m_user_agent.empty() &&
        !req->client.DefaultRequestHeaders().UserAgent().TryParseAdd(StringUtil::UTF8StringToWideString(m_user_agent)))
    {
      Log_WarningPrintf("Failed to set user agent to '%s'", m_user_agent.c_str());
    }

    if (req->type == Request::Type::Post)
    {
      const HttpStringContent post_content(StringUtil::UTF8StringToWideString(req->post_data),
                                           winrt::Windows::Storage::Streams::UnicodeEncoding::Utf8,
                                           L"application/x-www-form-urlencoded");
      req->request_async = req->client.PostAsync(uri, post_content);
    }
    else
    {
      req->request_async = req->client.GetAsync(uri);
    }

    req->request_async.Completed(
      [req](const IAsyncOperationWithProgress<HttpResponseMessage, HttpProgress>& operation, AsyncStatus status) {
        if (status == AsyncStatus::Completed)
        {
          Log_DevPrintf("Request for '%s' completed start portion", req->url.c_str());
          try
          {
            req->state.store(Request::State::Receiving);
            req->start_time = Common::Timer::GetCurrentValue();

            const HttpResponseMessage response(req->request_async.get());
            req->status_code = static_cast<s32>(response.StatusCode());

            const IHttpContent content(response.Content());
            req->receive_async = content.ReadAsBufferAsync();
            req->receive_async.Completed(
              [req](
                const IAsyncOperationWithProgress<winrt::Windows::Storage::Streams::IBuffer, uint64_t>& inner_operation,
                AsyncStatus inner_status) {
                if (inner_status == AsyncStatus::Completed)
                {
                  const winrt::Windows::Storage::Streams::IBuffer buffer(inner_operation.get());
                  if (buffer && buffer.Length() > 0)
                  {
                    req->data.resize(buffer.Length());
                    std::memcpy(req->data.data(), buffer.data(), req->data.size());
                  }

                  Log_DevPrintf("End of request '%s', %zu bytes received", req->url.c_str(), req->data.size());
                  req->state.store(Request::State::Complete);
                }
                else if (inner_status == AsyncStatus::Canceled)
                {
                  // don't do anything, the request has been freed
                }
                else
                {
                  Log_ErrorPrintf("Request for '%s' failed during recieve phase: %08X", req->url.c_str(),
                                  inner_operation.ErrorCode().value);
                  req->status_code = -1;
                  req->state.store(Request::State::Complete);
                }
              });
          }
          catch (const winrt::hresult_error& err)
          {
            Log_ErrorPrintf("Failed to receive HTTP request for '%s': %08X %s", req->url.c_str(), err.code(),
                            StringUtil::WideStringToUTF8String(err.message()).c_str());
            req->status_code = -1;
            req->state.store(Request::State::Complete);
          }

          req->receive_async = nullptr;
        }
        else if (status == AsyncStatus::Canceled)
        {
          // don't do anything, the request has been freed
        }
        else
        {
          Log_ErrorPrintf("Request for '%s' failed during start phase: %08X", req->url.c_str(),
                          operation.ErrorCode().value);
          req->status_code = -1;
          req->state.store(Request::State::Complete);
        }

        req->request_async = nullptr;
      });
  }
  catch (const winrt::hresult_error& err)
  {
    Log_ErrorPrintf("Failed to start HTTP request for '%s': %08X %s", req->url.c_str(), err.code(),
                    StringUtil::WideStringToUTF8String(err.message()).c_str());
    req->callback(-1, req->data);
    delete req;
    return false;
  }

  Log_DevPrintf("Started HTTP request for '%s'", req->url.c_str());
  req->state = Request::State::Started;
  req->start_time = Common::Timer::GetCurrentValue();
  return true;
}

void HTTPDownloaderUWP::CloseRequest(HTTPDownloader::Request* request)
{
  Request* req = static_cast<Request*>(request);
  if (req->request_async)
    req->request_async.Cancel();
  if (req->receive_async)
    req->receive_async.Cancel();

  req->client.Close();
  delete req;
}

} // namespace FrontendCommon