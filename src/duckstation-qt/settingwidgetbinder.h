#pragma once
#include <optional>
#include <type_traits>

#include "core/settings.h"
#include "qthostinterface.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtGui/QAction>
#else
#include <QtWidgets/QAction>
#endif

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>

namespace SettingWidgetBinder {

template<typename T>
struct SettingAccessor
{
  static bool getBoolValue(const T* widget);
  static void setBoolValue(T* widget, bool value);

  static int getIntValue(const T* widget);
  static void setIntValue(T* widget, int value);

  static int getFloatValue(const T* widget);
  static void setFloatValue(T* widget, int value);

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
  static void setIntValue(QLineEdit* widget, int value) { widget->setText(QString::number(value)); }

  static float getFloatValue(const QLineEdit* widget) { return widget->text().toFloat(); }
  static void setFloatValue(QLineEdit* widget, float value) { widget->setText(QString::number(value)); }

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
  static bool getBoolValue(const QComboBox* widget) { return widget->currentIndex() > 0; }
  static void setBoolValue(QComboBox* widget, bool value) { widget->setCurrentIndex(value ? 1 : 0); }

  static int getIntValue(const QComboBox* widget) { return widget->currentIndex(); }
  static void setIntValue(QComboBox* widget, int value) { widget->setCurrentIndex(value); }

  static float getFloatValue(const QComboBox* widget) { return static_cast<float>(widget->currentIndex()); }
  static void setFloatValue(QComboBox* widget, float value) { widget->setCurrentIndex(static_cast<int>(value)); }

  static QString getStringValue(const QComboBox* widget)
  {
    const QVariant currentData(widget->currentData());
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (currentData.type() == QVariant::String)
      return currentData.toString();
#else
    if (currentData.metaType().id() == QMetaType::QString)
      return currentData.toString();
#endif

    return widget->currentText();
  }

  static void setStringValue(QComboBox* widget, const QString& value)
  {
    const int index = widget->findData(value);
    if (index >= 0)
    {
      widget->setCurrentIndex(index);
      return;
    }

    widget->setCurrentText(value);
  }

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

  static float getFloatValue(const QCheckBox* widget) { return widget->isChecked() ? 1.0f : 0.0f; }
  static void setFloatValue(QCheckBox* widget, float value) { widget->setChecked(value != 0.0f); }

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

  static float getFloatValue(const QSlider* widget) { return static_cast<float>(widget->value()); }
  static void setFloatValue(QSlider* widget, float value) { widget->setValue(static_cast<int>(value)); }

  static QString getStringValue(const QSlider* widget) { return QString::number(widget->value()); }
  static void setStringValue(QSlider* widget, const QString& value) { widget->setValue(value.toInt()); }

  template<typename F>
  static void connectValueChanged(QSlider* widget, F func)
  {
    widget->connect(widget, &QSlider::valueChanged, func);
  }
};

template<>
struct SettingAccessor<QSpinBox>
{
  static bool getBoolValue(const QSpinBox* widget) { return widget->value() > 0; }
  static void setBoolValue(QSpinBox* widget, bool value) { widget->setValue(value ? 1 : 0); }

  static int getIntValue(const QSpinBox* widget) { return widget->value(); }
  static void setIntValue(QSpinBox* widget, int value) { widget->setValue(value); }

  static float getFloatValue(const QSpinBox* widget) { return static_cast<float>(widget->value()); }
  static void setFloatValue(QSpinBox* widget, float value) { widget->setValue(static_cast<int>(value)); }

  static QString getStringValue(const QSpinBox* widget) { return QString::number(widget->value()); }
  static void setStringValue(QSpinBox* widget, const QString& value) { widget->setValue(value.toInt()); }

  template<typename F>
  static void connectValueChanged(QSpinBox* widget, F func)
  {
    widget->connect(widget, QOverload<int>::of(&QSpinBox::valueChanged), func);
  }
};

template<>
struct SettingAccessor<QDoubleSpinBox>
{
  static bool getBoolValue(const QDoubleSpinBox* widget) { return widget->value() > 0.0; }
  static void setBoolValue(QDoubleSpinBox* widget, bool value) { widget->setValue(value ? 1.0 : 0.0); }

  static int getIntValue(const QDoubleSpinBox* widget) { return static_cast<int>(widget->value()); }
  static void setIntValue(QDoubleSpinBox* widget, int value) { widget->setValue(static_cast<double>(value)); }

  static float getFloatValue(const QDoubleSpinBox* widget) { return static_cast<float>(widget->value()); }
  static void setFloatValue(QDoubleSpinBox* widget, float value) { widget->setValue(static_cast<double>(value)); }

  static QString getStringValue(const QDoubleSpinBox* widget) { return QString::number(widget->value()); }
  static void setStringValue(QDoubleSpinBox* widget, const QString& value) { widget->setValue(value.toDouble()); }

  template<typename F>
  static void connectValueChanged(QDoubleSpinBox* widget, F func)
  {
    widget->connect(widget, QOverload<double>::of(&QDoubleSpinBox::valueChanged), func);
  }
};

template<>
struct SettingAccessor<QAction>
{
  static bool getBoolValue(const QAction* widget) { return widget->isChecked(); }
  static void setBoolValue(QAction* widget, bool value) { widget->setChecked(value); }

