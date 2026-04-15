// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/error.h"
#include "common/types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class ProgressCallback;

class HTTPDownloader
{
public:
  using HeaderList = std::span<const char* const>;

  static constexpr u64 WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_MS = 16;
  static constexpr u64 WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_NS = WAIT_FOR_ALL_REQUESTS_POLL_INTERVAL_MS * 1000000;

  enum : s32
  {
    HTTP_STATUS_CANCELLED = -3,
    HTTP_STATUS_TIMEOUT = -2,
    HTTP_STATUS_ERROR = -1,
    HTTP_STATUS_OK = 200
  };

  struct Request
  {
    using Data = std::vector<u8>;
    using Callback =
      std::function<void(s32 status_code, const Error& error, const std::string& content_type, Data data)>;

    enum class Type : u8
    {
      Get,
      Post,
    };

    enum class State : u8
    {
      Pending,
      Cancelled,
      Started,
      Receiving,
      Complete,
    };

    Request(HTTPDownloader* parent, Type type, std::string url, std::string post_data, Callback callback,
            ProgressCallback* progress, u16 timeout_seconds);
    virtual ~Request();

    HTTPDownloader* parent;
    Callback callback;
    ProgressCallback* progress;
    std::string url;
    std::string post_data;
    std::string content_type;
    Data data;
    Error error;
    u64 start_time = 0;
    u64 last_update_time = 0;
    s32 status_code = 0;
    u32 content_length = 0;
    u32 last_progress_update = 0;
    Type type = Type::Get;
    std::atomic<State> state{State::Pending};
    u16 timeout_seconds = 0;
  };

  HTTPDownloader();
  virtual ~HTTPDownloader();

  static std::unique_ptr<HTTPDownloader> Create(std::string user_agent, Error* error = nullptr);
  static std::string GetExtensionForContentType(const std::string& content_type);

  void SetDefaultTimeout(u16 timeout_seconds);
  void SetMaxActiveRequests(u32 max_active_requests);

  void CreateRequest(std::string url, Request::Callback callback, ProgressCallback* progress = nullptr,
                     HeaderList additional_headers = {}, std::optional<u16> timeout_seconds = {});
  void CreatePostRequest(std::string url, std::string post_data, Request::Callback callback,
                         ProgressCallback* progress = nullptr, HeaderList additional_headers = {},
                         std::optional<u16> timeout_seconds = {});
  void PollRequests();
  void WaitForAllRequests();
  void WaitForAllRequestsWithYield(std::function<void()> before_sleep_cb, std::function<void()> after_sleep_cb);
  bool HasAnyRequests();
  void CancelAllRequests();

protected:
  virtual Request* InternalCreateRequest(Request::Type type, std::string url, std::string post_data,
                                         Request::Callback callback, ProgressCallback* progress, u16 timeout_seconds,
                                         HeaderList additional_headers) = 0;

  virtual bool StartRequest(Request* request) = 0;
  virtual void CloseRequest(Request* request) = 0;

  void LockedAddRequest(Request* request);
  void StartOrAddRequest(Request* request);
  u32 LockedGetActiveRequestCount();
  void LockedPollRequests(std::unique_lock<std::mutex>& lock);

  u16 m_default_timeout;
  u32 m_max_active_requests;

  std::mutex m_pending_http_request_lock;
  std::vector<Request*> m_pending_http_requests;
};
