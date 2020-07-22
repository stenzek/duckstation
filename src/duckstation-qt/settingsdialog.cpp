#include "settingsdialog.h"
#include "advancedsettingswidget.h"
#include "audiosettingswidget.h"
#include "consolesettingswidget.h"
#include "controllersettingswidget.h"
#include "gamelistsettingswidget.h"
#include "generalsettingswidget.h"
#include "gpusettingswidget.h"
#include "hotkeysettingswidget.h"
#include "memorycardsettingswidget.h"
#include "qthostinterface.h"
#include <QtWidgets/QTextEdit>

static constexpr char DEFAULT_SETTING_HELP_TEXT[] = "";

static constexpr std::array<const char*, static_cast<int>(SettingsDialog::Category::Count)> s_category_help_text = {
  {"<strong>General Settings</strong><hr>These options control how the emulator looks and behaves.<br><br>Mouse over "
   "an option for additional information.",
   "<strong>Console Settings</strong><hr>These options determine the configuration of the simulated "
   "console.<br><br>Mouse over an option for additional information.",
   "<strong>Game List Settings</strong><hr>The list above shows the directories which will be searched by DuckStation "
   "to populate the game list. Search directories can be added, removed, and switched to recursive/non-recursive. "
   "Additionally, the redump.org database can be downloaded or updated to provide titles for discs, as the discs "
   "themselves do not provide title information.",
   "<strong>Hotkey Settings</strong><hr>Binding a hotkey allows you to trigger events such as a resetting or taking "
   "screenshots at the press of a key/controller button. Hotkey titles are self-explanatory. Clicking a binding will "
   "start a countdown, in which case you should press the key or controller button/axis you wish to bind. If no button "
   "is pressed and the timer lapses, the binding will be unchanged. To clear a binding, right-click the button. To "
   "bind multiple buttons, hold Shift and click the button.",
   "<strong>Controller Settings</strong><hr>This page lets you choose the type of controller you wish to simulate for "
   "the console, and rebind the keys or host game controller buttons to your choosing. Clicking a binding will start a "
   "countdown, in which case you should press the key or controller button/axis you wish to bind. (For rumble, press "
   "any button/axis on the controller you wish to send rumble to.) If no button is pressed and the timer lapses, "
   "the binding will be unchanged. To clear a binding, right-click the button. To bind multiple buttons, hold Shift "
   "and click the button.",
   "<strong>Memory Card Settings</strong><hr>This page lets you control what mode the memory card emulation will "
   "function in, and where the images for these cards will be stored on disk.",
   "<strong>GPU Settings</strong><hr>These options control the simulation of the GPU in the console. Various "
   "enhancements are available, mouse over each for additional information.",
   "<strong>Audio Settings</strong><hr>These options control the audio output of the console. Mouse over an option for "
   "additional information.",
   "<strong>Advanced Settings</strong><hr>These options control logging and internal behavior of the emulator. Mouse "
   "over an option for additional information."}};

SettingsDialog::SettingsDialog(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QDialog(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_general_settings = new GeneralSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_console_settings = new ConsoleSettingsWidget(host_interface, m_ui.settingsContainer);
  m_game_list_settings = new GameListSettingsWidget(host_interface, m_ui.settingsContainer);
  m_hotkey_settings = new HotkeySettingsWidget(host_interface, m_ui.settingsContainer);
  m_controller_settings = new ControllerSettingsWidget(host_interface, m_ui.settingsContainer);
  m_memory_card_settings = new MemoryCardSettingsWidget(host_interface, m_ui.settingsContainer);
  m_gpu_settings = new GPUSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_audio_settings = new AudioSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_advanced_settings = new AdvancedSettingsWidget(host_interface, m_ui.settingsContainer, this);

  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GeneralSettings), m_general_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::ConsoleSettings), m_console_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GameListSettings), m_game_list_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::HotkeySettings), m_hotkey_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::ControllerSettings), m_controller_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::MemoryCardSettings), m_memory_card_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GPUSettings), m_gpu_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AudioSettings), m_audio_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AdvancedSettings), m_advanced_settings);

  m_ui.settingsCategory->setCurrentRow(0);
  m_ui.settingsContainer->setCurrentIndex(0);
  m_ui.helpText->setText(tr(s_category_help_text[0]));
  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this, &SettingsDialog::onCategoryCurrentRowChanged);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::setCategory(Category category)
{
  if (category >= Category::Count)
    return;

  m_ui.settingsCategory->setCurrentRow(static_cast<int>(category));
}

void SettingsDialog::onCategoryCurrentRowChanged(int row)
{
  Q_ASSERT(row < static_cast<int>(Category::Count));
  m_ui.settingsContainer->setCurrentIndex(row);
  m_ui.helpText->setText(tr(s_category_help_text[row]));
}

void SettingsDialog::registerWidgetHelp(QObject* object, const char* title, const char* recommended_value,
                                        const char* text)
{
  // construct rich text with formatted description
  QString full_text;
  full_text += "<table width='100%' cellpadding='0' cellspacing='0'><tr><td><strong>";
  full_text += tr(title);
  full_text += "</strong></td><td align='right'><strong>";
  full_text += tr("Recommended Value");
  full_text += ": </strong>";
  full_text += recommended_value;
  full_text += "</td></table><hr>";
  full_text += text;

  m_widget_help_text_map[object] = std::move(full_text);
  object->installEventFilter(this);
}

bool SettingsDialog::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() == QEvent::Enter)
  {
    auto iter = m_widget_help_text_map.constFind(object);
    if (iter != m_widget_help_text_map.end())
    {
      m_current_help_widget = object;
      m_ui.helpText->setText(iter.value());
    }
  }
  else if (event->type() == QEvent::Leave)
  {
    if (m_current_help_widget)
    {
      m_current_help_widget = nullptr;
      m_ui.helpText->setText(tr(s_category_help_text[m_ui.settingsCategory->currentRow()]));
    }
  }

  return QDialog::eventFilter(object, event);
}