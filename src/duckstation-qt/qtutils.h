#pragma once
#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <initializer_list>
#include <optional>

class ByteStream;

class QFrame;
class QKeyEvent;
class QTableView;
class QWidget;

namespace QtUtils {

/// Creates a horizontal line widget.
QFrame* CreateHorizontalLine(QWidget* parent);

/// Returns the greatest parent of a widget, i.e. its dialog/window.
QWidget* GetRootWidget(QWidget* widget, bool stop_at_window_or_dialog = true);

/// Resizes columns of the table view to at the specified widths. A width of -1 will stretch the column to use the
/// remaining space.
void ResizeColumnsForTableView(QTableView* view, const std::initializer_list<int>& widths);

/// Returns a string identifier for a Qt key ID.
QString GetKeyIdentifier(int key);

/// Returns the integer Qt key ID for an identifier.
std::optional<int> GetKeyIdForIdentifier(const QString& key_identifier);

/// Stringizes a key event.
QString KeyEventToString(const QKeyEvent* ke);

/// Returns an integer id for a stringized key event. Modifiers are in the upper bits.
std::optional<int> ParseKeyString(const QString& key_str);

/// Returns a key id for a key event, including any modifiers.
int KeyEventToInt(const QKeyEvent* ke);

/// Reads a whole stream to a Qt byte array.
QByteArray ReadStreamToQByteArray(ByteStream* stream, bool rewind = false);

/// Creates a stream from a Qt byte array.
bool WriteQByteArrayToStream(QByteArray& arr, ByteStream* stream);

} // namespace QtUtils