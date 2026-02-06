// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "controllersettingswindow.h"
#include "controllerbindingwidgets.h"
#include "controllerglobalsettingswidget.h"
#include "hotkeysettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"

#include "core/controller.h"
#include "core/core.h"

#include "util/ini_settings_interface.h"
#include "util/input_manager.h"

#include "common/assert.h"
#include "common/file_system.h"

#include <QtWidgets/QInputDialog>
#include <QtWidgets/QTextEdit>
#include <array>

#include "moc_controllersettingswindow.cpp"

using namespace Qt::StringLiterals;

ControllerSettingsWindow::ControllerSettingsWindow(INISettingsInterface* game_sif /* = nullptr */,
                                                   bool edit_profiles /* = false */, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_editing_settings_interface(game_sif), m_editing_input_profiles(edit_profiles)
{
  m_ui.setupUi(this);
  m_ui.buttonBox->button(QDialogButtonBox::Close)->setDefault(true);

  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  connect(m_ui.settingsCategory, &QListWidget::currentRowChanged, this,
          &ControllerSettingsWindow::onCategoryCurrentRowChanged);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &ControllerSettingsWindow::close);

  if (!game_sif && !edit_profiles)
  {
    // editing global settings
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
    m_ui.editProfileLayout->removeWidget(m_ui.copyGlobalSettings);
    delete m_ui.copyGlobalSettings;
    m_ui.copyGlobalSettings = nullptr;

    if (QPushButton* button = m_ui.buttonBox->button(QDialogButtonBox::RestoreDefaults))
      connect(button, &QPushButton::clicked, this, &ControllerSettingsWindow::onRestoreDefaultsClicked);
  }
  else
  {
    if (QPushButton* button = m_ui.buttonBox->button(QDialogButtonBox::RestoreDefaults))
      m_ui.buttonBox->removeButton(button);

    connect(m_ui.copyGlobalSettings, &QPushButton::clicked, this,
            &ControllerSettingsWindow::onCopyGlobalSettingsClicked);

    if (edit_profiles)
    {
      setWindowTitle(tr("DuckStation Controller Presets"));
      refreshProfileList();

      connect(m_ui.currentProfile, &QComboBox::currentIndexChanged, this,
              &ControllerSettingsWindow::onCurrentProfileChanged);
      connect(m_ui.newProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onNewProfileClicked);
      connect(m_ui.applyProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onApplyProfileClicked);
      connect(m_ui.deleteProfile, &QPushButton::clicked, this, &ControllerSettingsWindow::onDeleteProfileClicked);
    }
    else
    {
      // editing game settings
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
    }
  }

  if (m_ui.settingsContainer->count() == 0)
    createWidgets();
}

ControllerSettingsWindow::~ControllerSettingsWindow() = default;

void ControllerSettingsWindow::editControllerSettingsForGame(QWidget* parent, INISettingsInterface* sif)
{
  ControllerSettingsWindow* dlg = new ControllerSettingsWindow(sif, false, parent);
  dlg->setWindowFlag(Qt::Window);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowModality(Qt::WindowModal);
  dlg->setWindowTitle(parent->windowTitle());
  dlg->setWindowIcon(parent->windowIcon());
  dlg->show();
}

int ControllerSettingsWindow::getHotkeyCategoryIndex() const
{
  const std::array<bool, 2> mtap_enabled = getEnabledMultitaps();
  return 1 + (mtap_enabled[0] ? 4 : 1) + (mtap_enabled[1] ? 4 : 1);
}

int ControllerSettingsWindow::getCategoryRow() const
{
  return m_ui.settingsCategory->currentRow();
}

void ControllerSettingsWindow::setCategoryRow(int row)
{
  m_ui.settingsCategory->setCurrentRow(row);
}

