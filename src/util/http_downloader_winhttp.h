// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "http_downloader.h"

#include "common/windows_headers.h"

#include <winhttp.h>

class HTTPDownloaderWinHttp final : public HTTPDownloader
{
public:
  HTTPDownloaderWinHttp();
  ~HTTPDownloaderWinHttp() override;

  bool Initialize(std::string user_agent);

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
