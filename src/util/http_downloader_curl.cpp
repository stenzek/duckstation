// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "http_downloader.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "common/threading.h"
#include "common/timer.h"

#include <algorithm>
#include <curl/curl.h>
#include <deque>
#include <functional>
#include <pthread.h>
#include <signal.h>
#include <thread>

LOG_CHANNEL(HTTPDownloader);

namespace {
class HTTPDownloaderCurl final : public HTTPDownloader
{
public:
  HTTPDownloaderCurl();
  ~HTTPDownloaderCurl() override;

  bool Initialize(std::string user_agent, Error* error);

protected:
  Request* InternalCreateRequest() override;
  bool StartRequest(HTTPDownloader::Request* request) override;
  void CloseRequest(HTTPDownloader::Request* request) override;

private:
  enum class QueueAction
  {
    Add,
    RemoveAndDelete
  };

  struct Request : HTTPDownloader::Request
  {
    CURL* handle = nullptr;
  };

  static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

  void WorkerThreadEntryPoint();

  void ProcessQueuedActions();
  void ReadMultiResults();

  CURLM* m_multi_handle = nullptr;
  std::string m_user_agent;

  // Background thread for curl operations
  std::thread m_worker_thread;
  std::atomic_bool m_worker_thread_shutdown{false};

  // Worker thread queue
  std::deque<std::pair<QueueAction, Request*>> m_worker_queue;
  std::mutex m_worker_queue_mutex;
};
} // namespace

HTTPDownloaderCurl::HTTPDownloaderCurl() : HTTPDownloader()
{
}

HTTPDownloaderCurl::~HTTPDownloaderCurl()
{
  // Signal worker thread to shutdown
  if (m_worker_thread.joinable())
  {
    {
      const std::unique_lock lock(m_worker_queue_mutex);
      m_worker_thread_shutdown.store(true, std::memory_order_release);

      // Should break the curl_multi_poll wait.
      if (m_multi_handle)
        curl_multi_wakeup(m_multi_handle);
    }

    m_worker_thread.join();
  }

  if (m_multi_handle)
    curl_multi_cleanup(m_multi_handle);
}

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent, Error* error)
{
  std::unique_ptr<HTTPDownloaderCurl> instance = std::make_unique<HTTPDownloaderCurl>();
  if (!instance->Initialize(std::move(user_agent), error))
    instance.reset();

  return instance;
}

static bool s_curl_initialized = false;
static std::once_flag s_curl_initialized_once_flag;

bool HTTPDownloaderCurl::Initialize(std::string user_agent, Error* error)
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
      Error::SetStringView(error, "curl_global_init() failed");
      return false;
    }
  }

  m_multi_handle = curl_multi_init();
  if (!m_multi_handle)
  {
    Error::SetStringView(error, "curl_multi_init() failed");
    return false;
  }

  m_user_agent = std::move(user_agent);

  // Start the worker thread
  m_worker_thread = std::thread(&HTTPDownloaderCurl::WorkerThreadEntryPoint, this);
  return true;
}

size_t HTTPDownloaderCurl::WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
  Request* req = static_cast<Request*>(userdata);
  const size_t current_size = req->data.size();
  const size_t transfer_size = size * nmemb;
  const size_t new_size = current_size + transfer_size;
  req->data.resize(new_size);
  req->start_time = Timer::GetCurrentValue();
  std::memcpy(&req->data[current_size], ptr, transfer_size);

  if (req->content_length == 0)
  {
    curl_off_t length;
    if (curl_easy_getinfo(req->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK)
      req->content_length = static_cast<u32>(length);
  }

  return nmemb;
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

void HTTPDownloaderCurl::WorkerThreadEntryPoint()
{
  Threading::SetNameOfCurrentThread("HTTPDownloaderCurl Worker Thread");

  // Apparently OpenSSL can fire SIGPIPE...
  sigset_t block_mask = {};
  sigemptyset(&block_mask);
  sigaddset(&block_mask, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &block_mask, nullptr) != 0)
    WARNING_LOG("Failed to block SIGPIPE");

  while (!m_worker_thread_shutdown.load(std::memory_order_acquire))
  {
    // Wait for activity with curl_multi_poll
    int numfds = 0;
    curl_multi_poll(m_multi_handle, nullptr, 0, 1000, &numfds);

    // Process any queued actions
    ProcessQueuedActions();

    // Perform curl operations
    int running_handles;
    const CURLMcode err = curl_multi_perform(m_multi_handle, &running_handles);
    if (err != CURLM_OK)
      ERROR_LOG("curl_multi_perform() returned {}", static_cast<int>(err));

    // Read any results
    ReadMultiResults();
  }
}

