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
  HTTPDownloader::CreateRequest(
    std::move(url), owner,
    [this](s32 status_code, Error& error, std::string& content_type, HTTPDownloader::RequestData& data) {
      handleResponse(status_code, error, content_type, data);
    },
    progress, additional_headers, timeout_seconds);
}

void AsyncHTTPRequest::post(std::string url, std::string post_data, const void* owner,
                            ProgressCallback* progress /* = nullptr */,
                            HTTPDownloader::HeaderList additional_headers /* =  */,
                            std::optional<u16> timeout_seconds /* = */)
{
  HTTPDownloader::CreatePostRequest(
    std::move(url), std::move(post_data), owner,
    [this](s32 status_code, Error& error, std::string& content_type, HTTPDownloader::RequestData& data) {
      handleResponse(status_code, error, content_type, data);
    },
    progress, additional_headers, timeout_seconds);
}

ALWAYS_INLINE_RELEASE void AsyncHTTPRequest::handleResponse(s32 status_code, Error& error, std::string& content_type,
                                                            HTTPDownloader::RequestData& data)
{
  m_status_code = status_code;
  m_error = std::move(error);
  m_content_type = std::move(content_type);
  m_data = std::move(data);
  QMetaObject::invokeMethod(this, &AsyncHTTPRequest::finishRequest, Qt::QueuedConnection);
}

void AsyncHTTPRequest::finishRequest()
{
  emit requestComplete(m_status_code, m_error, m_content_type, m_data);
  deleteLater();
}
