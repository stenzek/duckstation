// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controllersettingswindow.h"
#include "controllerbindingwidgets.h"
#include "controllerglobalsettingswidget.h"
#include "hotkeysettingswidget.h"
#include "qthost.h"

#include "core/controller.h"
#include "core/host.h"

#include "util/ini_settings_interface.h"
#include "util/input_manager.h"

#include "common/assert.h"
#include "common/file_system.h"

#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTextEdit>
#include <array>

ControllerSettingsWindow::ControllerSettingsWindow(SettingsInterface* game_sif /* = nullptr */,
                                                   QWidget* parent /* = nullptr */)
  : QWidget(parent), m_editing_settings_interface(game_sif)
{
  m_ui.setupUi(this);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  m_ui.settingsCategory->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this,
          &ControllerSettingsWindow::onCategoryCurrentRowChanged);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &ControllerSettingsWindow::close);

  if (!game_sif)
  {
    refreshProfileList();

    m_ui.editProfileLayout->removeWidget(m_ui.copyGlobalSettings);
    delete m_ui.copyGlobalSettings;
    m_ui.copyGlobalSettings = nullptr;

    connect(m_ui.currentProfile, &QComboBox::currentIndexChanged, this,
            &ControllerSettingsWindow::onCurrentProfileChanged);
    connect(m_ui.newProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onNewProfileClicked);
    connect(m_ui.applyProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onApplyProfileClicked);
    connect(m_ui.deleteProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onDeleteProfileClicked);
    connect(m_ui.restoreDefaults, &QPushButton::clicked, this, &ControllerSettingsWindow::onRestoreDefaultsClicked);

    connect(g_emu_thread, &EmuThread::onInputDevicesEnumerated, this,
            &ControllerSettingsWindow::onInputDevicesEnumerated);
    connect(g_emu_thread, &EmuThread::onInputDeviceConnected, this, &ControllerSettingsWindow::onInputDeviceConnected);
    connect(g_emu_thread, &EmuThread::onInputDeviceDisconnected, this,
            &ControllerSettingsWindow::onInputDeviceDisconnected);
    connect(g_emu_thread, &EmuThread::onVibrationMotorsEnumerated, this,
            &ControllerSettingsWindow::onVibrationMotorsEnumerated);

    // trigger a device enumeration to populate the device list
    g_emu_thread->enumerateInputDevices();
    g_emu_thread->enumerateVibrationMotors();
  }
  else
  {
    m_ui.editProfileLayout->removeWidget(m_ui.editProfileLabel);
    delete m_ui.editProfileLabel;
    m_ui.editProfileLabel = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.currentProfile);
    delete m_ui.currentProfile;
    m_ui.currentProfile = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.newProfile);
    delete m_ui.newProfile;
    m_ui.newProfile = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.applyProfile);
    delete m_ui.applyProfile;
    m_ui.applyProfile = nullptr;
    m_ui.editProfileLayout->removeWidget(m_ui.deleteProfile);
    delete m_ui.deleteProfile;
    m_ui.deleteProfile = nullptr;

    connect(m_ui.copyGlobalSettings, &QPushButton::clicked, this,
            &ControllerSettingsWindow::onCopyGlobalSettingsClicked);
    connect(m_ui.restoreDefaults, &QPushButton::clicked, this,
            &ControllerSettingsWindow::onRestoreDefaultsForGameClicked);
  }

  createWidgets();
}

ControllerSettingsWindow::~ControllerSettingsWindow() = default;

void ControllerSettingsWindow::editControllerSettingsForGame(QWidget* parent, SettingsInterface* sif)
{
  ControllerSettingsWindow* dlg = new ControllerSettingsWindow(sif, parent);
  dlg->setWindowFlag(Qt::Window);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowModality(Qt::WindowModality::WindowModal);
  dlg->setWindowTitle(parent->windowTitle());
  dlg->setWindowIcon(parent->windowIcon());
  dlg->show();
}

int ControllerSettingsWindow::getHotkeyCategoryIndex() const
{
  const std::array<bool, 2> mtap_enabled = getEnabledMultitaps();
  return 1 + (mtap_enabled[0] ? 4 : 1) + (mtap_enabled[1] ? 4 : 1);
}

ControllerSettingsWindow::Category ControllerSettingsWindow::getCurrentCategory() const
{
  const int index = m_ui.settingsCategory->currentRow();
  if (index == 0)
    return Category::GlobalSettings;
  else if (index >= getHotkeyCategoryIndex())
    return Category::HotkeySettings;
  else
    return Category::FirstControllerSettings;
}

