// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "common/heap_array.h"

#include <string>
#include <string_view>

#include <QtCore/QObject>

class QPixmap;

class AsyncPixmapLoader final : public QObject
{
  Q_OBJECT

public:
  explicit AsyncPixmapLoader(QObject* parent = nullptr);
  ~AsyncPixmapLoader() override;

  static bool isQueueNeeded(std::string_view url_or_path);

  static QPixmap load(std::string_view url_or_path);

  void enqueue(std::string_view url_or_path);

Q_SIGNALS:
  void pixmapLoaded(QPixmap& pixmap);

private:
  void finishLoad();

  std::string m_extension;
  DynamicHeapArray<u8> m_data;
};
