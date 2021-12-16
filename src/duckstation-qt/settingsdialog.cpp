#include "settingsdialog.h"
#include "advancedsettingswidget.h"
#include "audiosettingswidget.h"
#include "biossettingswidget.h"
#include "consolesettingswidget.h"
#include "controllersettingswidget.h"
#include "displaysettingswidget.h"
#include "emulationsettingswidget.h"
#include "enhancementsettingswidget.h"
#include "gamelistsettingswidget.h"
#include "generalsettingswidget.h"
#include "hotkeysettingswidget.h"
#include "memorycardsettingswidget.h"
#include "postprocessingsettingswidget.h"
#include "qthostinterface.h"
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTextEdit>

#ifdef WITH_CHEEVOS
#include "achievementsettingswidget.h"
#endif

static constexpr char DEFAULT_SETTING_HELP_TEXT[] = "";

SettingsDialog::SettingsDialog(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QDialog(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);
  setCategoryHelpTexts();

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_general_settings = new GeneralSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_bios_settings = new BIOSSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_console_settings = new ConsoleSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_emulation_settings = new EmulationSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_game_list_settings = new GameListSettingsWidget(host_interface, m_ui.settingsContainer);
  m_hotkey_settings = new HotkeySettingsWidget(host_interface, m_ui.settingsContainer);
  m_controller_settings = new ControllerSettingsWidget(host_interface, m_ui.settingsContainer);
  m_memory_card_settings = new MemoryCardSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_display_settings = new DisplaySettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_enhancement_settings = new EnhancementSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_post_processing_settings = new PostProcessingSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_audio_settings = new AudioSettingsWidget(host_interface, m_ui.settingsContainer, this);
  m_advanced_settings = new AdvancedSettingsWidget(host_interface, m_ui.settingsContainer, this);

#ifdef WITH_CHEEVOS
  m_achievement_settings = new AchievementSettingsWidget(host_interface, m_ui.settingsContainer, this);
#endif

  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GeneralSettings), m_general_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::BIOSSettings), m_bios_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::ConsoleSettings), m_console_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::EmulationSettings), m_emulation_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GameListSettings), m_game_list_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::HotkeySettings), m_hotkey_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::ControllerSettings), m_controller_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::MemoryCardSettings), m_memory_card_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::DisplaySettings), m_display_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::EnhancementSettings), m_enhancement_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::PostProcessingSettings), m_post_processing_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AudioSettings), m_audio_settings);

#ifdef WITH_CHEEVOS
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AchievementSettings), m_achievement_settings);
#else
  QLabel* placeholder_label =
    new QLabel(tr("This DuckStation build was not compiled with RetroAchievements support."), m_ui.settingsContainer);
  placeholder_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AchievementSettings), placeholder_label);
#endif

  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AdvancedSettings), m_advanced_settings);

  m_ui.settingsCategory->setCurrentRow(0);
  m_ui.settingsContainer->setCurrentIndex(0);
  m_ui.helpText->setText(m_category_help_text[0]);
  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this, &SettingsDialog::onCategoryCurrentRowChanged);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &SettingsDialog::accept);
  connect(m_ui.buttonBox, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) {
    if (m_ui.buttonBox->buttonRole(button) == QDialogButtonBox::ResetRole)
    {
      onRestoreDefaultsClicked();
    }
  });

  connect(m_console_settings, &ConsoleSettingsWidget::multitapModeChanged, m_controller_settings,
          &ControllerSettingsWidget::updateMultitapControllerTitles);
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::setCategoryHelpTexts()
{
  m_category_help_text[static_cast<int>(Category::GeneralSettings)] = tr(
    "<strong>General Settings</strong><hr>These options control how the emulator looks and behaves.<br><br>Mouse over "
    "an option for additional information.");
  m_category_help_text[static_cast<int>(Category::ConsoleSettings)] =
    tr("<strong>Console Settings</strong><hr>These options determine the configuration of the simulated "
       "console.<br><br>Mouse over an option for additional information.");
  m_category_help_text[static_cast<int>(Category::GameListSettings)] =
    tr("<strong>Game List Settings</strong><hr>The list above shows the directories which will be searched by "
       "DuckStation to populate the game list. Search directories can be added, removed, and switched to "
       "recursive/non-recursive.");
  m_category_help_text[static_cast<int>(Category::HotkeySettings)] = tr(
    "<strong>Hotkey Settings</strong><hr>Binding a hotkey allows you to trigger events such as a resetting or taking "
    "screenshots at the press of a key/controller button. Hotkey titles are self-explanatory. Clicking a binding will "
    "start a countdown, in which case you should press the key or controller button/axis you wish to bind. If no "
    "button  is pressed and the timer lapses, the binding will be unchanged. To clear a binding, right-click the "
    "button. To  bind multiple buttons, hold Shift and click the button.");
  m_category_help_text[static_cast<int>(Category::ControllerSettings)] = tr(
    "<strong>Controller Settings</strong><hr>This page lets you choose the type of controller you wish to simulate for "
    "the console, and rebind the keys or host game controller buttons to your choosing. Clicking a binding will start "
    "a countdown, in which case you should press the key or controller button/axis you wish to bind. (For rumble, "
    "press any button/axis on the controller you wish to send rumble to.) If no button is pressed and the timer "
    "lapses, the binding will be unchanged. To clear a binding, right-click the button. To bind multiple buttons, hold "
    "Shift and click the button.");
  m_category_help_text[static_cast<int>(Category::MemoryCardSettings)] =
    tr("<strong>Memory Card Settings</strong><hr>This page lets you control what mode the memory card emulation will "
       "function in, and where the images for these cards will be stored on disk.");
  m_category_help_text[static_cast<int>(Category::DisplaySettings)] =
    tr("<strong>Display Settings</strong><hr>These options control the how the frames generated by the console are "
       "displayed on the screen.");
  m_category_help_text[static_cast<int>(Category::EnhancementSettings)] =
    tr("<strong>Enhancement Settings</strong><hr>These options control enhancements which can improve visuals compared "
       "to the original console. Mouse over each option for additional information.");
  m_category_help_text[static_cast<int>(Category::PostProcessingSettings)] =
    tr("<strong>Post-Processing Settings</strong><hr>Post processing allows you to alter the appearance of the image "
       "displayed on the screen with various filters. Shaders will be executed in sequence.");
  m_category_help_text[static_cast<int>(Category::AudioSettings)] =
    tr("<strong>Audio Settings</strong><hr>These options control the audio output of the console. Mouse over an option "
       "for additional information.");
  m_category_help_text[static_cast<int>(Category::AdvancedSettings)] = tr(
    "<strong>Advanced Settings</strong><hr>These options control logging and internal behavior of the emulator. Mouse "
    "over an option for additional information.");
}

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
  m_ui.helpText->setText(m_category_help_text[row]);
}

void SettingsDialog::onRestoreDefaultsClicked()
{
  if (QMessageBox::question(this, tr("Confirm Restore Defaults"),
                            tr("Are you sure you want to restore the default settings? Any preferences will be lost."),
                            QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
  {
    return;
  }

  m_host_interface->setDefaultSettings();
}

void SettingsDialog::registerWidgetHelp(QObject* object, QString title, QString recommended_value, QString text)
{
  // construct rich text with formatted description
  QString full_text;
  full_text += "<table width='100%' cellpadding='0' cellspacing='0'><tr><td><strong>";
  full_text += title;
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
      m_ui.helpText->setText(m_category_help_text[m_ui.settingsCategory->currentRow()]);
    }
  }

  return QDialog::eventFilter(object, event);
}