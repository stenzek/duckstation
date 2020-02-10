#pragma once
#include <optional>
#include <type_traits>

#include "core/settings.h"
#include "qthostinterface.h"

#include <QtWidgets/QAction>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSlider>

namespace SettingWidgetBinder {

template<typename T>
struct SettingAccessor
{
  static bool getBoolValue(const T* widget);
  static void setBoolValue(T* widget, bool value);

  static int getIntValue(const T* widget);
  static void setIntValue(T* widget, int value);

  static QString getStringValue(const T* widget);
  static void setStringValue(T* widget, const QString& value);

  template<typename F>
  static void connectValueChanged(T* widget, F func);
};

template<>
struct SettingAccessor<QLineEdit>
{
  static bool getBoolValue(const QLineEdit* widget) { return widget->text().toInt() != 0; }
  static void setBoolValue(QLineEdit* widget, bool value)
  {
    widget->setText(value ? QStringLiteral("1") : QStringLiteral("0"));
  }

  static int getIntValue(const QLineEdit* widget) { return widget->text().toInt(); }
  static void setIntValue(QLineEdit* widget, int value) { widget->setText(QStringLiteral("%1").arg(value)); }

  static QString getStringValue(const QLineEdit* widget) { return widget->text(); }
  static void setStringValue(QLineEdit* widget, const QString& value) { widget->setText(value); }

  template<typename F>
  static void connectValueChanged(QLineEdit* widget, F func)
  {
    widget->connect(widget, &QLineEdit::textChanged, func);
  }
};

template<>
struct SettingAccessor<QComboBox>
{
  static bool getBoolValue(const QComboBox* widget) { return widget->currentText() > 0; }
  static void setBoolValue(QComboBox* widget, bool value) { widget->setCurrentIndex(value ? 1 : 0); }

  static int getIntValue(const QComboBox* widget) { return widget->currentIndex(); }
  static void setIntValue(QComboBox* widget, int value) { widget->setCurrentIndex(value); }

  static QString getStringValue(const QComboBox* widget) { return widget->currentText(); }
  static void setStringValue(QComboBox* widget, const QString& value) { widget->setCurrentText(value); }

  template<typename F>
  static void connectValueChanged(QComboBox* widget, F func)
  {
    widget->connect(widget, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), func);
  }
};

template<>
struct SettingAccessor<QCheckBox>
{
  static bool getBoolValue(const QCheckBox* widget) { return widget->isChecked(); }
  static void setBoolValue(QCheckBox* widget, bool value) { widget->setChecked(value); }

  static int getIntValue(const QCheckBox* widget) { return widget->isChecked() ? 1 : 0; }
  static void setIntValue(QCheckBox* widget, int value) { widget->setChecked(value != 0); }

  static QString getStringValue(const QCheckBox* widget)
  {
    return widget->isChecked() ? QStringLiteral("1") : QStringLiteral("0");
  }
  static void setStringValue(QCheckBox* widget, const QString& value) { widget->setChecked(value.toInt() != 0); }

  template<typename F>
  static void connectValueChanged(QCheckBox* widget, F func)
  {
    widget->connect(widget, &QCheckBox::stateChanged, func);
  }
};

template<>
struct SettingAccessor<QSlider>
{
  static bool getBoolValue(const QSlider* widget) { return widget->value() > 0; }
  static void setBoolValue(QSlider* widget, bool value) { widget->setValue(value ? 1 : 0); }

  static int getIntValue(const QSlider* widget) { return widget->value(); }
  static void setIntValue(QSlider* widget, int value) { widget->setValue(value); }

  static QString getStringValue(const QSlider* widget) { return QStringLiteral("%1").arg(widget->value()); }
  static void setStringValue(QSlider* widget, const QString& value) { widget->setValue(value.toInt()); }

  template<typename F>
  static void connectValueChanged(QSlider* widget, F func)
  {
    widget->connect(widget, &QSlider::valueChanged, func);
  }
};

template<>
struct SettingAccessor<QAction>
{
  static bool getBoolValue(const QAction* widget) { return widget->isChecked(); }
  static void setBoolValue(QAction* widget, bool value) { widget->setChecked(value); }

