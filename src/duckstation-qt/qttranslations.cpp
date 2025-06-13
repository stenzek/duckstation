// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "mainwindow.h"
#include "qthost.h"

#include "core/gpu_thread.h"
#include "core/host.h"

#include "util/imgui_manager.h"

#include "common/assert.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include "fmt/format.h"
#include "imgui.h"

#include <QtCore/QFile>
#include <QtCore/QTranslator>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QMessageBox>

#include <optional>
#include <vector>

#ifdef _WIN32
#include "common/windows_headers.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#endif

LOG_CHANNEL(Host);

#if 0
// Qt internal strings we'd like to have translated
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Services")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Hide %1")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Hide Others")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Show All")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Preferences...")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "Quit %1")
QT_TRANSLATE_NOOP("MAC_APPLICATION_MENU", "About %1")
#endif

namespace QtHost {
struct FontOrderInfo
{
  const char* language;
  ImGuiManager::TextFontOrder font_order;
};

static QString FixLanguageName(const QString& language);
static void UpdateFontOrder(std::string_view language);
static const FontOrderInfo* GetFontOrderInfo(std::string_view language);

static std::vector<QTranslator*> s_translators;
} // namespace QtHost

void QtHost::UpdateApplicationLanguage(QWidget* dialog_parent)
{
  for (QTranslator* translator : s_translators)
  {
    qApp->removeTranslator(translator);
    translator->deleteLater();
  }
  s_translators.clear();

  // Fix old language names.
  const std::string config_language = Host::GetBaseStringSettingValue("Main", "Language", GetDefaultLanguage());
  const QString language = FixLanguageName(QString::fromStdString(config_language));

  // install the base qt translation first
#ifndef __APPLE__
  const QString base_dir = QStringLiteral("%1/translations").arg(qApp->applicationDirPath());
#else
  const QString base_dir = QStringLiteral("%1/../Resources/translations").arg(qApp->applicationDirPath());
#endif

  // Qt base uses underscores instead of hyphens.
  const QString qt_language = QString(language).replace(QChar('-'), QChar('_'));
  QString base_path(QStringLiteral("%1/qt_%2.qm").arg(base_dir).arg(qt_language));
  bool has_base_ts = QFile::exists(base_path);
  if (!has_base_ts)
  {
    // Try without the country suffix.
    const int index = language.lastIndexOf('-');
    if (index > 0)
    {
      base_path = QStringLiteral("%1/qt_%2.qm").arg(base_dir).arg(language.left(index));
      has_base_ts = QFile::exists(base_path);
    }
  }
  if (has_base_ts)
  {
    QTranslator* base_translator = new QTranslator(qApp);
    if (!base_translator->load(base_path))
    {
      QMessageBox::warning(
        dialog_parent, QStringLiteral("Translation Error"),
        QStringLiteral("Failed to find load base translation file for '%1':\n%2").arg(language).arg(base_path));
      delete base_translator;
    }
    else
    {
      s_translators.push_back(base_translator);
      qApp->installTranslator(base_translator);
    }
  }

  const QString path = QStringLiteral("%1/duckstation-qt_%3.qm").arg(base_dir).arg(language);
  if (!QFile::exists(path))
  {
    QMessageBox::warning(
      dialog_parent, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to find translation file for language '%1':\n%2").arg(language).arg(path));
    return;
  }

  QTranslator* translator = new QTranslator(qApp);
  if (!translator->load(path))
  {
    QMessageBox::warning(
      dialog_parent, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to load translation file for language '%1':\n%2").arg(language).arg(path));
    delete translator;
    return;
  }

  INFO_LOG("Loaded translation file for language {}", language.toUtf8().constData());
  qApp->installTranslator(translator);
  s_translators.push_back(translator);

  // We end up here both on language change, and on startup.
  UpdateFontOrder(config_language);
}

QString QtHost::FixLanguageName(const QString& language)
{
  if (language == QStringLiteral("pt-br"))
    return QStringLiteral("pt-BR");
  else if (language == QStringLiteral("pt-pt"))
    return QStringLiteral("pt-PT");
  else if (language == QStringLiteral("zh-cn"))
    return QStringLiteral("zh-CN");
  else
    return language;
}

s32 Host::Internal::GetTranslatedStringImpl(std::string_view context, std::string_view msg,
                                            std::string_view disambiguation, char* tbuf, size_t tbuf_space)
{
  // This is really awful. Thankfully we're caching the results...
  const std::string temp_context(context);
  const std::string temp_msg(msg);
  const std::string temp_disambiguation(disambiguation);
  const QString translated_msg = qApp->translate(temp_context.c_str(), temp_msg.c_str(),
                                                 disambiguation.empty() ? nullptr : temp_disambiguation.c_str());
  const QByteArray translated_utf8 = translated_msg.toUtf8();
  const size_t translated_size = translated_utf8.size();
  if (translated_size > tbuf_space)
    return -1;
  else if (translated_size > 0)
    std::memcpy(tbuf, translated_utf8.constData(), translated_size);

  return static_cast<s32>(translated_size);
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
  return qApp->translate(context, msg, disambiguation, count).toStdString();
}

SmallString Host::TranslatePluralToSmallString(const char* context, const char* msg, const char* disambiguation,
                                               int count)
{
  const QString qstr = qApp->translate(context, msg, disambiguation, count);
  SmallString ret;

#ifdef _WIN32
  // Cheeky way to avoid heap allocations.
  static_assert(sizeof(*qstr.utf16()) == sizeof(wchar_t));
  ret.assign(std::wstring_view(reinterpret_cast<const wchar_t*>(qstr.utf16()), qstr.length()));
#else
  const QByteArray utf8 = qstr.toUtf8();
  ret.assign(utf8.constData(), utf8.length());
#endif

  return ret;
}

std::span<const std::pair<const char*, const char*>> Host::GetAvailableLanguageList()
{
  static constexpr const std::pair<const char*, const char*> languages[] = {{"English", "en"},
                                                                            {"Español de Latinoamérica", "es"},
                                                                            {"Español de España", "es-ES"},
                                                                            {"Français", "fr"},
                                                                            {"Bahasa Indonesia", "id"},
                                                                            {"日本語", "ja"},
                                                                            {"한국어", "ko"},
                                                                            {"Italiano", "it"},
                                                                            {"Polski", "pl"},
                                                                            {"Português (Pt)", "pt-PT"},
                                                                            {"Português (Br)", "pt-BR"},
                                                                            {"Русский", "ru"},
                                                                            {"Svenska", "sv"},
                                                                            {"Türkçe", "tr"},
                                                                            {"简体中文", "zh-CN"}};

  return languages;
}

bool Host::ChangeLanguage(const char* new_language)
{
  Host::RunOnUIThread([new_language = std::string(new_language)]() {
    Host::SetBaseStringSettingValue("Main", "Language", new_language.c_str());
    Host::CommitBaseSettingChanges();
    QtHost::UpdateApplicationLanguage(g_main_window);
    g_main_window->recreate();
  });
  return true;
}

const char* QtHost::GetDefaultLanguage()
{
  // TODO: Default system language instead.
  return "en";
}

void QtHost::UpdateFontOrder(std::string_view language)
{
  ImGuiManager::TextFontOrder font_order = ImGuiManager::GetDefaultTextFontOrder();
  if (const FontOrderInfo* fo = GetFontOrderInfo(language))
    font_order = fo->font_order;

  if (g_emu_thread)
  {
    Host::RunOnCPUThread([font_order]() mutable {
      GPUThread::RunOnThread([font_order]() mutable { ImGuiManager::SetTextFontOrder(font_order); });
      Host::ClearTranslationCache();
    });
  }
  else
  {
    // Startup, safe to set directly.
    ImGuiManager::SetTextFontOrder(font_order);
    Host::ClearTranslationCache();
  }
}

#define TF(name) ImGuiManager::TextFont::name

// Why is this a thing? Because we want all glyphs to be available, but don't want to conflict
// between codepoints shared between Chinese and Japanese. Therefore we prioritize the language
// that the user has selected.
static constexpr const QtHost::FontOrderInfo s_font_order[] = {
  {"ja", {TF(Default), TF(Japanese), TF(Chinese), TF(Korean)}},
  {"ko", {TF(Default), TF(Korean), TF(Japanese), TF(Chinese)}},
  {"zh-CN", {TF(Default), TF(Chinese), TF(Japanese), TF(Korean)}},
};

#undef TF

const QtHost::FontOrderInfo* QtHost::GetFontOrderInfo(std::string_view language)
{
  for (const FontOrderInfo& it : s_font_order)
  {
    if (language == it.language)
      return &it;
  }

  return nullptr;
}
