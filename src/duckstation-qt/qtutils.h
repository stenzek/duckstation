#pragma once
#include <initializer_list>

class QTableView;

namespace QtUtils {

/// Resizes columns of the table view to at the specified widths. A width of -1 will stretch the column to use the
/// remaining space.
void ResizeColumnsForTableView(QTableView* view, const std::initializer_list<int>& widths);

} // namespace QtUtils