void ControllerSettingsWindow::setCategory(Category category)
{
  switch (category)
  {
    case Category::GlobalSettings:
      m_ui.settingsCategory->setCurrentRow(0);
      break;

    case Category::FirstControllerSettings:
      m_ui.settingsCategory->setCurrentRow(1);
      break;

    case Category::HotkeySettings:
      m_ui.settingsCategory->setCurrentRow(getHotkeyCategoryIndex());
      break;

    default:
      break;
  }
}

void ControllerSettingsWindow::onCategoryCurrentRowChanged(int row)
{
  m_ui.settingsContainer->setCurrentIndex(row);
}

void ControllerSettingsWindow::onCurrentProfileChanged(int index)
{
  std::string profile_name;
  if (index > 0)
    profile_name = m_ui.currentProfile->itemText(index).toStdString();

  switchProfile(profile_name);
}

void ControllerSettingsWindow::onNewProfileClicked()
{
  const std::string profile_name =
    QInputDialog::getText(this, tr("Create Input Profile"), tr("Enter the name for the new input profile:"))
      .toStdString();
  if (profile_name.empty())
    return;

  std::string profile_path = System::GetInputProfilePath(profile_name);
  if (FileSystem::FileExists(profile_path.c_str()))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("A profile with the name '%1' already exists.").arg(QString::fromStdString(profile_name)));
    return;
  }

  const int res = QMessageBox::question(this, tr("Create Input Profile"),
                                        tr("Do you want to copy all bindings from the currently-selected profile to "
                                           "the new profile? Selecting No will create a completely empty profile."),
                                        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (res == QMessageBox::Cancel)
    return;

  INISettingsInterface temp_si(std::move(profile_path));
  if (res == QMessageBox::Yes)
  {
    // copy from global or the current profile
    if (!m_editing_settings_interface)
    {
      const int hkres = QMessageBox::question(
        this, tr("Create Input Profile"),
        tr("Do you want to copy the current hotkey bindings from global settings to the new input profile?"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
      if (hkres == QMessageBox::Cancel)
        return;

      const bool copy_hotkey_bindings = (hkres == QMessageBox::Yes);
      if (copy_hotkey_bindings)
        temp_si.SetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", true);

      // from global
      auto lock = Host::GetSettingsLock();
      InputManager::CopyConfiguration(&temp_si, *Host::Internal::GetBaseSettingsLayer(), true, true, true,
                                      copy_hotkey_bindings);
    }
    else
    {
      // from profile
      const bool copy_hotkey_bindings =
        m_editing_settings_interface->GetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false);
      temp_si.SetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", copy_hotkey_bindings);
      InputManager::CopyConfiguration(&temp_si, *m_editing_settings_interface, true, true, true, copy_hotkey_bindings);
    }
  }
  else
  {
    // still need to copy the source config
    if (!m_editing_settings_interface)
      InputManager::CopyConfiguration(&temp_si, *Host::Internal::GetBaseSettingsLayer(), false, true, false, false);
    else
      InputManager::CopyConfiguration(&temp_si, *m_editing_settings_interface, false, true, false, false);
  }

  if (!temp_si.Save())
  {
    QMessageBox::critical(
      this, tr("Error"),
      tr("Failed to save the new profile to '%1'.").arg(QString::fromStdString(temp_si.GetPath())));
    return;
  }

  refreshProfileList();
  switchProfile(profile_name);
}

void ControllerSettingsWindow::onApplyProfileClicked()
{
  if (QMessageBox::question(this, tr("Load Input Profile"),
                            tr("Are you sure you want to load the input profile named '%1'?\n\n"
                               "All current global bindings will be removed, and the profile bindings loaded.\n\n"
                               "You cannot undo this action.")
                              .arg(m_profile_name)) != QMessageBox::Yes)
  {
    return;
  }

  {
    const bool copy_hotkey_bindings =
      m_editing_settings_interface->GetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false);
    auto lock = Host::GetSettingsLock();
    InputManager::CopyConfiguration(Host::Internal::GetBaseSettingsLayer(), *m_editing_settings_interface, true, true,
                                    true, copy_hotkey_bindings);
    QtHost::QueueSettingsSave();
  }
  g_emu_thread->applySettings();

  // make it visible
  switchProfile({});
}

