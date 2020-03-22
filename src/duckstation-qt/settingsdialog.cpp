#include "settingsdialog.h"
#include "audiosettingswidget.h"
#include "consolesettingswidget.h"
#include "gamelistsettingswidget.h"
#include "generalsettingswidget.h"
#include "gpusettingswidget.h"
#include "hotkeysettingswidget.h"
#include "portsettingswidget.h"
#include "qthostinterface.h"
#include <QtWidgets/QTextEdit>

static constexpr char DEFAULT_SETTING_HELP_TEXT[] = "Mouse over an option for additional information.";

SettingsDialog::SettingsDialog(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QDialog(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  m_general_settings = new GeneralSettingsWidget(host_interface, m_ui.settingsContainer);
  m_console_settings = new ConsoleSettingsWidget(host_interface, m_ui.settingsContainer);
  m_game_list_settings = new GameListSettingsWidget(host_interface, m_ui.settingsContainer);
  m_hotkey_settings = new HotkeySettingsWidget(host_interface, m_ui.settingsContainer);
  m_port_settings = new PortSettingsWidget(host_interface, m_ui.settingsContainer);
  m_gpu_settings = new GPUSettingsWidget(host_interface, m_ui.settingsContainer);
  m_audio_settings = new AudioSettingsWidget(host_interface, m_ui.settingsContainer);

  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GeneralSettings), m_general_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::ConsoleSettings), m_console_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GameListSettings), m_game_list_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::HotkeySettings), m_hotkey_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::PortSettings), m_port_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::GPUSettings), m_gpu_settings);
  m_ui.settingsContainer->insertWidget(static_cast<int>(Category::AudioSettings), m_audio_settings);

  m_ui.settingsCategory->setCurrentRow(0);
  m_ui.settingsContainer->setCurrentIndex(0);
  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this, &SettingsDialog::onCategoryCurrentRowChanged);

  m_ui.helpText->setText(tr(DEFAULT_SETTING_HELP_TEXT));
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
  m_ui.settingsContainer->setCurrentIndex(row);
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
      m_ui.helpText->setText(tr(DEFAULT_SETTING_HELP_TEXT));
    }
  }

  return QDialog::eventFilter(object, event);
}