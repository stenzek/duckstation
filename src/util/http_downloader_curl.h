// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "http_downloader.h"

#include "common/thirdparty/thread_pool.h"

#include <atomic>
#include <curl/curl.h>
#include <memory>
#include <mutex>

class HTTPDownloaderCurl final : public HTTPDownloader
{
public:
  HTTPDownloaderCurl();
  ~HTTPDownloaderCurl() override;

  bool Initialize(const char* user_agent);

protected:
  Request* InternalCreateRequest() override;
  void InternalPollRequests() override;
  bool StartRequest(HTTPDownloader::Request* request) override;
  void CloseRequest(HTTPDownloader::Request* request) override;

private:
  struct Request : HTTPDownloader::Request
  {
    CURL* handle = nullptr;
    std::atomic_bool closed{false};
  };

  static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);
  void ProcessRequest(Request* req);

  std::string m_user_agent;
  std::unique_ptr<cb::ThreadPool> m_thread_pool;
  std::mutex m_cancel_mutex;
};
