// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/http_downloader.h"

#include "common/error.h"

#include <string>
#include <string_view>

#include <QtCore/QObject>

class QPixmap;

class AsyncHTTPRequest final : public QObject
{
  Q_OBJECT

public:
  AsyncHTTPRequest();
  ~AsyncHTTPRequest() override;

  void get(std::string url, const void* owner, ProgressCallback* progress = nullptr,
           HTTPDownloader::HeaderList additional_headers = {}, std::optional<u16> timeout_seconds = {});
  void post(std::string url, std::string post_data, const void* owner, ProgressCallback* progress = nullptr,
            HTTPDownloader::HeaderList additional_headers = {}, std::optional<u16> timeout_seconds = {});

Q_SIGNALS:
  void requestComplete(qint32 status_code, const std::string& error_message, const std::string& content_type,
                       const HTTPDownloader::RequestData& data);

private:
  void handleResponse(s32 status_code, const std::string_view& error_message, const std::string_view& content_type,
                      HTTPDownloader::RequestData&& data);
  void finishRequest();

  s32 m_status_code = HTTPDownloader::HTTP_STATUS_ERROR;
  std::string m_error_message;
  std::string m_content_type;
  HTTPDownloader::RequestData m_data;
};
