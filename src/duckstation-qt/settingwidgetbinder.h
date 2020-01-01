#pragma once
#include <type_traits>

#include "core/settings.h"
#include "qthostinterface.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>

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
    widget->connect(widget, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), func);
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

/// Binds a widget's value to a setting, updating it when the value changes.
// template<typename WidgetType, typename DataType, typename = void>
// void BindWidgetToSetting(QtHostInterface* hi, WidgetType* widget, DataType Settings::*settings_ptr);

template<typename WidgetType>
void BindWidgetToSetting(QtHostInterface* hi, WidgetType* widget, bool Settings::*settings_ptr)
{
  using Accessor = SettingAccessor<WidgetType>;

  Accessor::setBoolValue(widget, hi->GetCoreSettings().*settings_ptr);

  Accessor::connectValueChanged(widget, [hi, widget, settings_ptr]() {
    (hi->GetCoreSettings().*settings_ptr) = Accessor::getBoolValue(widget);
    hi->updateQSettings();
  });
}

template<typename WidgetType>
void BindWidgetToSetting(QtHostInterface* hi, WidgetType* widget, std::string Settings::*settings_ptr)
{
  using Accessor = SettingAccessor<WidgetType>;

  Accessor::setStringValue(widget, QString::fromStdString(hi->GetCoreSettings().*settings_ptr));

  Accessor::connectValueChanged(widget, [hi, widget, settings_ptr]() {
    const QString value = Accessor::getStringValue(widget);
    (hi->GetCoreSettings().*settings_ptr) = value.toStdString();
    hi->updateQSettings();
  });
}

template<typename WidgetType, typename DataType>
void BindWidgetToSetting(QtHostInterface* hi, WidgetType* widget, DataType Settings::*settings_ptr,
                         std::enable_if_t<std::is_enum_v<DataType>, int>* v = nullptr)
{
  using Accessor = SettingAccessor<WidgetType>;
  using UnderlyingType = std::underlying_type_t<DataType>;

  Accessor::setIntValue(widget, static_cast<int>(static_cast<UnderlyingType>(hi->GetCoreSettings().*settings_ptr)));

  Accessor::connectValueChanged(widget, [hi, widget, settings_ptr](int) {
    const int value = Accessor::getIntValue(widget);
    (hi->GetCoreSettings().*settings_ptr) = static_cast<DataType>(static_cast<UnderlyingType>(value));
    hi->updateQSettings();
  });
}

} // namespace SettingWidgetBinder