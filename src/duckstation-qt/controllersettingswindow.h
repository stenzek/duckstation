// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_controllersettingswindow.h"

#include "util/input_manager.h"

#include "common/types.h"

#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

#include <array>
#include <string>
#include <utility>
#include <vector>

class Error;

class ControllerGlobalSettingsWidget;
class ControllerBindingWidget;
class HotkeySettingsWidget;

class SettingsInterface;

class ControllerSettingsWindow final : public QWidget
{
  Q_OBJECT

public:
  enum class Category
  {
    GlobalSettings,
    FirstControllerSettings,
    HotkeySettings,
    Count
  };

  enum : u32
  {
    MAX_PORTS = 8
  };

  ControllerSettingsWindow(SettingsInterface* game_sif = nullptr, QWidget* parent = nullptr);
  ~ControllerSettingsWindow();

  static void editControllerSettingsForGame(QWidget* parent, SettingsInterface* sif);

  ALWAYS_INLINE HotkeySettingsWidget* getHotkeySettingsWidget() const { return m_hotkey_settings; }

  ALWAYS_INLINE const std::vector<std::pair<std::string, std::string>>& getDeviceList() const { return m_device_list; }
  ALWAYS_INLINE const QStringList& getVibrationMotors() const { return m_vibration_motors; }

  ALWAYS_INLINE bool isEditingGlobalSettings() const
  {
    return (m_profile_name.isEmpty() && !m_editing_settings_interface);
  }
  ALWAYS_INLINE bool isEditingGameSettings() const
  {
    return (m_profile_name.isEmpty() && m_editing_settings_interface);
  }
  ALWAYS_INLINE bool isEditingProfile() const { return !m_profile_name.isEmpty(); }
  ALWAYS_INLINE SettingsInterface* getEditingSettingsInterface() { return m_editing_settings_interface; }

  Category getCurrentCategory() const;

  void updateListDescription(u32 global_slot, ControllerBindingWidget* widget);

  void switchProfile(const std::string_view name);

  // Helper functions for updating setting values globally or in the profile.
  bool getBoolValue(const char* section, const char* key, bool default_value) const;
  s32 getIntValue(const char* section, const char* key, s32 default_value) const;
  std::string getStringValue(const char* section, const char* key, const char* default_value) const;
  void setBoolValue(const char* section, const char* key, bool value);
  void setIntValue(const char* section, const char* key, s32 value);
  void setStringValue(const char* section, const char* key, const char* value);
  void clearSettingValue(const char* section, const char* key);
  void saveAndReloadGameSettings();

Q_SIGNALS:
  void windowClosed();
  void inputProfileSwitched();

public Q_SLOTS:
  void setCategory(Category category);

private Q_SLOTS:
  void onCategoryCurrentRowChanged(int row);
  void onCurrentProfileChanged(int index);
  void onNewProfileClicked();
  void onApplyProfileClicked();
  void onDeleteProfileClicked();
  void onRestoreDefaultsClicked();
  void onCopyGlobalSettingsClicked();
  void onRestoreDefaultsForGameClicked();

  void onInputDevicesEnumerated(const std::vector<std::pair<std::string, std::string>>& devices);
  void onInputDeviceConnected(const std::string& identifier, const std::string& device_name);
  void onInputDeviceDisconnected(const std::string& identifier);
  void onVibrationMotorsEnumerated(const QList<InputBindingKey>& motors);

  void createWidgets();

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  int getHotkeyCategoryIndex() const;
  void refreshProfileList();

  std::array<bool, 2> getEnabledMultitaps() const;

  Ui::ControllerSettingsWindow m_ui;

  SettingsInterface* m_editing_settings_interface = nullptr;

  ControllerGlobalSettingsWidget* m_global_settings = nullptr;
  std::array<ControllerBindingWidget*, MAX_PORTS> m_port_bindings{};
  HotkeySettingsWidget* m_hotkey_settings = nullptr;

  std::vector<std::pair<std::string, std::string>> m_device_list;
  QStringList m_vibration_motors;

  QString m_profile_name;
  std::unique_ptr<SettingsInterface> m_profile_settings_interface;
};
