#include "hotkeysettingswidget.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>

HotkeySettingsWidget::HotkeySettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  createUi();
}

HotkeySettingsWidget::~HotkeySettingsWidget() = default;

void HotkeySettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  m_tab_widget = new QTabWidget(this);

  createButtons();

  layout->addWidget(m_tab_widget, 0, 0, 1, 1);

  setLayout(layout);
}

void HotkeySettingsWidget::createButtons()
{
  std::vector<QtHostInterface::HotkeyInfo> hotkeys = m_host_interface->getHotkeyList();

  for (const QtHostInterface::HotkeyInfo& hi : hotkeys)
  {
    auto iter = m_categories.find(hi.category);
    if (iter == m_categories.end())
    {
      QScrollArea* scroll = new QScrollArea(m_tab_widget);
      QWidget* container = new QWidget(scroll);
      QVBoxLayout* vlayout = new QVBoxLayout(container);
      QGridLayout* layout = new QGridLayout();
      layout->setContentsMargins(0, 0, 0, 0);
      vlayout->addLayout(layout);
      vlayout->addStretch(1);
      iter = m_categories.insert(hi.category, Category{container, layout});
      scroll->setWidget(container);
      scroll->setWidgetResizable(true);
      scroll->setBackgroundRole(QPalette::Base);
      scroll->setFrameShape(QFrame::NoFrame);
      m_tab_widget->addTab(scroll, hi.category);
    }

    QWidget* container = iter->container;
    QGridLayout* layout = iter->layout;
    const int layout_count = layout->count() / 2;
    const int target_column = (layout_count / ROWS_PER_COLUMN) * 2;
    const int target_row = layout_count % ROWS_PER_COLUMN;

    const QString setting_name = QStringLiteral("Hotkeys/%1").arg(hi.name);
    layout->addWidget(new QLabel(hi.display_name, container), target_row, target_column);
    layout->addWidget(new InputButtonBindingWidget(m_host_interface, setting_name, container), target_row,
                      target_column + 1);
  }
}
