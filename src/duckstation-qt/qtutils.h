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
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QWidget>
#include <functional>
#include <initializer_list>
#include <optional>

class Error;

class QComboBox;
class QFrame;
class QKeyEvent;
class QLabel;
class QMenu;
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
/// If the window was hidden and parent_window is provided, the window is centered on parent_window.
void ShowOrRaiseWindow(QWidget* window, const QWidget* parent_window = nullptr, bool restore_geometry = false);

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
std::optional<u32> KeyEventToCode(const QKeyEvent* ev);

/// Opens a URL with the default handler.
void OpenURL(QWidget* parent, const QUrl& qurl);

/// Opens a URL string with the default handler.
void OpenURL(QWidget* parent, const std::string_view url);

/// Prompts for an address in hex.
std::optional<unsigned> PromptForAddress(QWidget* parent, const QString& title, const QString& label, bool code);

/// Converts a std::string_view to a QString safely.
QString StringViewToQString(std::string_view str);

/// Ensures line endings are normalized in \n format.
QString NormalizeLineEndings(QString str);

/// Sets a widget to italics if the setting value is inherited.
void SetWidgetFontForInheritedSetting(QWidget* widget, bool inherited);

/// Binds a label to a slider's value.
void BindLabelToSlider(QSlider* slider, QLabel* label, float range = 1.0f);

/// Changes whether a window is resizable.
void SetWindowResizeable(QWidget* widget, bool resizeable);

/// Adjusts the fixed size for a window if it's not resizeable.
void ResizePotentiallyFixedSizeWindow(QWidget* widget, int width, int height);

/// Replacement for QMessageBox::question() and friends that doesn't look terrible on MacOS.
QMessageBox::StandardButton MessageBoxInformation(QWidget* parent, const QString& title, const QString& text,
                                                  QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                                  QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton MessageBoxWarning(QWidget* parent, const QString& title, const QString& text,
                                              QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                              QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton MessageBoxCritical(QWidget* parent, const QString& title, const QString& text,
                                               QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                                               QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton MessageBoxQuestion(
  QWidget* parent, const QString& title, const QString& text,
  QMessageBox::StandardButtons buttons = QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No),
  QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton MessageBoxIcon(QWidget* parent, QMessageBox::Icon icon, const QString& title,
                                           const QString& text, QMessageBox::StandardButtons buttons,
                                           QMessageBox::StandardButton defaultButton);
QMessageBox* NewMessageBox(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text,
                           QMessageBox::StandardButtons buttons,
                           QMessageBox::StandardButton defaultButton = QMessageBox::NoButton,
                           bool delete_on_close = true);
void AsyncMessageBox(QWidget* parent, QMessageBox::Icon icon, const QString& title, const QString& text,
                     QMessageBox::StandardButtons button = QMessageBox::Ok);

/// Styles a popup menu for the current theme.
void StylePopupMenu(QMenu* menu);
void StyleChildMenus(QWidget* widget);

/// Creates a new popup menu, styled for the current theme.
QMenu* NewPopupMenu(QWidget* parent, bool delete_on_close = true);

/// Returns icon for language.
QIcon GetIconForTranslationLanguage(std::string_view language_name);

/// Returns icon for region.
QIcon GetIconForRegion(ConsoleRegion region);
QIcon GetIconForRegion(DiscRegion region);

/// Returns icon for entry type.
QIcon GetIconForEntryType(GameList::EntryType type);
QIcon GetIconForCompatibility(GameDatabase::CompatibilityRating rating);
QIcon GetIconForLanguage(std::string_view language_name);

/// Scales a Memory Card Icon (QPixmap or QImage) using Sharp Bilinear scaling
void ResizeSharpBilinear(QPixmap& pm, int size, int base_size);
void ResizeSharpBilinear(QImage& pm, int size, int base_size);

/// Applies the device pixel ratio to the given size, giving the size in pixels.
QSize ApplyDevicePixelRatioToSize(const QSize& size, qreal device_pixel_ratio);

/// Removes the device pixel ratio from the given size, giving the size in device-independent units.
QSize GetDeviceIndependentSize(const QSize& size, qreal device_pixel_ratio);

/// Returns the pixel size (real geometry) for a widget.
/// Also returns the "real" DPR scale for the widget, ignoring any operating-system level downsampling.
std::pair<QSize, qreal> GetPixelSizeForWidget(const QWidget* widget);

/// Returns the common window info structure for a Qt widget.
std::optional<WindowInfo> GetWindowInfoForWidget(QWidget* widget, RenderAPI render_api, Error* error = nullptr);

/// Saves a window's geometry to configuration. Returns false if the configuration was changed.
void SaveWindowGeometry(QWidget* widget, bool auto_commit_changes = true);
void SaveWindowGeometry(std::string_view window_name, QWidget* widget, bool auto_commit_changes = true);

/// Restores a window's geometry from configuration. Returns false if it was not found in the configuration.
bool RestoreWindowGeometry(QWidget* widget);
bool RestoreWindowGeometry(std::string_view window_name, QWidget* widget);

/// Positions a window in the center of its parent or the screen.
void CenterWindowRelativeToParent(QWidget* window, const QWidget* parent_window);

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
