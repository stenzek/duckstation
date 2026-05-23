// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "asynchttprequest.h"

#include "core/host.h"

#include "util/http_cache.h"
#include "util/http_downloader.h"

#include "common/error.h"
#include "common/log.h"

#include "moc_asynchttprequest.cpp"

LOG_CHANNEL(Host);

AsyncHTTPRequest::AsyncHTTPRequest() : QObject()
{
}

AsyncHTTPRequest::~AsyncHTTPRequest() = default;

void AsyncHTTPRequest::get(std::string url, const void* owner, ProgressCallback* progress /* = nullptr */,
                           HTTPDownloader::HeaderList additional_headers /* =  */,
                           std::optional<u16> timeout_seconds /* = */)
{
  HTTPDownloader* const downloader = HTTPCache::GetDownloader(&m_error);
  if (!downloader)
  {
    emit requestComplete(HTTPDownloader::HTTP_STATUS_ERROR, m_error, m_content_type, m_data);
    deleteLater();
    return;
  }

  downloader->CreateRequest(
    std::move(url), owner,
    [this](s32 status_code, Error& error, std::string& content_type, HTTPDownloader::Request::Data& data) {
      m_status_code = status_code;
      m_error = std::move(error);
      m_content_type = std::move(content_type);
      m_data = std::move(data);
      QMetaObject::invokeMethod(this, &AsyncHTTPRequest::finishRequest, Qt::QueuedConnection);
    },
    progress, additional_headers, timeout_seconds);
}

void AsyncHTTPRequest::post(std::string url, std::string post_data, const void* owner,
                            ProgressCallback* progress /* = nullptr */,
                            HTTPDownloader::HeaderList additional_headers /* =  */,
                            std::optional<u16> timeout_seconds /* = */)
{
  HTTPDownloader* const downloader = HTTPCache::GetDownloader(&m_error);
  if (!downloader)
  {
    emit requestComplete(HTTPDownloader::HTTP_STATUS_ERROR, m_error, m_content_type, m_data);
    deleteLater();
    return;
  }

  downloader->CreatePostRequest(
    std::move(url), std::move(post_data), owner,
    [this](s32 status_code, Error& error, std::string& content_type, HTTPDownloader::Request::Data& data) {
      m_status_code = status_code;
      m_error = std::move(error);
      m_content_type = std::move(content_type);
      m_data = std::move(data);
      QMetaObject::invokeMethod(this, &AsyncHTTPRequest::finishRequest, Qt::QueuedConnection);
    },
    progress, additional_headers, timeout_seconds);
}

void AsyncHTTPRequest::finishRequest()
{
  emit requestComplete(m_status_code, m_error, m_content_type, m_data);
  deleteLater();
}
