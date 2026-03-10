// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qthost.h"

#include "core/core.h"
#include "core/fullscreenui_widgets.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"

#include <QtCore/QFile>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleFactory>

using namespace Qt::StringLiterals;

LOG_CHANNEL(Host);

namespace QtHost {
static bool ShouldDisableStyleSheet();
static void SetThemeAttributes(bool is_stylesheet_theme, bool is_variable_color_theme, bool is_dark_theme);
static bool NativeThemeStylesheetNeedsUpdate();
static void SetStyleFromSettings();
static void SetStyleSheet(const QString& stylesheet);
static QString GetNativeThemeStylesheet();
static bool LoadStyledFusionTheme(std::string_view name);

namespace {
struct ThemesLocals
{
  QString unthemed_style_name;
  QPalette unthemed_palette;
  bool is_stylesheet_theme = false;
  bool is_variable_color_theme = false;
  bool is_dark_theme = false;
  bool unthemed_style_name_set = false;

#ifdef __linux__
  bool use_system_font = false;
  bool system_font_set = false;
  QFont system_font;
#endif
};
} // namespace

static ThemesLocals s_themes_locals;

} // namespace QtHost

const char* QtHost::GetDefaultThemeName()
{
#ifndef __APPLE__
  return "darkerfusion";
#else
  return "";
#endif
}

void QtHost::UpdateApplicationTheme()
{
  if (!s_themes_locals.unthemed_style_name_set)
  {
    s_themes_locals.unthemed_style_name_set = true;
    s_themes_locals.unthemed_style_name = QApplication::style()->objectName();
    s_themes_locals.unthemed_palette = QApplication::palette();
  }

#ifdef __linux__
  // Fonts on Linux are ugly and too large. Override it by default.
  const bool use_system_font = Core::GetBoolSettingValue("Main", "UseSystemFont", false);
  bool update_font = (use_system_font != s_themes_locals.use_system_font);
  s_themes_locals.use_system_font = use_system_font;
  if (!s_themes_locals.system_font_set)
  {
    update_font = true;
    s_themes_locals.system_font_set = true;
    s_themes_locals.system_font = QGuiApplication::font();
  }
  if (update_font)
  {
    if (!use_system_font)
    {
      QFont application_font = s_themes_locals.system_font;
      application_font.setFamilies(GetRobotoFontFamilies());
      application_font.setPixelSize(12);
      QApplication::setFont(application_font);
    }
    else
    {
      QApplication::setFont(s_themes_locals.system_font);
    }
  }
#endif

  SetStyleFromSettings();
  UpdateThemeOnStyleChange();
}

bool QtHost::ShouldDisableStyleSheet()
{
  return Core::GetBaseBoolSettingValue("Main", "DisableStylesheet", false);
}