  static int getIntValue(const QAction* widget) { return widget->isChecked() ? 1 : 0; }
  static void setIntValue(QAction* widget, int value) { widget->setChecked(value != 0); }

  static QString getStringValue(const QAction* widget)
  {
    return widget->isChecked() ? QStringLiteral("1") : QStringLiteral("0");
  }
  static void setStringValue(QAction* widget, const QString& value) { widget->setChecked(value.toInt() != 0); }

  template<typename F>
  static void connectValueChanged(QAction* widget, F func)
  {
    widget->connect(widget, &QAction::toggled, func);
  }
};

/// Binds a widget's value to a setting, updating it when the value changes.

template<typename WidgetType>
void BindWidgetToBoolSetting(QtHostInterface* hi, WidgetType* widget, const char* setting_name)
{
  using Accessor = SettingAccessor<WidgetType>;

  Accessor::setBoolValue(widget, hi->getSettingValue(setting_name).toBool());

  Accessor::connectValueChanged(widget, [hi, widget, setting_name]() {
    const bool new_value = Accessor::getBoolValue(widget);
    hi->putSettingValue(setting_name, new_value);
    hi->applySettings();
  });
}

template<typename WidgetType>
void BindWidgetToIntSetting(QtHostInterface* hi, WidgetType* widget, const char* setting_name)
{
  using Accessor = SettingAccessor<WidgetType>;

  Accessor::setIntValue(widget, hi->getSettingValue(setting_name).toInt());

  Accessor::connectValueChanged(widget, [hi, widget, setting_name]() {
    const int new_value = Accessor::getIntValue(widget);
    hi->putSettingValue(setting_name, new_value);
    hi->applySettings();
  });
}

template<typename WidgetType>
void BindWidgetToNormalizedSetting(QtHostInterface* hi, WidgetType* widget, const char* setting_name, float range)
{
  using Accessor = SettingAccessor<WidgetType>;

  Accessor::setIntValue(widget, static_cast<int>(hi->getSettingValue(setting_name).toFloat() * range));

  Accessor::connectValueChanged(widget, [hi, widget, setting_name, range]() {
    const float new_value = (static_cast<float>(Accessor::getIntValue(widget)) / range);
    hi->putSettingValue(setting_name, new_value);
    hi->applySettings();
  });
}

template<typename WidgetType>
void BindWidgetToStringSetting(QtHostInterface* hi, WidgetType* widget, const char* setting_name)
{
  using Accessor = SettingAccessor<WidgetType>;

  Accessor::setStringValue(widget, hi->getSettingValue(setting_name).toString());

  Accessor::connectValueChanged(widget, [hi, widget, setting_name]() {
    const QString new_value = Accessor::getStringValue(widget);
    hi->putSettingValue(setting_name, new_value);
    hi->applySettings();
  });
}

template<typename WidgetType, typename DataType>
void BindWidgetToEnumSetting(QtHostInterface* hi, WidgetType* widget, const char* setting_name,
                             std::optional<DataType> (*from_string_function)(const char* str),
                             const char* (*to_string_function)(DataType value))
{
  using Accessor = SettingAccessor<WidgetType>;
  using UnderlyingType = std::underlying_type_t<DataType>;

  const QString old_setting_string_value = hi->getSettingValue(setting_name).toString();
  const std::optional<DataType> old_setting_value =
    from_string_function(old_setting_string_value.toStdString().c_str());
  if (old_setting_value.has_value())
    Accessor::setIntValue(widget, static_cast<int>(static_cast<UnderlyingType>(old_setting_value.value())));

  Accessor::connectValueChanged(widget, [hi, widget, setting_name, to_string_function]() {
    const DataType value = static_cast<DataType>(static_cast<UnderlyingType>(Accessor::getIntValue(widget)));
    const char* string_value = to_string_function(value);
    hi->putSettingValue(setting_name, QString::fromLocal8Bit(string_value));
    hi->applySettings();
  });
}

} // namespace SettingWidgetBinder