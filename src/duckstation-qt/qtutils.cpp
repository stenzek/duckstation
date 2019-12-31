#include "qtutils.h"
#include <QtWidgets/QTableView>
#include <algorithm>

namespace QtUtils {

void ResizeColumnsForTableView(QTableView* view, const std::initializer_list<int>& widths)
{
  const int total_width =
    std::accumulate(widths.begin(), widths.end(), 0, [](int a, int b) { return a + std::max(b, 0); });

  const int flex_width = std::max(view->width() - total_width - 2, 1);

  int column_index = 0;
  for (const int spec_width : widths)
  {
    const int width = spec_width < 0 ? flex_width : spec_width;
    view->setColumnWidth(column_index, width);
    column_index++;
  }
}

} // namespace QtUtils