void ControllerSettingsWindow::onDeleteProfileClicked()
{
  if (QMessageBox::question(this, tr("Delete Input Profile"),
                            tr("Are you sure you want to delete the input profile named '%1'?\n\n"
                               "You cannot undo this action.")
                              .arg(m_profile_name)) != QMessageBox::Yes)
  {
    return;
  }

  std::string profile_path(System::GetInputProfilePath(m_profile_name.toStdString()));
  if (!FileSystem::DeleteFile(profile_path.c_str()))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to delete '%1'.").arg(QString::fromStdString(profile_path)));
    return;
  }

  // switch back to global
  refreshProfileList();
  switchProfile({});
}

void ControllerSettingsWindow::onRestoreDefaultsClicked()
{
  if (QMessageBox::question(
        this, tr("Restore Defaults"),
        tr("Are you sure you want to restore the default controller configuration?\n\n"
           "All shared bindings and configuration will be lost, but your input profiles will remain.\n\n"
           "You cannot undo this action.")) != QMessageBox::Yes)
  {
    return;
  }

  // actually restore it
  g_emu_thread->setDefaultSettings(false, true);

  // reload all settings
  switchProfile({});
}

void ControllerSettingsWindow::onCopyGlobalSettingsClicked()
{
  DebugAssert(isEditingGameSettings());

  {
    const auto lock = Host::GetSettingsLock();
    InputManager::CopyConfiguration(m_editing_settings_interface, *Host::Internal::GetBaseSettingsLayer(), true, true,
                                    true, false);
  }

  m_editing_settings_interface->Save();
  g_emu_thread->reloadGameSettings();
  createWidgets();

  QMessageBox::information(QtUtils::GetRootWidget(this), tr("DuckStation Controller Settings"),
                           tr("Per-game controller configuration reset to global settings."));
}

void ControllerSettingsWindow::onRestoreDefaultsForGameClicked()
{
  DebugAssert(isEditingGameSettings());
  Settings::SetDefaultControllerConfig(*m_editing_settings_interface);
  m_editing_settings_interface->Save();
  g_emu_thread->reloadGameSettings();
  createWidgets();

  QMessageBox::information(QtUtils::GetRootWidget(this), tr("DuckStation Controller Settings"),
                           tr("Per-game controller configuration reset to default settings."));
}

void ControllerSettingsWindow::onInputDevicesEnumerated(const std::vector<std::pair<std::string, std::string>>& devices)
{
  m_device_list = devices;
  for (const auto& [device_name, display_name] : m_device_list)
    m_global_settings->addDeviceToList(QString::fromStdString(device_name), QString::fromStdString(display_name));
}

void ControllerSettingsWindow::onInputDeviceConnected(const std::string& identifier, const std::string& device_name)
{
  m_device_list.emplace_back(identifier, device_name);
  m_global_settings->addDeviceToList(QString::fromStdString(identifier), QString::fromStdString(device_name));
  g_emu_thread->enumerateVibrationMotors();
}

void ControllerSettingsWindow::onInputDeviceDisconnected(const std::string& identifier)
{
  for (auto iter = m_device_list.begin(); iter != m_device_list.end(); ++iter)
  {
    if (iter->first == identifier)
    {
      m_device_list.erase(iter);
      break;
    }
  }

  m_global_settings->removeDeviceFromList(QString::fromStdString(identifier));
  g_emu_thread->enumerateVibrationMotors();
}

void ControllerSettingsWindow::onVibrationMotorsEnumerated(const QList<InputBindingKey>& motors)
{
  m_vibration_motors.clear();
  m_vibration_motors.reserve(motors.size());

  for (const InputBindingKey key : motors)
  {
    const std::string key_str(InputManager::ConvertInputBindingKeyToString(InputBindingInfo::Type::Motor, key));
    if (!key_str.empty())
      m_vibration_motors.push_back(QString::fromStdString(key_str));
  }
}

bool ControllerSettingsWindow::getBoolValue(const char* section, const char* key, bool default_value) const
{
  if (m_editing_settings_interface)
    return m_editing_settings_interface->GetBoolValue(section, key, default_value);
  else
    return Host::GetBaseBoolSettingValue(section, key, default_value);
}

s32 ControllerSettingsWindow::getIntValue(const char* section, const char* key, s32 default_value) const
{
  if (m_editing_settings_interface)
    return m_editing_settings_interface->GetIntValue(section, key, default_value);
  else
    return Host::GetBaseIntSettingValue(section, key, default_value);
}

std::string ControllerSettingsWindow::getStringValue(const char* section, const char* key,
                                                     const char* default_value) const
{
  std::string value;
  if (m_editing_settings_interface)
    value = m_editing_settings_interface->GetStringValue(section, key, default_value);
  else
    value = Host::GetBaseStringSettingValue(section, key, default_value);
  return value;
}

