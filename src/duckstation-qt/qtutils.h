// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "util/window_info.h"

#include "common/types.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtWidgets/QWidget>
#include <functional>
#include <initializer_list>
#include <optional>

class Error;

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

enum class RenderAPI : u8;

enum class ConsoleRegion : u8;
enum class DiscRegion : u8;
namespace GameDatabase {
enum class CompatibilityRating : u8;
}
namespace GameList {
enum class EntryType : u8;
}

namespace QtUtils {

/// Creates a horizontal line widget.
QFrame* CreateHorizontalLine(QWidget* parent);

/// Returns the greatest parent of a widget, i.e. its dialog/window.
QWidget* GetRootWidget(QWidget* widget, bool stop_at_window_or_dialog = true);

/// Shows or raises a window (brings it to the front).
void ShowOrRaiseWindow(QWidget* window);

/// Closes and deletes a window later, outside of this event pump.
template<typename T>
inline void CloseAndDeleteWindow(T*& window)
{
  if (!window)
    return;

  window->close();

  // Some windows delete themselves.
  if (window)
    window->deleteLater();

  window = nullptr;
}

/// For any positive values, sets the corresponding column width to the specified value.
/// Any values of 0 will set the column's width based on the content.
/// Any values of -1 will stretch the column to use the remaining space.
void SetColumnWidthsForTableView(QTableView* view, const std::initializer_list<int>& widths);
void SetColumnWidthsForTreeView(QTreeView* view, const std::initializer_list<int>& widths);

/// Returns a key id for a key event, including any modifiers that we need (e.g. Keypad).
/// NOTE: Defined in QtKeyCodes.cpp, not QtUtils.cpp.
u32 KeyEventToCode(const QKeyEvent* ev);

/// Opens a URL with the default handler.
void OpenURL(QWidget* parent, const QUrl& qurl);

/// Opens a URL string with the default handler.
void OpenURL(QWidget* parent, const std::string_view url);

/// Prompts for an address in hex.
std::optional<unsigned> PromptForAddress(QWidget* parent, const QString& title, const QString& label, bool code);

/// Converts a std::string_view to a QString safely.
QString StringViewToQString(std::string_view str);

/// Sets a widget to italics if the setting value is inherited.
void SetWidgetFontForInheritedSetting(QWidget* widget, bool inherited);

/// Binds a label to a slider's value.
void BindLabelToSlider(QSlider* slider, QLabel* label, float range = 1.0f);

/// Changes whether a window is resizable.
void SetWindowResizeable(QWidget* widget, bool resizeable);

/// Adjusts the fixed size for a window if it's not resizeable.
void ResizePotentiallyFixedSizeWindow(QWidget* widget, int width, int height);

/// Returns icon for language.
QIcon GetIconForTranslationLanguage(std::string_view language_name);

/// Returns icon for region.
QIcon GetIconForRegion(ConsoleRegion region);
QIcon GetIconForRegion(DiscRegion region);

/// Returns icon for entry type.
QIcon GetIconForEntryType(GameList::EntryType type);
QIcon GetIconForCompatibility(GameDatabase::CompatibilityRating rating);
QIcon GetIconForLanguage(std::string_view language_name);

/// Returns the pixel ratio/scaling factor for a widget.
qreal GetDevicePixelRatioForWidget(const QWidget* widget);

/// Returns the common window info structure for a Qt widget.
std::optional<WindowInfo> GetWindowInfoForWidget(QWidget* widget, RenderAPI render_api, Error* error = nullptr);

/// Saves a window's geometry to configuration. Returns false if the configuration was changed.
void SaveWindowGeometry(std::string_view window_name, QWidget* widget, bool auto_commit_changes = true);

/// Restores a window's geometry from configuration. Returns false if it was not found in the configuration.
bool RestoreWindowGeometry(std::string_view window_name, QWidget* widget);

/// CPU-friendly way of blocking the UI thread while some predicate holds true.
template<typename Predicate>
inline void ProcessEventsWithSleep(QEventLoop::ProcessEventsFlags flags, const Predicate& pred, int sleep_time_ms = 10)
{
  if (sleep_time_ms == 0)
  {
    while (pred())
      QCoreApplication::processEvents(flags);
  }

  if (!pred())
    return;

  QEventLoop loop;
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, &timer, [&loop, &pred]() {
    if (pred())
      return;

    loop.exit();
  });
  timer.start(sleep_time_ms);
  loop.exec(flags);
}

} // namespace QtUtils