void HTTPDownloaderCurl::ProcessQueuedActions()
{
  const std::unique_lock lock(m_worker_queue_mutex);
  while (!m_worker_queue.empty())
  {
    const auto& [action, request] = m_worker_queue.front();
    switch (action)
    {
      case QueueAction::Add:
      {
        const CURLMcode err = curl_multi_add_handle(m_multi_handle, request->handle);
        if (err != CURLM_OK)
        {
          ERROR_LOG("curl_multi_add_handle() returned {}", static_cast<int>(err));
          request->error.SetStringFmt("curl_multi_add_handle() failed: {}", curl_multi_strerror(err));
          request->state.store(Request::State::Complete, std::memory_order_release);
        }
      }
      break;

      case QueueAction::RemoveAndDelete:
      {
        curl_multi_remove_handle(m_multi_handle, request->handle);
        curl_easy_cleanup(request->handle);
        delete request;
      }
      break;
    }

    m_worker_queue.pop_front();
  }
}

void HTTPDownloaderCurl::ReadMultiResults()
{
  for (;;)
  {
    int msgq;
    struct CURLMsg* msg = curl_multi_info_read(m_multi_handle, &msgq);
    if (!msg)
      break;

    if (msg->msg != CURLMSG_DONE)
    {
      WARNING_LOG("Unexpected multi message {}", static_cast<int>(msg->msg));
      continue;
    }

    Request* req;
    if (curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &req) != CURLE_OK)
    {
      ERROR_LOG("curl_easy_getinfo() failed");
      continue;
    }

    if (msg->data.result == CURLE_OK)
    {
      long response_code = 0;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &response_code);
      req->status_code = static_cast<s32>(response_code);

      char* content_type = nullptr;
      if (curl_easy_getinfo(req->handle, CURLINFO_CONTENT_TYPE, &content_type) == CURLE_OK && content_type)
        req->content_type = content_type;

      DEV_LOG("Request for '{}' returned status code {} and {} bytes", req->url, req->status_code, req->data.size());
    }
    else
    {
      ERROR_LOG("Request for '{}' returned error {}", req->url, static_cast<int>(msg->data.result));
      req->error.SetStringFmt("Request failed: {}", curl_easy_strerror(msg->data.result));
    }

    req->state.store(Request::State::Complete, std::memory_order_release);
  }
}

bool HTTPDownloaderCurl::StartRequest(HTTPDownloader::Request* request)
{
  Request* req = static_cast<Request*>(request);
  curl_easy_setopt(req->handle, CURLOPT_URL, request->url.c_str());
  curl_easy_setopt(req->handle, CURLOPT_USERAGENT, m_user_agent.c_str());
  curl_easy_setopt(req->handle, CURLOPT_WRITEFUNCTION, &HTTPDownloaderCurl::WriteCallback);
  curl_easy_setopt(req->handle, CURLOPT_WRITEDATA, req);
  curl_easy_setopt(req->handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(req->handle, CURLOPT_PRIVATE, req);
  curl_easy_setopt(req->handle, CURLOPT_FOLLOWLOCATION, 1L);

  if (request->type == Request::Type::Post)
  {
    curl_easy_setopt(req->handle, CURLOPT_POST, 1L);
    curl_easy_setopt(req->handle, CURLOPT_POSTFIELDS, request->post_data.c_str());
  }

  DEV_LOG("Started HTTP request for '{}'", req->url);
  req->state.store(Request::State::Started, std::memory_order_release);
  req->start_time = Timer::GetCurrentValue();

  // Add to action queue for worker thread to process
  const std::unique_lock lock(m_worker_queue_mutex);
  m_worker_queue.emplace_back(QueueAction::Add, req);

  // Wake up worker thread
  curl_multi_wakeup(m_multi_handle);

  return true;
}

void HTTPDownloaderCurl::CloseRequest(HTTPDownloader::Request* request)
{
  Request* req = static_cast<Request*>(request);
  DebugAssert(req->handle);

  // Add to action queue for worker thread to process
  const std::unique_lock lock(m_worker_queue_mutex);
  m_worker_queue.emplace_back(QueueAction::RemoveAndDelete, req);

  // Wake up worker thread
  curl_multi_wakeup(m_multi_handle);
}
