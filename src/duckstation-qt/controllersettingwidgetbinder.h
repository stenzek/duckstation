// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "qthost.h"
#include "settingwidgetbinder.h"

#include "core/host.h"

#include <QtCore/QtCore>
#include <QtGui/QAction>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <optional>
#include <type_traits>

/// This nastyness is required because input profiles aren't overlaid settings like the rest of them, it's
/// input profile *or* global, not both.
namespace ControllerSettingWidgetBinder {
/// Interface specific method of BindWidgetToBoolSetting().
template<typename WidgetType>
inline void BindWidgetToInputProfileBool(SettingsInterface* sif, WidgetType* widget, std::string section,
                                         std::string key, bool default_value)
{
  using Accessor = SettingWidgetBinder::SettingAccessor<WidgetType>;

  if (sif)
  {
    const bool value = sif->GetBoolValue(section.c_str(), key.c_str(), default_value);
    Accessor::setBoolValue(widget, value);

    Accessor::connectValueChanged(widget, [sif, widget, section = std::move(section), key = std::move(key)]() {
      const bool new_value = Accessor::getBoolValue(widget);
      sif->SetBoolValue(section.c_str(), key.c_str(), new_value);
      QtHost::SaveGameSettings(sif, false);
      g_core_thread->reloadInputProfile();
    });
  }
  else
  {
    const bool value = Core::GetBaseBoolSettingValue(section.c_str(), key.c_str(), default_value);
    Accessor::setBoolValue(widget, value);

    Accessor::connectValueChanged(widget, [widget, section = std::move(section), key = std::move(key)]() {
      const bool new_value = Accessor::getBoolValue(widget);
      Core::SetBaseBoolSettingValue(section.c_str(), key.c_str(), new_value);
      Host::CommitBaseSettingChanges();
      g_core_thread->applySettings();
    });
  }
}

/// Interface specific method of BindWidgetToIntSetting().
template<typename WidgetType>
inline void BindWidgetToInputProfileInt(SettingsInterface* sif, WidgetType* widget, std::string section,
                                        std::string key, int default_value, int option_offset = 0)
{
  using Accessor = SettingWidgetBinder::SettingAccessor<WidgetType>;

  if (sif)
  {
    const int value = sif->GetIntValue(section.c_str(), key.c_str(), default_value) - option_offset;
    Accessor::setIntValue(widget, value);

    Accessor::connectValueChanged(widget,
                                  [sif, widget, section = std::move(section), key = std::move(key), option_offset]() {
                                    const int new_value = Accessor::getIntValue(widget);
                                    sif->SetIntValue(section.c_str(), key.c_str(), new_value + option_offset);
                                    QtHost::SaveGameSettings(sif, false);
                                    g_core_thread->reloadInputProfile();
                                  });
  }
  else
  {
    const int value = Core::GetBaseIntSettingValue(section.c_str(), key.c_str(), default_value) - option_offset;
    Accessor::setIntValue(widget, value);

    Accessor::connectValueChanged(
      widget, [widget, section = std::move(section), key = std::move(key), option_offset]() {
        const int new_value = Accessor::getIntValue(widget);
        Core::SetBaseIntSettingValue(section.c_str(), key.c_str(), new_value + option_offset);
        Host::CommitBaseSettingChanges();
        g_core_thread->applySettings();
      });
  }
}

/// Interface specific method of BindWidgetToFloatSetting().
template<typename WidgetType>
inline void BindWidgetToInputProfileFloat(SettingsInterface* sif, WidgetType* widget, std::string section,
                                          std::string key, float default_value)
{
  using Accessor = SettingWidgetBinder::SettingAccessor<WidgetType>;

  if (sif)
  {
    const float value = sif->GetFloatValue(section.c_str(), key.c_str(), default_value);
    Accessor::setFloatValue(widget, value);

    Accessor::connectValueChanged(widget, [sif, widget, section = std::move(section), key = std::move(key)]() {
      const float new_value = Accessor::getFloatValue(widget);
      sif->SetFloatValue(section.c_str(), key.c_str(), new_value);
      QtHost::SaveGameSettings(sif, false);
      g_core_thread->reloadInputProfile();
    });
  }
  else
  {
    const float value = Core::GetBaseFloatSettingValue(section.c_str(), key.c_str(), default_value);
    Accessor::setFloatValue(widget, value);

    Accessor::connectValueChanged(widget, [widget, section = std::move(section), key = std::move(key)]() {
      const float new_value = Accessor::getFloatValue(widget);
      Core::SetBaseFloatSettingValue(section.c_str(), key.c_str(), new_value);
      Host::CommitBaseSettingChanges();
      g_core_thread->applySettings();
    });
  }
}

/// Interface specific method of BindWidgetToNormalizedSetting().
template<typename WidgetType>
inline void BindWidgetToInputProfileNormalized(SettingsInterface* sif, WidgetType* widget, std::string section,
                                               std::string key, float range, float default_value)
{
  using Accessor = SettingWidgetBinder::SettingAccessor<WidgetType>;

  if (sif)
  {
    const float value = sif->GetFloatValue(section.c_str(), key.c_str(), default_value);
    Accessor::setIntValue(widget, static_cast<int>(value * range));

    Accessor::connectValueChanged(widget, [sif, widget, section = std::move(section), key = std::move(key), range]() {
      const int new_value = Accessor::getIntValue(widget);
      sif->SetFloatValue(section.c_str(), key.c_str(), static_cast<float>(new_value) / range);
      QtHost::SaveGameSettings(sif, false);
      g_core_thread->reloadInputProfile();
    });
  }
  else
  {
    const float value = Core::GetBaseFloatSettingValue(section.c_str(), key.c_str(), default_value);
    Accessor::setIntValue(widget, static_cast<int>(value * range));

    Accessor::connectValueChanged(widget, [widget, section = std::move(section), key = std::move(key), range]() {
      const float new_value = (static_cast<float>(Accessor::getIntValue(widget)) / range);
      Core::SetBaseFloatSettingValue(section.c_str(), key.c_str(), new_value);
      Host::CommitBaseSettingChanges();
      g_core_thread->applySettings();
    });
  }
}

/// Interface specific method of BindWidgetToStringSetting().
template<typename WidgetType>
inline void BindWidgetToInputProfileString(SettingsInterface* sif, WidgetType* widget, std::string section,
                                           std::string key, std::string default_value = std::string())
{
  using Accessor = SettingWidgetBinder::SettingAccessor<WidgetType>;

  if (sif)
  {
    const QString value(
      QString::fromStdString(sif->GetStringValue(section.c_str(), key.c_str(), default_value.c_str())));

    Accessor::setStringValue(widget, value);

    Accessor::connectValueChanged(widget, [widget, sif, section = std::move(section), key = std::move(key)]() {
      sif->SetStringValue(section.c_str(), key.c_str(), Accessor::getStringValue(widget).toUtf8().constData());
      QtHost::SaveGameSettings(sif, false);
      g_core_thread->reloadInputProfile();
    });
  }
  else
  {
    const QString value(
      QString::fromStdString(Core::GetBaseStringSettingValue(section.c_str(), key.c_str(), default_value.c_str())));

    Accessor::setStringValue(widget, value);

    Accessor::connectValueChanged(widget, [widget, section = std::move(section), key = std::move(key)]() {
      Core::SetBaseStringSettingValue(section.c_str(), key.c_str(),
                                      Accessor::getStringValue(widget).toUtf8().constData());
      Host::CommitBaseSettingChanges();
      g_core_thread->applySettings();
    });
  }
}

/// Interface specific method of BindWidgetToEnumSetting().
template<typename WidgetType, typename DataType, typename ValueCountType>
inline void BindWidgetToInputProfileEnumSetting(SettingsInterface* sif, WidgetType* widget, std::string section,
                                                std::string key,
                                                std::optional<DataType> (*from_string_function)(const char* str),
                                                const char* (*to_string_function)(DataType value),
                                                const char* (*to_display_name_function)(DataType value),
                                                DataType default_value, ValueCountType value_count)
{
  using Accessor = SettingWidgetBinder::SettingAccessor<WidgetType>;
  using UnderlyingType = std::underlying_type_t<DataType>;

  for (UnderlyingType i = 0; i < static_cast<UnderlyingType>(value_count); i++)
    Accessor::addOption(widget, to_display_name_function(static_cast<DataType>(i)));

  const std::string value =
    sif ? sif->GetStringValue(section.c_str(), key.c_str(), to_string_function(default_value)) :
          Core::GetBaseStringSettingValue(section.c_str(), key.c_str(), to_string_function(default_value));
  const std::optional<DataType> typed_value = from_string_function(value.c_str());
  if (typed_value.has_value())
    Accessor::setIntValue(widget, static_cast<int>(static_cast<UnderlyingType>(typed_value.value())));
  else
    Accessor::setIntValue(widget, static_cast<int>(static_cast<UnderlyingType>(default_value)));

  if (sif)
  {
    Accessor::connectValueChanged(
      widget, [sif, widget, section = std::move(section), key = std::move(key), to_string_function]() {
        const DataType value = static_cast<DataType>(static_cast<UnderlyingType>(Accessor::getIntValue(widget)));
        const char* string_value = to_string_function(value);
        sif->SetStringValue(section.c_str(), key.c_str(), string_value);
        QtHost::SaveGameSettings(sif, true);
        g_core_thread->reloadInputProfile();
      });
  }
  else
  {
    Accessor::connectValueChanged(
      widget, [widget, section = std::move(section), key = std::move(key), to_string_function]() {
        const DataType value = static_cast<DataType>(static_cast<UnderlyingType>(Accessor::getIntValue(widget)));
        const char* string_value = to_string_function(value);
        Core::SetBaseStringSettingValue(section.c_str(), key.c_str(), string_value);
        Host::CommitBaseSettingChanges();
        g_core_thread->applySettings();
      });
  }
}
} // namespace ControllerSettingWidgetBinder
