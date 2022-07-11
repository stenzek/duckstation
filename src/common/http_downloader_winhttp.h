#pragma once
#include "http_downloader.h"

#include "common/windows_headers.h"

#include <winhttp.h>

namespace Common {

class HTTPDownloaderWinHttp final : public HTTPDownloader
{
public:
  HTTPDownloaderWinHttp();
  ~HTTPDownloaderWinHttp() override;

  bool Initialize(const char* user_agent);

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

} // namespace FrontendCommon