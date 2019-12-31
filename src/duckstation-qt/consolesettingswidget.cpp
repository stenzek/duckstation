#include "consolesettingswidget.h"

ConsoleSettingsWidget::ConsoleSettingsWidget(QWidget* parent /*= nullptr*/) : QWidget(parent)
{
  m_ui.setupUi(this);
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;
