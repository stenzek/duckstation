// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "http_downloader.h"

#include <atomic>
#include <curl/curl.h>
#include <memory>
#include <mutex>

class HTTPDownloaderCurl final : public HTTPDownloader
{
public:
  HTTPDownloaderCurl();
  ~HTTPDownloaderCurl() override;

  bool Initialize(std::string user_agent);

protected:
  Request* InternalCreateRequest() override;
  void InternalPollRequests() override;
  bool StartRequest(HTTPDownloader::Request* request) override;
  void CloseRequest(HTTPDownloader::Request* request) override;

private:
  struct Request : HTTPDownloader::Request
  {
    CURL* handle = nullptr;
  };

  static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

  CURLM* m_multi_handle = nullptr;
  std::string m_user_agent;
};
