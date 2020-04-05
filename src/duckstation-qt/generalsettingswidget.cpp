#include "generalsettingswidget.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"

GeneralSettingsWidget::GeneralSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.pauseOnStart, "Main/StartPaused");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.startFullscreen, "Main/StartFullscreen");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.renderToMain, "Main/RenderToMainWindow");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.saveStateOnExit, "Main/SaveStateOnExit");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.confirmPowerOff, "Main/ConfirmPowerOff");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.showOSDMessages, "Display/ShowOSDMessages");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.showFPS, "Display/ShowFPS");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.showVPS, "Display/ShowVPS");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.showSpeed, "Display/ShowSpeed");

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableSpeedLimiter, "Main/SpeedLimiterEnabled");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.increaseTimerResolution,
                                               "Main/IncreaseTimerResolution");
  SettingWidgetBinder::BindWidgetToNormalizedSetting(m_host_interface, m_ui.emulationSpeed, "Main/EmulationSpeed",
                                                     100.0f);

  connect(m_ui.enableSpeedLimiter, &QCheckBox::stateChanged, this,
          &GeneralSettingsWidget::onEnableSpeedLimiterStateChanged);
  connect(m_ui.emulationSpeed, &QSlider::valueChanged, this, &GeneralSettingsWidget::onEmulationSpeedValueChanged);

  onEnableSpeedLimiterStateChanged();
  onEmulationSpeedValueChanged(m_ui.emulationSpeed->value());

  dialog->registerWidgetHelp(m_ui.confirmPowerOff, "Confirm Power Off", "Checked",
                             "Determines whether a prompt will be displayed to confirm shutting down the emulator/game "
                             "when the hotkey is pressed.");
  dialog->registerWidgetHelp(m_ui.saveStateOnExit, "Save State On Exit", "Checked",
                             "Automatically saves the emulator state when powering down or exiting. You can then "
                             "resume directly from where you left off next time.");
  dialog->registerWidgetHelp(m_ui.startFullscreen, "Start Fullscreen", "Unchecked",
                             "Automatically switches to fullscreen mode when a game is started.");
  dialog->registerWidgetHelp(m_ui.renderToMain, "Render To Main Window", "Checked",
                             "Renders the display of the simulated console to the main window of the application, over "
                             "the game list. If unchecked, the display will render in a seperate window.");
  dialog->registerWidgetHelp(m_ui.pauseOnStart, "Pause On Start", "Unchecked",
                             "Pauses the emulator when a game is started.");
  dialog->registerWidgetHelp(m_ui.enableSpeedLimiter, "Enable Speed Limiter", "Checked",
                             "Throttles the emulation speed to the chosen speed above. If unchecked, the emulator will "
                             "run as fast as possible, which may not be playable.");
  dialog->registerWidgetHelp(m_ui.increaseTimerResolution, "Increase Timer Resolution", "Checked",
                             "Increases the system timer resolution when emulation is started to provide more accurate "
                             "frame pacing. May increase battery usage on laptops.");
  dialog->registerWidgetHelp(m_ui.emulationSpeed, "Emulation Speed", "100%",
                             "Sets the target emulation speed. It is not guaranteed that this speed will be reached, "
                             "and if not, the emulator will run as fast as it can manage.");
  dialog->registerWidgetHelp(m_ui.showOSDMessages, "Show OSD Messages", "Checked",
                             "Shows on-screen-display messages when events occur such as save states being "
                             "created/loaded, screenshots being taken, etc.");
  dialog->registerWidgetHelp(m_ui.showFPS, "Show FPS", "Unchecked",
                             "Shows the internal frame rate of the game in the top-right corner of the display.");
  dialog->registerWidgetHelp(m_ui.showVPS, "Show VPS", "Unchecked",
                             "Shows the number of frames (or v-syncs) displayed per second by the system in the "
                             "top-right corner of the display.");
  dialog->registerWidgetHelp(
    m_ui.showSpeed, "Show Speed", "Unchecked",
    "Shows the current emulation speed of the system in the top-right corner of the display as a percentage.");
}

GeneralSettingsWidget::~GeneralSettingsWidget() = default;

void GeneralSettingsWidget::onEnableSpeedLimiterStateChanged()
{
  m_ui.emulationSpeed->setDisabled(!m_ui.enableSpeedLimiter->isChecked());
}

void GeneralSettingsWidget::onEmulationSpeedValueChanged(int value)
{
  m_ui.emulationSpeedLabel->setText(tr("%1%").arg(value));
}
