#pragma once
#include <QtCore/QString>
#include <initializer_list>
#include <optional>

class QTableView;

namespace QtUtils {

/// Resizes columns of the table view to at the specified widths. A width of -1 will stretch the column to use the
/// remaining space.
void ResizeColumnsForTableView(QTableView* view, const std::initializer_list<int>& widths);

/// Returns a string identifier for a Qt key ID.
QString GetKeyIdentifier(int key);

/// Returns the integer Qt key ID for an identifier.
std::optional<int> GetKeyIdForIdentifier(const QString& key_identifier);

} // namespace QtUtils