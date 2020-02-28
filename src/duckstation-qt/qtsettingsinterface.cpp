#include "qtsettingsinterface.h"
#include <QtCore/QSettings>
#include <algorithm>

static QString GetFullKey(const char* section, const char* key)
{
  return QStringLiteral("%1/%2").arg(section, key);
}

QtSettingsInterface::QtSettingsInterface(QSettings& settings) : m_settings(settings) {}

QtSettingsInterface::~QtSettingsInterface() = default;

int QtSettingsInterface::GetIntValue(const char* section, const char* key, int default_value /*= 0*/)
{
  QVariant value = m_settings.value(GetFullKey(section, key));
  if (!value.isValid())
    return default_value;

  bool converted_value_okay;
  int converted_value = value.toInt(&converted_value_okay);
  if (!converted_value_okay)
    return default_value;
  else
    return converted_value;
}

float QtSettingsInterface::GetFloatValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
  QVariant value = m_settings.value(GetFullKey(section, key));
  if (!value.isValid())
    return default_value;

  bool converted_value_okay;
  float converted_value = value.toFloat(&converted_value_okay);
  if (!converted_value_okay)
    return default_value;
  else
    return converted_value;
}

bool QtSettingsInterface::GetBoolValue(const char* section, const char* key, bool default_value /*= false*/)
{
  QVariant value = m_settings.value(GetFullKey(section, key));
  return value.isValid() ? value.toBool() : default_value;
}

std::string QtSettingsInterface::GetStringValue(const char* section, const char* key,
                                                const char* default_value /*= ""*/)
{
  QVariant value = m_settings.value(GetFullKey(section, key));
  return value.isValid() ? value.toString().toStdString() : std::string(default_value);
}

void QtSettingsInterface::SetIntValue(const char* section, const char* key, int value)
{
  m_settings.setValue(GetFullKey(section, key), QVariant(value));
}

void QtSettingsInterface::SetFloatValue(const char* section, const char* key, float value)
{
  m_settings.setValue(GetFullKey(section, key), QString::number(value));
}

void QtSettingsInterface::SetBoolValue(const char* section, const char* key, bool value)
{
  m_settings.setValue(GetFullKey(section, key), QVariant(value));
}

void QtSettingsInterface::SetStringValue(const char* section, const char* key, const char* value)
{
  m_settings.setValue(GetFullKey(section, key), QVariant(value));
}

std::vector<std::string> QtSettingsInterface::GetStringList(const char* section, const char* key)
{
  QVariant value = m_settings.value(GetFullKey(section, key));
  if (value.type() == QVariant::String)
    return {value.toString().toStdString()};
  else if (value.type() != QVariant::StringList)
    return {};

  QStringList value_sl = value.toStringList();
  std::vector<std::string> results;
  results.reserve(static_cast<unsigned>(value_sl.size()));
  std::transform(value_sl.begin(), value_sl.end(), std::back_inserter(results),
                 [](const QString& str) { return str.toStdString(); });
  return results;
}

void QtSettingsInterface::SetStringList(const char* section, const char* key,
                                        const std::vector<std::string_view>& items)
{
  QString full_key = GetFullKey(section, key);
  if (items.empty())
  {
    m_settings.remove(full_key);
    return;
  }

  QStringList sl;
  sl.reserve(static_cast<int>(items.size()));
  std::transform(items.begin(), items.end(), std::back_inserter(sl), [](const std::string_view& sv) {
    return QString::fromLocal8Bit(sv.data(), static_cast<int>(sv.size()));
  });
  m_settings.setValue(full_key, sl);
}

bool QtSettingsInterface::RemoveFromStringList(const char* section, const char* key, const char* item)
{
  QString full_key = GetFullKey(section, key);
  QVariant var = m_settings.value(full_key);
  QStringList sl = var.toStringList();
  if (sl.removeAll(item) == 0)
    return false;

  if (sl.isEmpty())
    m_settings.remove(full_key);
  else
    m_settings.setValue(full_key, sl);
  return true;
}

bool QtSettingsInterface::AddToStringList(const char* section, const char* key, const char* item)
{
  QString full_key = GetFullKey(section, key);
  QVariant var = m_settings.value(full_key);

  QStringList sl = (var.type() == QVariant::StringList) ? var.toStringList() : QStringList();
  QString qitem(item);
  if (sl.contains(qitem))
    return false;

  sl.push_back(qitem);
  m_settings.setValue(full_key, sl);
  return true;
}

void QtSettingsInterface::DeleteValue(const char* section, const char* key)
{
  m_settings.remove(GetFullKey(section, key));
}
