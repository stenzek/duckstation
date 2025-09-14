// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_controllersettingswindow.h"

#include "util/input_manager.h"

#include "core/types.h"

#include <QtCore/QAbstractListModel>
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

class INISettingsInterface;

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

  ControllerSettingsWindow(INISettingsInterface* game_sif = nullptr, bool edit_profiles = false,
                           QWidget* parent = nullptr);
  ~ControllerSettingsWindow();

  static void editControllerSettingsForGame(QWidget* parent, INISettingsInterface* sif);

  ALWAYS_INLINE HotkeySettingsWidget* getHotkeySettingsWidget() const { return m_hotkey_settings; }

  ALWAYS_INLINE bool isEditingGlobalSettings() const
  {
    return (!m_editing_input_profiles && !m_editing_settings_interface);
  }
  ALWAYS_INLINE bool isEditingGameSettings() const
  {
    return (!m_editing_input_profiles && m_editing_settings_interface);
  }
  ALWAYS_INLINE bool isEditingProfile() const { return m_editing_input_profiles; }
  ALWAYS_INLINE INISettingsInterface* getEditingSettingsInterface() { return m_editing_settings_interface; }

  Category getCurrentCategory() const;

  void updateListDescription(u32 global_slot, ControllerBindingWidget* widget);

  void switchProfile(const std::string_view name);

  void setCategory(Category category);

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

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  int getHotkeyCategoryIndex() const;
  void refreshProfileList();

  std::array<bool, 2> getEnabledMultitaps() const;

  void createWidgets();

  void onCategoryCurrentRowChanged(int row);
  void onCurrentProfileChanged(int index);
  void onNewProfileClicked();
  void onApplyProfileClicked();
  void onDeleteProfileClicked();
  void onRestoreDefaultsClicked();
  void onCopyGlobalSettingsClicked();

  Ui::ControllerSettingsWindow m_ui;

  INISettingsInterface* m_editing_settings_interface = nullptr;

  ControllerGlobalSettingsWidget* m_global_settings = nullptr;
  std::array<ControllerBindingWidget*, NUM_CONTROLLER_AND_CARD_PORTS> m_port_bindings{};
  HotkeySettingsWidget* m_hotkey_settings = nullptr;

  QString m_profile_name;
  std::unique_ptr<INISettingsInterface> m_profile_settings_interface;
  bool m_editing_input_profiles = false;
};