void QtHost::SetThemeAttributes(bool is_stylesheet_theme, bool is_variable_color_theme, bool is_dark_theme)
{
  s_themes_locals.is_stylesheet_theme = is_stylesheet_theme;
  s_themes_locals.is_variable_color_theme = is_variable_color_theme;
  s_themes_locals.is_dark_theme = is_dark_theme;

  if (is_variable_color_theme)
    qApp->styleHints()->unsetColorScheme();
  else
    qApp->styleHints()->setColorScheme(is_dark_theme ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
}

void QtHost::SetStyleSheet(const QString& stylesheet)
{
#ifdef __linux__
  // Fonts on Linux are ugly and too large. Unfortunately QApplication::setFont() doesn't seem to apply to
  // all widgets, so instead we have to jankily prefix it to all stylesheets.
  if (!s_themes_locals.use_system_font)
    qApp->setStyleSheet(QStringLiteral("QMenu, QMenuBar { font-family: \"Roboto\"; font-size: 12px; }\n") + stylesheet);
  else
    qApp->setStyleSheet(stylesheet);
#else
  qApp->setStyleSheet(stylesheet);
#endif
}

bool QtHost::LoadStyledFusionTheme(std::string_view name)
{
  Error error;
  const std::optional<std::string> data =
    QtHost::ReadResourceFileToString(TinyString::from_format(":themes/{}.qss", name), true, &error);
  if (!data.has_value())
  {
    ERROR_LOG("Failed to read theme stylesheet '{}': {}", name, error.GetDescription());
    return false;
  }

  // Parse the /*!PALETTE ... */ comment block for QPalette colors.
  QPalette palette;
  bool is_dark = true;
  const std::string_view sv(*data);
  const std::string_view::size_type palette_start = sv.find("/*!PALETTE");
  const std::string_view::size_type palette_end =
    (palette_start != std::string_view::npos) ? sv.find("*/", palette_start) : std::string_view::npos;
  if (palette_start != std::string_view::npos && palette_end != std::string_view::npos)
  {
    std::string_view palette_block = sv.substr(palette_start, palette_end - palette_start);
    while (!palette_block.empty())
    {
      std::string_view line;
      if (const std::string_view::size_type pos = palette_block.find('\n'); pos != std::string_view::npos)
      {
        line = palette_block.substr(0, pos);
        palette_block.remove_prefix(pos + 1);
      }
      else
      {
        line = palette_block;
        palette_block = {};
      }

      // Each line is: " * key: value"
      const std::string_view stripped = StringUtil::StripWhitespace(line);
      if (!stripped.starts_with("* "))
        continue;

      const std::string_view entry = stripped.substr(2); // skip "* "
      const std::string_view::size_type colon = entry.find(':');
      if (colon == std::string_view::npos)
        continue;

      const std::string_view key = StringUtil::StripWhitespace(entry.substr(0, colon));
      const std::string_view value = StringUtil::StripWhitespace(entry.substr(colon + 1));

      // Non-color metadata keys.
      if (key == "dark")
      {
        is_dark = (value == "dark");
        continue;
      }

      const QColor color(QtUtils::StringViewToQString(value));
      if (!color.isValid())
      {
        WARNING_LOG("Invalid color '{}' for palette key '{}'", value, key);
        continue;
      }

      if (key == "window")
        palette.setColor(QPalette::Window, color);
      else if (key == "window-text")
        palette.setColor(QPalette::WindowText, color);
      else if (key == "base")
        palette.setColor(QPalette::Base, color);
      else if (key == "alternate-base")
        palette.setColor(QPalette::AlternateBase, color);
      else if (key == "tooltip-base")
        palette.setColor(QPalette::ToolTipBase, color);
      else if (key == "tooltip-text")
        palette.setColor(QPalette::ToolTipText, color);
      else if (key == "text")
        palette.setColor(QPalette::Text, color);
      else if (key == "button")
        palette.setColor(QPalette::Button, color);
      else if (key == "button-text")
        palette.setColor(QPalette::ButtonText, color);
      else if (key == "link")
        palette.setColor(QPalette::Link, color);
      else if (key == "highlight")
        palette.setColor(QPalette::Highlight, color);
      else if (key == "highlight-text")
        palette.setColor(QPalette::HighlightedText, color);
      else if (key == "placeholder-text")
        palette.setColor(QPalette::PlaceholderText, color);
      else if (key == "active-button")
        palette.setColor(QPalette::Active, QPalette::Button, color);
      else if (key == "disabled-button-text")
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, color);
      else if (key == "disabled-window-text")
        palette.setColor(QPalette::Disabled, QPalette::WindowText, color);
      else if (key == "disabled-text")
        palette.setColor(QPalette::Disabled, QPalette::Text, color);
      else if (key == "disabled-light")
        palette.setColor(QPalette::Disabled, QPalette::Light, color);
      else
        WARNING_LOG("Unknown palette key '{}' in theme '{}'", key, name);
    }
  }
  else
  {
    WARNING_LOG("Theme '{}' has no PALETTE block, using default palette", name);
  }

  SetThemeAttributes(true, false, is_dark); // is_dark parsed from PALETTE block
  qApp->setStyle(QStyleFactory::create("Fusion"_L1));
  qApp->setPalette(palette);
  if (ShouldDisableStyleSheet())
    SetStyleSheet(QString());
  else
    SetStyleSheet(QString::fromStdString(data.value()));

  return true;
}

