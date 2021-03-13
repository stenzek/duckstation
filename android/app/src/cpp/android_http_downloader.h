#pragma once
#include "common/thirdparty/thread_pool.h"
#include "frontend-common/http_downloader.h"
#include <atomic>
#include <jni.h>
#include <memory>
#include <mutex>

namespace FrontendCommon {

class AndroidHTTPDownloader final : public HTTPDownloader
{
public:
  AndroidHTTPDownloader();
  ~AndroidHTTPDownloader() override;

  bool Initialize();

protected:
  Request* InternalCreateRequest() override;
  void InternalPollRequests() override;
  bool StartRequest(HTTPDownloader::Request* request) override;
  void CloseRequest(HTTPDownloader::Request* request) override;

private:
  struct Request : HTTPDownloader::Request
  {
    std::atomic_bool closed{false};
  };

  void ProcessRequest(Request* req);

  std::unique_ptr<cb::ThreadPool> m_thread_pool;
  std::mutex m_cancel_mutex;

  jclass m_URLDownloader_class = nullptr;
  jmethodID m_URLDownloader_constructor = nullptr;
  jmethodID m_URLDownloader_get = nullptr;
  jmethodID m_URLDownloader_post = nullptr;
  jmethodID m_URLDownloader_getStatusCode = nullptr;
  jmethodID m_URLDownloader_getData = nullptr;
};

} // namespace FrontendCommon