void ControllerSettingsWindow::setCategory(u32 category)
{
  switch (category)
  {
    case CATEGORY_GLOBAL_SETTINGS:
      m_ui.settingsCategory->setCurrentRow(0);
      break;

    case CATEGORY_FIRST_CONTROLLER_SETTINGS:
      m_ui.settingsCategory->setCurrentRow(1);
      break;

    case CATEGORY_HOTKEY_SETTINGS:
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
  switchProfile(m_ui.currentProfile->itemText(index).toStdString());
}

void ControllerSettingsWindow::onNewProfileClicked()
{
  const std::string profile_name =
    QInputDialog::getText(this, tr("Create Controller Preset"), tr("Enter the name for the new controller preset:"))
      .toStdString();
  if (profile_name.empty())
    return;

  std::string profile_path = System::GetInputProfilePath(profile_name);
  if (FileSystem::FileExists(profile_path.c_str()))
  {
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, tr("Error"),
      tr("A preset with the name '%1' already exists.").arg(QString::fromStdString(profile_name)));
    return;
  }

  const int res =
    QtUtils::MessageBoxQuestion(this, tr("Create Controller Preset"),
                                tr("Do you want to copy all bindings from the currently-selected preset to "
                                   "the new preset? Selecting No will create a completely empty preset."),
                                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (res == QMessageBox::Cancel)
    return;

  INISettingsInterface temp_si(std::move(profile_path));
  if (res == QMessageBox::Yes)
  {
    // copy from global or the current profile
    if (!m_editing_settings_interface)
    {
      const int hkres = QtUtils::MessageBoxQuestion(
        this, tr("Create Controller Preset"),
        tr("Do you want to copy the current hotkey bindings from global settings to the new controller preset?"),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
      if (hkres == QMessageBox::Cancel)
        return;

      const bool copy_hotkey_bindings = (hkres == QMessageBox::Yes);
      if (copy_hotkey_bindings)
        temp_si.SetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", true);

      // from global
      const auto lock = Core::GetSettingsLock();
      InputManager::CopyConfiguration(&temp_si, *Core::GetBaseSettingsLayer(), true, false, true, copy_hotkey_bindings);
    }
    else
    {
      // from profile
      const bool copy_hotkey_bindings =
        m_editing_settings_interface->GetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false);
      const bool copy_sources =
        m_editing_settings_interface->GetBoolValue("ControllerPorts", "UseProfileInputSources", false);
      temp_si.SetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", copy_hotkey_bindings);
      temp_si.SetBoolValue("ControllerPorts", "UseProfileInputSources", copy_sources);
      InputManager::CopyConfiguration(&temp_si, *m_editing_settings_interface, true, copy_sources, true,
                                      copy_hotkey_bindings);
    }
  }

  if (!temp_si.Save())
  {
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, tr("Error"),
      tr("Failed to save the new preset to '%1'.").arg(QString::fromStdString(temp_si.GetPath())));
    return;
  }

  refreshProfileList();
  switchProfile(profile_name);
}

void ControllerSettingsWindow::onApplyProfileClicked()
{
  if (QtUtils::MessageBoxQuestion(this, tr("Load Controller Preset"),
                                  tr("Are you sure you want to apply the controller preset named '%1'?\n\n"
                                     "All current global bindings will be removed, and the preset bindings loaded.\n\n"
                                     "You cannot undo this action.")
                                    .arg(m_profile_name)) != QMessageBox::Yes)
  {
    return;
  }

  {
    const bool copy_hotkey_bindings =
      m_editing_settings_interface->GetBoolValue("ControllerPorts", "UseProfileHotkeyBindings", false);
    const bool copy_sources =
      m_editing_settings_interface->GetBoolValue("ControllerPorts", "UseProfileInputSources", false);
    const auto lock = Core::GetSettingsLock();
    InputManager::CopyConfiguration(Core::GetBaseSettingsLayer(), *m_editing_settings_interface, true, copy_sources,
                                    true, copy_hotkey_bindings);
    QtHost::QueueSettingsSave();
  }
  g_core_thread->applySettings();

  // Recreate global widget on profile apply
  g_main_window->getControllerSettingsWindow()->createWidgets();
}

void ControllerSettingsWindow::onDeleteProfileClicked()
{
  if (QtUtils::MessageBoxQuestion(this, tr("Delete Controller Preset"),
                                  tr("Are you sure you want to delete the controller preset named '%1'?\n\n"
                                     "You cannot undo this action.")
                                    .arg(m_profile_name)) != QMessageBox::Yes)
  {
    return;
  }

  std::string profile_path(System::GetInputProfilePath(m_profile_name.toStdString()));
  if (!FileSystem::DeleteFile(profile_path.c_str()))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                             tr("Failed to delete '%1'.").arg(QString::fromStdString(profile_path)));
    return;
  }

  // switch back to global
  refreshProfileList();
  switchProfile({});
}

void ControllerSettingsWindow::onRestoreDefaultsClicked()
{
  if (QtUtils::MessageBoxQuestion(this, tr("Restore Defaults"),
                                  tr("Are you sure you want to restore the default controller configuration?\n\n"
                                     "All bindings and configuration will be lost. You cannot undo this action.")) !=
      QMessageBox::Yes)
  {
    return;
  }

  // actually restore it
  g_core_thread->setDefaultSettings(false, false, true);

  // reload all settings
  createWidgets();
}

void ControllerSettingsWindow::onCopyGlobalSettingsClicked()
{
  DebugAssert(!isEditingGlobalSettings());

  {
    const auto lock = Core::GetSettingsLock();
    InputManager::CopyConfiguration(m_editing_settings_interface, *Core::GetBaseSettingsLayer(), true, false, true,
                                    false);
  }

  m_editing_settings_interface->Save();
  g_core_thread->reloadGameSettings();
  createWidgets();

  QtUtils::AsyncMessageBox(this, QMessageBox::Information, tr("DuckStation Controller Settings"),
                           isEditingGameSettings() ? tr("Per-game controller configuration reset to global settings.") :
                                                     tr("Controller preset reset to global settings."));
}

bool ControllerSettingsWindow::getBoolValue(const char* section, const char* key, bool default_value) const
{
  if (m_editing_settings_interface)
    return m_editing_settings_interface->GetBoolValue(section, key, default_value);
  else
    return Core::GetBaseBoolSettingValue(section, key, default_value);
}