bool QtHost::IsStylesheetTheme(std::string_view theme_name)
{
  return (!theme_name.empty() && theme_name != "fusion" && theme_name != "qdarkstyle"
#ifdef _WIN32
          && theme_name != "windowsvista"
#endif
  );
}

void QtHost::SetStyleFromSettings()
{
  const TinyString theme = Core::GetBaseTinyStringSettingValue("UI", "Theme", QtHost::GetDefaultThemeName());

  // Clear any existing stylesheet before applying new. Avoids half-painted windows when changing themes.
  if (s_themes_locals.is_stylesheet_theme)
    SetStyleSheet(QString());

  if (theme == "qdarkstyle")
  {
    SetThemeAttributes(true, false, true);
    qApp->setStyle(s_themes_locals.unthemed_style_name);
    qApp->setPalette(s_themes_locals.unthemed_palette);

    QFile f(":qdarkstyle/style.qss"_L1);
    if (f.open(QFile::ReadOnly | QFile::Text))
      SetStyleSheet(f.readAll());
  }
  else if (theme == "fusion")
  {
    SetThemeAttributes(false, true, false);
    qApp->setStyle(QStyleFactory::create("Fusion"_L1));
    qApp->setPalette(s_themes_locals.unthemed_palette);
  }
#ifdef _WIN32
  else if (theme == "windowsvista")
  {
    SetThemeAttributes(false, false, false);
    qApp->setStyle(QStyleFactory::create("windowsvista"_L1));
    qApp->setPalette(s_themes_locals.unthemed_palette);
  }
#endif
  else if (theme.empty() || !LoadStyledFusionTheme(theme))
  {
    const QString stylesheet = GetNativeThemeStylesheet();
    SetThemeAttributes(!stylesheet.isEmpty(), true, false);
    qApp->setStyle(s_themes_locals.unthemed_style_name);
    qApp->setPalette(s_themes_locals.unthemed_palette);

    // Cleared above.
    if (!stylesheet.isEmpty())
      SetStyleSheet(stylesheet);
  }
}

bool QtHost::IsDarkApplicationTheme()
{
  if (!s_themes_locals.is_variable_color_theme)
    return s_themes_locals.is_dark_theme;

  const Qt::ColorScheme system_color_scheme = qApp->styleHints()->colorScheme();
  if (system_color_scheme != Qt::ColorScheme::Unknown) [[likely]]
    return (system_color_scheme == Qt::ColorScheme::Dark);

  const QPalette palette = qApp->palette();
  return (palette.windowText().color().value() > palette.window().color().value());
}

bool QtHost::HasGlobalStylesheet()
{
  return s_themes_locals.is_stylesheet_theme;
}

void QtHost::UpdateThemeOnStyleChange()
{
  const QLatin1StringView new_theme_name = IsDarkApplicationTheme() ? "white"_L1 : "black"_L1;
  if (QIcon::themeName() != new_theme_name)
    QIcon::setThemeName(new_theme_name);

  if (NativeThemeStylesheetNeedsUpdate())
  {
    const QString stylesheet = GetNativeThemeStylesheet();
    if (qApp->styleSheet() != stylesheet)
      qApp->setStyleSheet(stylesheet);
  }
}

QStringList QtHost::GetCustomThemeList()
{
  const std::string directory = Path::Combine(EmuFolders::UserResources, "themes");
  if (!FileSystem::DirectoryExists(directory.c_str()))
    return {};

  FileSystem::FindResultsArray results;
  FileSystem::FindFiles(directory.c_str(), "*.qss",
                        FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RELATIVE_PATHS |
                          FILESYSTEM_FIND_SORT_BY_NAME,
                        &results);

  QStringList ret;
  for (const FILESYSTEM_FIND_DATA& fd : results)
  {
    const std::string_view theme_name = Path::GetFileTitle(fd.FileName);
    if (!theme_name.empty())
      ret.append(QtUtils::StringViewToQString(theme_name));
  }

  return ret;
}

