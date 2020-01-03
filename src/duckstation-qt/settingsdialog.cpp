#include "settingsdialog.h"
#include "consolesettingswidget.h"
#include "gamelistsettingswidget.h"
#include "gpusettingswidget.h"
#include "portsettingswidget.h"
#include "qthostinterface.h"
#include <QtWidgets/QTextEdit>

SettingsDialog::SettingsDialog(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QDialog(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  m_console_settings = new ConsoleSettingsWidget(host_interface, m_ui.settingsContainer);
  m_game_list_settings = new GameListSettingsWidget(host_interface, m_ui.settingsContainer);
  m_port_settings = new PortSettingsWidget(host_interface, m_ui.settingsContainer);
  m_gpu_settings = new GPUSettingsWidget(host_interface, m_ui.settingsContainer);
  m_audio_settings = new QWidget(m_ui.settingsContainer);

  m_ui.settingsContainer->insertWidget(0, m_console_settings);
  m_ui.settingsContainer->insertWidget(1, m_game_list_settings);
  m_ui.settingsContainer->insertWidget(2, m_port_settings);
  m_ui.settingsContainer->insertWidget(3, m_gpu_settings);
  m_ui.settingsContainer->insertWidget(4, m_audio_settings);

  m_ui.settingsCategory->setCurrentRow(0);
  m_ui.settingsContainer->setCurrentIndex(0);
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
  m_ui.settingsContainer->setCurrentIndex(row);
}
