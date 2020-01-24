#include "consolesettingswidget.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QFileDialog>

static constexpr char BIOS_IMAGE_FILTER[] = "Binary Images (*.bin);;All Files (*.*)";

ConsoleSettingsWidget::ConsoleSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToEnumSetting(m_host_interface, m_ui.region, "Console/Region",
                                               &Settings::ParseConsoleRegionName, &Settings::GetConsoleRegionName);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_ui.biosPath, "BIOS/Path");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableTTYOutput, "BIOS/PatchTTYEnable");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.fastBoot, "BIOS/PatchFastBoot");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableSpeedLimiter,
                                               "General/SpeedLimiterEnabled");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pauseOnStart, "General/StartPaused");

  connect(m_ui.biosPathBrowse, &QPushButton::pressed, this, &ConsoleSettingsWidget::onBrowseBIOSPathButtonClicked);
}

ConsoleSettingsWidget::~ConsoleSettingsWidget() = default;

void ConsoleSettingsWidget::onBrowseBIOSPathButtonClicked()
{
  QString path = QFileDialog::getOpenFileName(this, tr("Select BIOS Image"), QString(), tr(BIOS_IMAGE_FILTER));
  if (path.isEmpty())
    return;

  m_ui.biosPath->setText(path);

  m_host_interface->putSettingValue("BIOS/Path", path);
  m_host_interface->applySettings();
}
