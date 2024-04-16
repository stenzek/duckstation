// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "util/window_info.h"

#include "common/types.h"

#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtGui/QIcon>
#include <QtWidgets/QWidget>
#include <functional>
#include <initializer_list>
#include <optional>

class ByteStream;

class QComboBox;
class QFrame;
class QKeyEvent;
class QLabel;
class QSlider;
class QTableView;
class QTreeView;
class QVariant;
class QWidget;
class QUrl;

enum class ConsoleRegion;
enum class DiscRegion : u8;
namespace GameDatabase {
enum class CompatibilityRating : u8;
}
namespace GameList {
enum class EntryType;
}

namespace QtUtils {

/// Wheel delta is 120 as in winapi.
static constexpr float MOUSE_WHEEL_DELTA = 120.0f;

/// Creates a horizontal line widget.
QFrame* CreateHorizontalLine(QWidget* parent);

/// Returns the greatest parent of a widget, i.e. its dialog/window.
QWidget* GetRootWidget(QWidget* widget, bool stop_at_window_or_dialog = true);

/// Shows or raises a window (brings it to the front).
void ShowOrRaiseWindow(QWidget* window);

/// Closes and deletes a window later, outside of this event pump.
template<typename T>
[[maybe_unused]] static void CloseAndDeleteWindow(T*& window)
{
  if (!window)
    return;

  window->close();

  // Some windows delete themselves.
  if (window)
    window->deleteLater();

  window = nullptr;
}

/// Resizes columns of the table view to at the specified widths. A negative width will stretch the column to use the
/// remaining space.
void ResizeColumnsForTableView(QTableView* view, const std::initializer_list<int>& widths);
void ResizeColumnsForTreeView(QTreeView* view, const std::initializer_list<int>& widths);

/// Returns a key id for a key event, including any modifiers that we need (e.g. Keypad).
/// NOTE: Defined in QtKeyCodes.cpp, not QtUtils.cpp.
u32 KeyEventToCode(const QKeyEvent* ev);

/// Reads a whole stream to a Qt byte array.
QByteArray ReadStreamToQByteArray(ByteStream* stream, bool rewind = false);

/// Creates a stream from a Qt byte array.
bool WriteQByteArrayToStream(QByteArray& arr, ByteStream* stream);

/// Opens a URL with the default handler.
void OpenURL(QWidget* parent, const QUrl& qurl);

/// Opens a URL string with the default handler.
void OpenURL(QWidget* parent, const char* url);

/// Prompts for an address in hex.
std::optional<unsigned> PromptForAddress(QWidget* parent, const QString& title, const QString& label, bool code);

/// Converts a std::string_view to a QString safely.
QString StringViewToQString(const std::string_view& str);

/// Sets a widget to italics if the setting value is inherited.
void SetWidgetFontForInheritedSetting(QWidget* widget, bool inherited);

/// Binds a label to a slider's value.
void BindLabelToSlider(QSlider* slider, QLabel* label, float range = 1.0f);

/// Changes whether a window is resizable.
void SetWindowResizeable(QWidget* widget, bool resizeable);

/// Adjusts the fixed size for a window if it's not resizeable.
void ResizePotentiallyFixedSizeWindow(QWidget* widget, int width, int height);

/// Returns icon for region.
QIcon GetIconForRegion(ConsoleRegion region);
QIcon GetIconForRegion(DiscRegion region);

/// Returns icon for entry type.
QIcon GetIconForEntryType(GameList::EntryType type);
QIcon GetIconForCompatibility(GameDatabase::CompatibilityRating rating);

/// Returns the pixel ratio/scaling factor for a widget.
qreal GetDevicePixelRatioForWidget(const QWidget* widget);

/// Returns the common window info structure for a Qt widget.
std::optional<WindowInfo> GetWindowInfoForWidget(QWidget* widget);

} // namespace QtUtils
