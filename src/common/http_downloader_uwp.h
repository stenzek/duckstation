#pragma once
#pragma once
#include "http_downloader.h"

#include "common/windows_headers.h"

#include <winrt/windows.Web.Http.h>

namespace Common {

class HTTPDownloaderUWP final : public HTTPDownloader
{
public:
  HTTPDownloaderUWP(std::string user_agent);
  ~HTTPDownloaderUWP() override;

protected:
  Request* InternalCreateRequest() override;
  void InternalPollRequests() override;
  bool StartRequest(HTTPDownloader::Request* request) override;
  void CloseRequest(HTTPDownloader::Request* request) override;

private:
  struct Request : HTTPDownloader::Request
  {
    std::wstring object_name;
    winrt::Windows::Web::Http::HttpClient client;
    winrt::Windows::Foundation::IAsyncOperationWithProgress<winrt::Windows::Web::Http::HttpResponseMessage,
                                                            winrt::Windows::Web::Http::HttpProgress>
      request_async{nullptr};
    winrt::Windows::Foundation::IAsyncOperationWithProgress<winrt::Windows::Storage::Streams::IBuffer, uint64_t>
      receive_async{};
  };

  std::string m_user_agent;
};

} // namespace FrontendCommon