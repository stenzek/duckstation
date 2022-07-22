#pragma once
#include "http_downloader.h"
#include "common/thirdparty/thread_pool.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <curl/curl.h>

namespace Common {

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

} // namespace FrontendCommon
