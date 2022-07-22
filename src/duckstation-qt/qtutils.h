#pragma once
#include "common/types.h"
#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtGui/QIcon>
#include <functional>
#include <initializer_list>
#include <optional>

class ByteStream;

class QComboBox;
class QFrame;
class QKeyEvent;
class QTableView;
class QTreeView;
class QVariant;
class QWidget;
class QUrl;

enum class ConsoleRegion;
enum class DiscRegion : u8;
namespace GameDatabase {
enum class CompatibilityRating : u32;
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

/// Fills a combo box with resolution scale options.
void FillComboBoxWithResolutionScales(QComboBox* cb);

/// Fills a combo box with multisampling options.
QVariant GetMSAAModeValue(uint multisamples, bool ssaa);
void DecodeMSAAModeValue(const QVariant& userdata, uint* multisamples, bool* ssaa);
void FillComboBoxWithMSAAModes(QComboBox* cb);

/// Prompts for an address in hex.
std::optional<unsigned> PromptForAddress(QWidget* parent, const QString& title, const QString& label, bool code);

/// Converts a std::string_view to a QString safely.
QString StringViewToQString(const std::string_view& str);

/// Sets a widget to italics if the setting value is inherited.
void SetWidgetFontForInheritedSetting(QWidget* widget, bool inherited);

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

} // namespace QtUtils
