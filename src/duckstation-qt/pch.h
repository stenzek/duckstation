// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core/pch.h"

#ifdef _MSC_VER
// Disable C4251 outside of Qt code too, because not all headers are included here.
#pragma warning(disable : 4251) // warning C4251: 'QLayoutItem::align': 'QFlags<Qt::AlignmentFlag>' needs to have
                                // dll-interface to be used by clients of 'QLayoutItem'

#pragma warning(push)
#pragma warning(disable : 4864) // warning C4864: expected 'template' keyword before dependent template name
#endif

#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QObject>
#include <QtCore/QSemaphore>
#include <QtCore/QString>
#include <QtCore/QtCore>
#include <QtWidgets/QWidget>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
