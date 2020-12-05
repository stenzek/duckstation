#include "hotkeysettingswidget.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QCoreApplication>
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
  const auto& hotkeys = m_host_interface->getHotkeyInfoList();
  for (const auto& hi : hotkeys)
  {
    const auto category = qApp->translate("Hotkeys", hi.category);

    auto iter = m_categories.find(category);
    if (iter == m_categories.end())
    {
      QScrollArea* scroll = new QScrollArea(m_tab_widget);
      QWidget* container = new QWidget(scroll);
      QVBoxLayout* vlayout = new QVBoxLayout(container);
      QGridLayout* layout = new QGridLayout();
      layout->setContentsMargins(0, 0, 0, 0);
      vlayout->addLayout(layout);
      vlayout->addStretch(1);
      iter = m_categories.insert(category, Category{container, layout});
      scroll->setWidget(container);
      scroll->setWidgetResizable(true);
      scroll->setBackgroundRole(QPalette::Base);
      scroll->setFrameShape(QFrame::NoFrame);
      m_tab_widget->addTab(scroll, category);
    }

    QWidget* container = iter->container;
    QGridLayout* layout = iter->layout;
    const int target_row = layout->count() / 2;

    std::string section_name("Hotkeys");
    std::string key_name(hi.name.GetCharArray());
    layout->addWidget(new QLabel(qApp->translate("Hotkeys", hi.display_name), container), target_row, 0);
    layout->addWidget(
      new InputButtonBindingWidget(m_host_interface, std::move(section_name), std::move(key_name), container),
      target_row, 1);
  }
}