void ControllerSettingsWindow::setBoolValue(const char* section, const char* key, bool value)
{
  if (m_editing_settings_interface)
  {
    m_editing_settings_interface->SetBoolValue(section, key, value);
    saveAndReloadGameSettings();
  }
  else
  {
    Host::SetBaseBoolSettingValue(section, key, value);
    Host::CommitBaseSettingChanges();
    g_emu_thread->applySettings();
  }
}

void ControllerSettingsWindow::setIntValue(const char* section, const char* key, s32 value)
{
  if (m_editing_settings_interface)
  {
    m_editing_settings_interface->SetIntValue(section, key, value);
    saveAndReloadGameSettings();
  }
  else
  {
    Host::SetBaseIntSettingValue(section, key, value);
    Host::CommitBaseSettingChanges();
    g_emu_thread->applySettings();
  }
}

void ControllerSettingsWindow::setStringValue(const char* section, const char* key, const char* value)
{
  if (m_editing_settings_interface)
  {
    m_editing_settings_interface->SetStringValue(section, key, value);
    saveAndReloadGameSettings();
  }
  else
  {
    Host::SetBaseStringSettingValue(section, key, value);
    Host::CommitBaseSettingChanges();
    g_emu_thread->applySettings();
  }
}

void ControllerSettingsWindow::saveAndReloadGameSettings()
{
  DebugAssert(m_editing_settings_interface);
  QtHost::SaveGameSettings(m_editing_settings_interface, false);
  g_emu_thread->reloadGameSettings(false);
}

void ControllerSettingsWindow::clearSettingValue(const char* section, const char* key)
{
  if (m_editing_settings_interface)
  {
    m_editing_settings_interface->DeleteValue(section, key);
    m_editing_settings_interface->Save();
    g_emu_thread->reloadGameSettings();
  }
  else
  {
    Host::DeleteBaseSettingValue(section, key);
    Host::CommitBaseSettingChanges();
    g_emu_thread->applySettings();
  }
}

void ControllerSettingsWindow::createWidgets()
{
  QSignalBlocker sb(m_ui.settingsContainer);
  QSignalBlocker sb2(m_ui.settingsCategory);

  while (m_ui.settingsContainer->count() > 0)
  {
    QWidget* widget = m_ui.settingsContainer->widget(m_ui.settingsContainer->count() - 1);
    m_ui.settingsContainer->removeWidget(widget);
    widget->deleteLater();
  }

  m_ui.settingsCategory->clear();

  m_global_settings = nullptr;
  m_hotkey_settings = nullptr;

  {
    // global settings
    QListWidgetItem* item = new QListWidgetItem();
    item->setText(tr("Global Settings"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("settings-3-line")));
    m_ui.settingsCategory->addItem(item);
    m_ui.settingsCategory->setCurrentRow(0);
    m_global_settings = new ControllerGlobalSettingsWidget(m_ui.settingsContainer, this);
    m_ui.settingsContainer->addWidget(m_global_settings);
    connect(m_global_settings, &ControllerGlobalSettingsWidget::bindingSetupChanged, this,
            &ControllerSettingsWindow::createWidgets);
    if (!isEditingGameSettings())
    {
      for (const auto& [identifier, device_name] : m_device_list)
        m_global_settings->addDeviceToList(QString::fromStdString(identifier), QString::fromStdString(device_name));
    }
  }

  // load mtap settings
  const std::array<bool, 2> mtap_enabled = getEnabledMultitaps();
  for (u32 global_slot : Controller::PortDisplayOrder)
  {
    const bool is_mtap_port = Controller::PadIsMultitapSlot(global_slot);
    const auto [port, slot] = Controller::ConvertPadToPortAndSlot(global_slot);
    if (is_mtap_port && !mtap_enabled[port])
      continue;

    m_port_bindings[global_slot] = new ControllerBindingWidget(m_ui.settingsContainer, this, global_slot);
    m_ui.settingsContainer->addWidget(m_port_bindings[global_slot]);

    const QString display_name(QString::fromUtf8(m_port_bindings[global_slot]->getControllerInfo()->GetDisplayName()));

    QListWidgetItem* item = new QListWidgetItem();
    item->setText(tr("Controller Port %1\n%2")
                    .arg(Controller::GetPortDisplayName(port, slot, mtap_enabled[port]))
                    .arg(display_name));
    item->setIcon(m_port_bindings[global_slot]->getIcon());
    item->setData(Qt::UserRole, QVariant(global_slot));
    m_ui.settingsCategory->addItem(item);
  }

  // only add hotkeys if we're editing global settings
  if (!m_editing_settings_interface ||
      m_editing_settings_interface->GetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false))
  {
    QListWidgetItem* item = new QListWidgetItem();
    item->setText(tr("Hotkeys"));
    item->setIcon(QIcon::fromTheme(QStringLiteral("keyboard-line")));
    m_ui.settingsCategory->addItem(item);
    m_hotkey_settings = new HotkeySettingsWidget(m_ui.settingsContainer, this);
    m_ui.settingsContainer->addWidget(m_hotkey_settings);
  }

  if (!isEditingGameSettings())
  {
    m_ui.applyProfile->setEnabled(isEditingProfile());
    m_ui.deleteProfile->setEnabled(isEditingProfile());
    m_ui.restoreDefaults->setEnabled(isEditingGlobalSettings());
  }
}