s32 ControllerSettingsWindow::getIntValue(const char* section, const char* key, s32 default_value) const
{
  if (m_editing_settings_interface)
    return m_editing_settings_interface->GetIntValue(section, key, default_value);
  else
    return Core::GetBaseIntSettingValue(section, key, default_value);
}

std::string ControllerSettingsWindow::getStringValue(const char* section, const char* key,
                                                     const char* default_value) const
{
  std::string value;
  if (m_editing_settings_interface)
    value = m_editing_settings_interface->GetStringValue(section, key, default_value);
  else
    value = Core::GetBaseStringSettingValue(section, key, default_value);
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
    Core::SetBaseBoolSettingValue(section, key, value);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
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
    Core::SetBaseIntSettingValue(section, key, value);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
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
    Core::SetBaseStringSettingValue(section, key, value);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
  }
}

void ControllerSettingsWindow::saveAndReloadGameSettings()
{
  DebugAssert(m_editing_settings_interface);
  QtHost::SaveGameSettings(m_editing_settings_interface, false);
  g_core_thread->reloadGameSettings(false);
}

void ControllerSettingsWindow::clearSettingValue(const char* section, const char* key)
{
  if (m_editing_settings_interface)
  {
    m_editing_settings_interface->DeleteValue(section, key);
    m_editing_settings_interface->Save();
    g_core_thread->reloadGameSettings();
  }
  else
  {
    Core::DeleteBaseSettingValue(section, key);
    Host::CommitBaseSettingChanges();
    g_core_thread->applySettings();
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
    item->setIcon(QIcon::fromTheme("settings-3-line"_L1));
    m_ui.settingsCategory->addItem(item);
    m_ui.settingsCategory->setCurrentRow(0);
    m_global_settings = new ControllerGlobalSettingsWidget(m_ui.settingsContainer, this);
    m_ui.settingsContainer->addWidget(m_global_settings);
    connect(m_global_settings, &ControllerGlobalSettingsWidget::bindingSetupChanged, this,
            &ControllerSettingsWindow::createWidgets);
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

    const QString display_name(
      QtUtils::StringViewToQString(m_port_bindings[global_slot]->getControllerInfo()->GetDisplayName()));

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
    item->setIcon(QIcon::fromTheme("keyboard-line"_L1));
    m_ui.settingsCategory->addItem(item);
    m_hotkey_settings = new HotkeySettingsWidget(m_ui.settingsContainer, this);
    m_ui.settingsContainer->addWidget(m_hotkey_settings);
  }

  if (isEditingProfile())
  {
    const bool enable_buttons = static_cast<bool>(m_profile_settings_interface);
    m_ui.applyProfile->setEnabled(enable_buttons);
    m_ui.deleteProfile->setEnabled(enable_buttons);
    m_ui.copyGlobalSettings->setEnabled(enable_buttons);
  }
}

void ControllerSettingsWindow::closeEvent(QCloseEvent* event)
{
  if (isEditingGlobalSettings())
    QtUtils::SaveWindowGeometry(this);

  QWidget::closeEvent(event);
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

      const QString display_name = QtUtils::StringViewToQString(widget->getControllerInfo()->GetDisplayName());

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

  bool current_profile_found = false;
  for (const std::string& name : names)
  {
    const QString qname(QString::fromStdString(name));
    m_ui.currentProfile->addItem(qname);
    if (qname == m_profile_name)
    {
      m_ui.currentProfile->setCurrentIndex(m_ui.currentProfile->count() - 1);
      current_profile_found = true;
    }
  }

  if (!current_profile_found)
    switchProfile(names.empty() ? std::string_view() : std::string_view(names.front()));
}

void ControllerSettingsWindow::switchProfile(const std::string_view name)
{
  const QString name_qstr = QtUtils::StringViewToQString(name);
  {
    QSignalBlocker sb(m_ui.currentProfile);
    m_ui.currentProfile->setCurrentIndex(m_ui.currentProfile->findText(name_qstr));
  }
  m_profile_name = name_qstr;
  m_profile_settings_interface.reset();
  m_editing_settings_interface = nullptr;

  // disable UI if there is no selection
  const bool disable_ui = name.empty();
  m_ui.settingsCategory->setDisabled(disable_ui);
  m_ui.settingsContainer->setDisabled(disable_ui);

  if (name_qstr.isEmpty())
  {
    createWidgets();
    return;
  }

  std::string path = System::GetInputProfilePath(name);
  if (!FileSystem::FileExists(path.c_str()))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                             tr("The controller preset named '%1' cannot be found.").arg(name_qstr));
    return;
  }

  std::unique_ptr<INISettingsInterface> sif = std::make_unique<INISettingsInterface>(std::move(path));
  sif->Load();

  m_profile_settings_interface = std::move(sif);
  m_editing_settings_interface = m_profile_settings_interface.get();

  createWidgets();
}