  static int getIntValue(const QAction* widget) { return widget->isChecked() ? 1 : 0; }
  static void setIntValue(QAction* widget, int value) { widget->setChecked(value != 0); }

  static float getFloatValue(const QAction* widget) { return widget->isChecked() ? 1.0f : 0.0f; }
  static void setFloatValue(QAction* widget, float value) { widget->setChecked(value != 0.0f); }

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
void BindWidgetToBoolSetting(QtHostInterface* hi, WidgetType* widget, std::string section, std::string key,
                             bool default_value = false)
{
  using Accessor = SettingAccessor<WidgetType>;

  bool value = hi->GetBoolSettingValue(section.c_str(), key.c_str(), default_value);

  Accessor::setBoolValue(widget, value);

  Accessor::connectValueChanged(widget, [hi, widget, section, key]() {
    const bool new_value = Accessor::getBoolValue(widget);
    hi->SetBoolSettingValue(section.c_str(), key.c_str(), new_value);
    hi->applySettings();
  });
}

template<typename WidgetType>
void BindWidgetToIntSetting(QtHostInterface* hi, WidgetType* widget, std::string section, std::string key,
                            int default_value = 0)
{
  using Accessor = SettingAccessor<WidgetType>;

  s32 value = hi->GetIntSettingValue(section.c_str(), key.c_str(), static_cast<s32>(default_value));

  Accessor::setIntValue(widget, static_cast<int>(value));

  Accessor::connectValueChanged(widget, [hi, widget, section, key]() {
    const int new_value = Accessor::getIntValue(widget);
    hi->SetIntSettingValue(section.c_str(), key.c_str(), new_value);
    hi->applySettings();
  });
}

template<typename WidgetType>
void BindWidgetToFloatSetting(QtHostInterface* hi, WidgetType* widget, std::string section, std::string key,
                              float default_value = 0.0f)
{
  using Accessor = SettingAccessor<WidgetType>;

  float value = hi->GetFloatSettingValue(section.c_str(), key.c_str(), default_value);

  Accessor::setFloatValue(widget, value);

  Accessor::connectValueChanged(widget, [hi, widget, section, key]() {
    const float new_value = Accessor::getFloatValue(widget);
    hi->SetFloatSettingValue(section.c_str(), key.c_str(), new_value);
    hi->applySettings();
  });
}

template<typename WidgetType>
void BindWidgetToNormalizedSetting(QtHostInterface* hi, WidgetType* widget, std::string section, std::string key,
                                   float range, float default_value = 0.0f)
{
  using Accessor = SettingAccessor<WidgetType>;

  float value = hi->GetFloatSettingValue(section.c_str(), key.c_str(), default_value);

  Accessor::setIntValue(widget, static_cast<int>(value * range));

  Accessor::connectValueChanged(widget, [hi, widget, section, key, range]() {
    const float new_value = (static_cast<float>(Accessor::getIntValue(widget)) / range);
    hi->SetFloatSettingValue(section.c_str(), key.c_str(), new_value);
    hi->applySettings();
  });
}

template<typename WidgetType>
void BindWidgetToStringSetting(QtHostInterface* hi, WidgetType* widget, std::string section, std::string key,
                               std::string default_value = std::string())
{
  using Accessor = SettingAccessor<WidgetType>;

  std::string value = hi->GetStringSettingValue(section.c_str(), key.c_str(), default_value.c_str());

  Accessor::setStringValue(widget, QString::fromStdString(value));

  Accessor::connectValueChanged(widget, [hi, widget, section, key]() {
    const QString new_value = Accessor::getStringValue(widget);
    if (!new_value.isEmpty())
      hi->SetStringSettingValue(section.c_str(), key.c_str(), new_value.toUtf8().constData());
    else
      hi->RemoveSettingValue(section.c_str(), key.c_str());

    hi->applySettings();
  });
}

template<typename WidgetType, typename DataType>
void BindWidgetToEnumSetting(QtHostInterface* hi, WidgetType* widget, std::string section, std::string key,
                             std::optional<DataType> (*from_string_function)(const char* str),
                             const char* (*to_string_function)(DataType value), DataType default_value)
{
  using Accessor = SettingAccessor<WidgetType>;
  using UnderlyingType = std::underlying_type_t<DataType>;

  // TODO: Clean this up?
  const std::string old_setting_string_value =
    hi->GetStringSettingValue(section.c_str(), key.c_str(), to_string_function(default_value));
  const std::optional<DataType> old_setting_value = from_string_function(old_setting_string_value.c_str());
  if (old_setting_value.has_value())
    Accessor::setIntValue(widget, static_cast<int>(static_cast<UnderlyingType>(old_setting_value.value())));
  else
    Accessor::setIntValue(widget, static_cast<int>(static_cast<UnderlyingType>(default_value)));

  Accessor::connectValueChanged(widget, [hi, widget, section, key, to_string_function]() {
    const DataType value = static_cast<DataType>(static_cast<UnderlyingType>(Accessor::getIntValue(widget)));
    const char* string_value = to_string_function(value);
    hi->SetStringSettingValue(section.c_str(), key.c_str(), string_value);
    hi->applySettings();
  });
}

} // namespace SettingWidgetBinder