void ControllerSettingsWindow::closeEvent(QCloseEvent* event)
{
  QWidget::closeEvent(event);
  emit windowClosed();
}

void ControllerSettingsWindow::updateListDescription(u32 global_slot, ControllerBindingWidget* widget)
{
  for (int i = 0; i < m_ui.settingsCategory->count(); i++)
  {
    QListWidgetItem* item = m_ui.settingsCategory->item(i);
    const QVariant item_data(item->data(Qt::UserRole));
    bool is_ok;
    if (item_data.toUInt(&is_ok) == global_slot && is_ok)
    {
      const std::array<bool, 2> mtap_enabled = getEnabledMultitaps();
      const auto [port, slot] = Controller::ConvertPadToPortAndSlot(global_slot);

      const QString display_name = QString::fromUtf8(widget->getControllerInfo()->GetDisplayName());

      item->setText(tr("Controller Port %1\n%2")
                      .arg(Controller::GetPortDisplayName(port, slot, mtap_enabled[port]))
                      .arg(display_name));
      item->setIcon(widget->getIcon());
      break;
    }
  }
}

std::array<bool, 2> ControllerSettingsWindow::getEnabledMultitaps() const
{
  const MultitapMode mtap_mode =
    Settings::ParseMultitapModeName(
      getStringValue("ControllerPorts", "MultitapMode", Settings::GetMultitapModeName(Settings::DEFAULT_MULTITAP_MODE))
        .c_str())
      .value_or(Settings::DEFAULT_MULTITAP_MODE);
  return {{(mtap_mode == MultitapMode::Port1Only || mtap_mode == MultitapMode::BothPorts),
           (mtap_mode == MultitapMode::Port2Only || mtap_mode == MultitapMode::BothPorts)}};
}
void ControllerSettingsWindow::refreshProfileList()
{
  const std::vector<std::string> names(InputManager::GetInputProfileNames());

  QSignalBlocker sb(m_ui.currentProfile);
  m_ui.currentProfile->clear();
  m_ui.currentProfile->addItem(tr("Shared"));
  if (isEditingGlobalSettings())
    m_ui.currentProfile->setCurrentIndex(0);

  for (const std::string& name : names)
  {
    const QString qname(QString::fromStdString(name));
    m_ui.currentProfile->addItem(qname);
    if (qname == m_profile_name)
      m_ui.currentProfile->setCurrentIndex(m_ui.currentProfile->count() - 1);
  }
}

void ControllerSettingsWindow::switchProfile(const std::string_view name)
{
  QSignalBlocker sb(m_ui.currentProfile);

  if (!name.empty())
  {
    const QString name_qstr = QtUtils::StringViewToQString(name);

    std::string path = System::GetInputProfilePath(name);
    if (!FileSystem::FileExists(path.c_str()))
    {
      QMessageBox::critical(this, tr("Error"), tr("The input profile named '%1' cannot be found.").arg(name_qstr));
      return;
    }

    std::unique_ptr<INISettingsInterface> sif = std::make_unique<INISettingsInterface>(std::move(path));
    sif->Load();

    m_profile_settings_interface = std::move(sif);
    m_editing_settings_interface = m_profile_settings_interface.get();
    m_ui.currentProfile->setCurrentIndex(m_ui.currentProfile->findText(name_qstr));
    m_profile_name = name_qstr;
  }
  else
  {
    m_profile_settings_interface.reset();
    m_editing_settings_interface = nullptr;
    m_ui.currentProfile->setCurrentIndex(0);
    m_profile_name = QString();
  }

  createWidgets();
}