const char* Host::GetDefaultFullscreenUITheme()
{
  using namespace QtHost;

  static constexpr const std::pair<const char*, const char*> theme_mapping[] = {
    {"cobaltsky", "CobaltSky"}, {"greymatter", "GreyMatter"}, {"greengiant", "GreenGiant"},
    {"pinkypals", "PinkyPals"}, {"purplerain", "PurpleRain"}, {"darkocean", "DarkOcean"},
    {"darkruby", "DarkRuby"},   {"AMOLED", "AMOLED"},
  };

  const TinyString theme = Core::GetBaseTinyStringSettingValue("UI", "Theme", GetDefaultThemeName());
  const auto iter = std::ranges::find_if(theme_mapping, [theme](const auto& pair) { return (theme == pair.first); });
  if (iter != std::end(theme_mapping))
    return iter->second;

  return IsDarkApplicationTheme() ? "Dark" : "Light";
}

bool QtHost::NativeThemeStylesheetNeedsUpdate()
{
#ifdef __APPLE__
  // See below, only used on MacOS.
  // objectName() is empty after applying stylesheet.
  return (s_themes_locals.is_variable_color_theme && QApplication::style()->objectName().isEmpty());
#else
  return false;
#endif
}

QString QtHost::GetNativeThemeStylesheet()
{
  QString ret;
#ifdef __APPLE__
  // Qt's native style on MacOS is... not great.
  // We re-theme the tool buttons to look like Cocoa tool buttons, and fix up popup menus.
  ret = R"(
QMenu {
    border-radius: 10px;
    padding: 4px 0;
}
QMenu::item {
    padding: 4px 15px;
    border-radius: 8px;
    margin: 0 2px;
}
QMenu::icon,
QMenu::indicator {
    left: 8px;
}
QMenu::icon:checked {
    border-radius: 4px;
}
QMenu::separator {
    height: 1px;
    margin: 4px 8px;
}
QToolButton {
    border: none;
    background: transparent;
    padding: 5px;
    border-radius: 10px;
}
.settings-window GamePatchSettingsWidget QScrollArea,
.settings-window GamePatchSettingsWidget #patches_container {
  border: none;
}
.settings-window GamePatchSettingsWidget #patches_container > QFrame {
  border: none;
  margin: 0px 8px;
})"_L1;
  if (IsDarkApplicationTheme())
  {
    ret += R"(
QMenu {
    background-color: #161616;
    border: 1px solid #2c2c2c;
}
QMenu::item {
    color: #dcdcdc;
}
QMenu::item:selected {
    background-color: #2b4ab3;
    color: #ffffff;
}
QMenu::item:disabled {
    color: #585858;
}
QMenu::icon:checked {
    background: #414141;
    border: 1px solid #777;
}
QMenu::separator {
    background: #3b3b3b;
}
QToolButton:checked {
    background-color: #454645;
}
QToolButton:hover {
    background-color: #393c3c;
}
QToolButton:pressed {
    background-color: #808180;
}
.settings-window GamePatchSettingsWidget QScrollArea,
.settings-window GamePatchSettingsWidget #patches_container {
  background: #171717;
}
.settings-window GamePatchSettingsWidget #patches_container > QFrame {
  border-bottom: 1px solid #414141;
})"_L1;
  }
  else
  {
    ret += R"(
QMenu {
    background-color: #bdbdbd;
    border: 1px solid #d5d5d4;
}
QMenu::item {
    color: #1d1d1d;
}
QMenu::item:selected {
    background-color: #2e5dc9;
    color: #ffffff;
}
QMenu::icon:checked {
    background: #414141;
    border: 1px solid #777;
}
QMenu::item:disabled {
    color: #909090;
}
QMenu::separator {
    background: #a9a9a9;
}
QToolButton:checked {
    background-color: #e2e2e2;
}
QToolButton:hover {
    background-color: #f0f0f0;
}
QToolButton:pressed {
    background-color: #8c8c8c;
}
.settings-window GamePatchSettingsWidget QScrollArea,
.settings-window GamePatchSettingsWidget #patches_container {
  background: #ffffff;
}
.settings-window GamePatchSettingsWidget #patches_container > QFrame {
  border-bottom: 1px solid #414141;
})"_L1;
  }
#endif
  return ret;
}
