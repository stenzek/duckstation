// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "qthost.h"

#include "util/imgui_fullscreen.h"

#include "common/path.h"

#include <QtCore/QFile>
#include <QtGui/QPalette>
#include <QtGui/QStyleHints>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleFactory>

namespace QtHost {
static void SetThemeAttributes(bool is_stylesheet_theme, bool is_variable_color_theme, bool is_dark_theme);
static void SetStyleFromSettings();
static QString GetNativeThemeStylesheet();
static bool NativeThemeStylesheetNeedsUpdate();

namespace {
struct State
{
  QString unthemed_style_name;
  QPalette unthemed_palette;
  bool is_stylesheet_theme = false;
  bool is_variable_color_theme = false;
  bool is_dark_theme = false;
  bool unthemed_style_name_set = false;
};
} // namespace

static State s_state;

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
  if (!s_state.unthemed_style_name_set)
  {
    s_state.unthemed_style_name_set = true;
    s_state.unthemed_style_name = QApplication::style()->objectName();
    s_state.unthemed_palette = QApplication::palette();
  }

  SetStyleFromSettings();
  UpdateThemeOnStyleChange();
}

void QtHost::SetThemeAttributes(bool is_stylesheet_theme, bool is_variable_color_theme, bool is_dark_theme)
{
  s_state.is_stylesheet_theme = is_stylesheet_theme;
  s_state.is_variable_color_theme = is_variable_color_theme;
  s_state.is_dark_theme = is_dark_theme;

  if (is_variable_color_theme)
    qApp->styleHints()->unsetColorScheme();
  else
    qApp->styleHints()->setColorScheme(is_dark_theme ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
}

void QtHost::SetStyleFromSettings()
{
  const TinyString theme = Host::GetBaseTinyStringSettingValue("UI", "Theme", QtHost::GetDefaultThemeName());

  if (theme == "qdarkstyle")
  {
    SetThemeAttributes(true, false, true);
    qApp->setStyle(s_state.unthemed_style_name);
    qApp->setPalette(s_state.unthemed_palette);
    qApp->setStyleSheet(QString());

    QFile f(QStringLiteral(":qdarkstyle/style.qss"));
    if (f.open(QFile::ReadOnly | QFile::Text))
      qApp->setStyleSheet(f.readAll());
  }
  else if (theme == "fusion")
  {
    SetThemeAttributes(false, true, false);
    qApp->setStyle(QStyleFactory::create("Fusion"));
    qApp->setPalette(s_state.unthemed_palette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "darkfusion")
  {
    // adapted from https://gist.github.com/QuantumCD/6245215
    SetThemeAttributes(false, false, true);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor lighterGray(75, 75, 75);
    static constexpr QColor darkGray(53, 53, 53);
    static constexpr QColor gray(128, 128, 128);
    static constexpr QColor black(25, 25, 25);
    static constexpr QColor blue(198, 238, 255);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, darkGray);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, black);
    darkPalette.setColor(QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, blue);
    darkPalette.setColor(QPalette::Highlight, lighterGray);
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);
    darkPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

    darkPalette.setColor(QPalette::Active, QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkGray);

    qApp->setPalette(darkPalette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "darkfusionblue")
  {
    // adapted from https://gist.github.com/QuantumCD/6245215
    SetThemeAttributes(false, false, true);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    // static constexpr QColor lighterGray(75, 75, 75);
    static constexpr QColor darkGray(53, 53, 53);
    static constexpr QColor gray(128, 128, 128);
    static constexpr QColor black(25, 25, 25);
    static constexpr QColor blue(198, 238, 255);
    static constexpr QColor blue2(0, 88, 208);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, darkGray);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, black);
    darkPalette.setColor(QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipBase, blue2);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, darkGray);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, blue);
    darkPalette.setColor(QPalette::Highlight, blue2);
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);
    darkPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

    darkPalette.setColor(QPalette::Active, QPalette::Button, gray.darker());
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkGray);

    qApp->setPalette(darkPalette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "darkerfusion")
  {
    SetThemeAttributes(true, false, true);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor window_color(36, 36, 36);
    static constexpr QColor base_color(43, 43, 43);
    static constexpr QColor button_color(40, 40, 40); // qt makes this lighter
    static constexpr QColor text(255, 255, 255);
    static constexpr QColor highlight_background(65, 65, 65);
    static constexpr QColor highlight_text(255, 255, 255);
    static constexpr QColor disabled_text(200, 200, 200);
    static constexpr QColor placeholder_text(200, 200, 200);
    static constexpr QColor link_text(198, 238, 255);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, window_color);
    darkPalette.setColor(QPalette::WindowText, text);
    darkPalette.setColor(QPalette::Base, base_color);
    darkPalette.setColor(QPalette::AlternateBase, window_color);
    darkPalette.setColor(QPalette::ToolTipBase, window_color);
    darkPalette.setColor(QPalette::ToolTipText, text);
    darkPalette.setColor(QPalette::Text, text);
    darkPalette.setColor(QPalette::Button, button_color);
    darkPalette.setColor(QPalette::ButtonText, text);
    darkPalette.setColor(QPalette::Link, link_text);
    darkPalette.setColor(QPalette::Highlight, highlight_background);
    darkPalette.setColor(QPalette::HighlightedText, highlight_text);
    darkPalette.setColor(QPalette::PlaceholderText, placeholder_text);

    darkPalette.setColor(QPalette::Active, QPalette::Button, button_color);
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled_text);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, disabled_text);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, disabled_text);
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, window_color);

    qApp->setPalette(darkPalette);
    qApp->setStyleSheet(QString());

    // menus are by far the ugliest part of fusion, so we style them manually
    const QString stylesheet = QStringLiteral(R"(
QMenu {
    border: 1px solid #444;
    border-radius: 8px;
    padding: 6px 10px;
    background-color: #232323;
}
QMenu::icon,
QMenu::indicator {
    left: 8px;
}
QMenu::item {
    padding: 6px 18px;
    border-radius: 8px;
}
QMenu::item:selected {
    background-color: #414141;
}
QMenu::icon:checked {
    background: #414141;
    border: 1px solid #777;
    border-radius: 4px;
}
QMenuBar::item {
    padding: 4px 6px;
    border-radius: 6px;
}
QMenuBar::item:selected, QMenuBar::item:pressed {
    background: #414141;
    border-radius: 4px;
}
QToolTip {
    color: #ffffff;
    background-color: #232323;
    border: 1px solid #444;
    border-radius: 6px;
    padding: 2px;
}
QToolBar {
    border: none;
}
    )");

    qApp->setStyleSheet(stylesheet);
  }
  else if (theme == "cobaltsky")
  {
    // Custom palette by KamFretoZ, A soothing deep royal blue
    // that are meant to be easy on the eyes as the main color.
    // Alternative dark theme.
    SetThemeAttributes(false, false, true);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor gray(150, 150, 150);
    static constexpr QColor royalBlue(29, 41, 81);
    static constexpr QColor darkishBlue(17, 30, 108);
    static constexpr QColor lighterBlue(25, 32, 130);
    static constexpr QColor highlight(36, 93, 218);
    static constexpr QColor link(0, 202, 255);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, royalBlue);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, royalBlue.lighter());
    darkPalette.setColor(QPalette::AlternateBase, darkishBlue);
    darkPalette.setColor(QPalette::ToolTipBase, darkishBlue);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, lighterBlue);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, link);
    darkPalette.setColor(QPalette::Highlight, highlight);
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);

    darkPalette.setColor(QPalette::Active, QPalette::Button, lighterBlue);
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, gray);

    qApp->setPalette(darkPalette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "greymatter")
  {
    SetThemeAttributes(false, false, true);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor darkGray(46, 52, 64);
    static constexpr QColor lighterGray(59, 66, 82);
    static constexpr QColor gray(111, 111, 111);
    static constexpr QColor blue(198, 238, 255);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, darkGray);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, lighterGray);
    darkPalette.setColor(QPalette::AlternateBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipBase, darkGray);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, lighterGray);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, blue);
    darkPalette.setColor(QPalette::Highlight, lighterGray.lighter());
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);
    darkPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

    darkPalette.setColor(QPalette::Active, QPalette::Button, lighterGray);
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray.lighter());
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray.lighter());
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray.lighter());
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkGray);

    qApp->setPalette(darkPalette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "greengiant")
  {
    // Custom palette by RedDevilus, Tame (Light/Washed out) Green as main color and Grayish Blue as complimentary.
    // Alternative white theme.
    SetThemeAttributes(false, false, false);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor black(25, 25, 25);
    static constexpr QColor gray(111, 111, 111);
    static constexpr QColor limerick(176, 196, 0);
    static constexpr QColor brown(135, 100, 50);
    static constexpr QColor pear(213, 222, 46);

    QPalette greenGiantPalette;
    greenGiantPalette.setColor(QPalette::Window, pear);
    greenGiantPalette.setColor(QPalette::WindowText, black);
    greenGiantPalette.setColor(QPalette::Base, limerick);
    greenGiantPalette.setColor(QPalette::AlternateBase, brown.lighter());
    greenGiantPalette.setColor(QPalette::ToolTipBase, brown);
    greenGiantPalette.setColor(QPalette::ToolTipText, Qt::white);
    greenGiantPalette.setColor(QPalette::Text, black);
    greenGiantPalette.setColor(QPalette::Button, brown.lighter());
    greenGiantPalette.setColor(QPalette::ButtonText, black.lighter());
    greenGiantPalette.setColor(QPalette::Link, brown.lighter());
    greenGiantPalette.setColor(QPalette::Highlight, brown);
    greenGiantPalette.setColor(QPalette::HighlightedText, Qt::white);

    greenGiantPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
    greenGiantPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray.darker());
    greenGiantPalette.setColor(QPalette::Disabled, QPalette::Text, gray.darker());
    greenGiantPalette.setColor(QPalette::Disabled, QPalette::Light, gray);

    qApp->setPalette(greenGiantPalette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "pinkypals")
  {
    SetThemeAttributes(false, false, false);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor black(25, 25, 25);
    static constexpr QColor pink(255, 174, 201);
    static constexpr QColor darkerPink(214, 145, 168);
    static constexpr QColor brightPink(224, 88, 133);
    static constexpr QColor congoPink(255, 127, 121);

    QPalette PinkyPalsPalette;
    PinkyPalsPalette.setColor(QPalette::Window, pink);
    PinkyPalsPalette.setColor(QPalette::WindowText, black);
    PinkyPalsPalette.setColor(QPalette::Base, darkerPink);
    PinkyPalsPalette.setColor(QPalette::AlternateBase, brightPink);
    PinkyPalsPalette.setColor(QPalette::ToolTipBase, pink);
    PinkyPalsPalette.setColor(QPalette::ToolTipText, darkerPink);
    PinkyPalsPalette.setColor(QPalette::Text, black);
    PinkyPalsPalette.setColor(QPalette::Button, pink);
    PinkyPalsPalette.setColor(QPalette::ButtonText, black);
    PinkyPalsPalette.setColor(QPalette::Link, black);
    PinkyPalsPalette.setColor(QPalette::Highlight, congoPink);
    PinkyPalsPalette.setColor(QPalette::HighlightedText, black);

    PinkyPalsPalette.setColor(QPalette::Active, QPalette::Button, pink);
    PinkyPalsPalette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(Qt::white).darker());
    PinkyPalsPalette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(Qt::white).darker());
    PinkyPalsPalette.setColor(QPalette::Disabled, QPalette::Text, QColor(Qt::white).darker());
    PinkyPalsPalette.setColor(QPalette::Disabled, QPalette::Light, QColor(Qt::white).darker());

    qApp->setPalette(PinkyPalsPalette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "AMOLED")
  {
    // Custom palette by KamFretoZ, A pure concentrated darkness
    // of a theme designed for maximum eye comfort and benefits
    // OLED screens.
    SetThemeAttributes(false, false, true);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor black(0, 0, 0);
    static constexpr QColor gray(25, 25, 25);
    static constexpr QColor lighterGray(75, 75, 75);
    static constexpr QColor blue(198, 238, 255);

    QPalette AMOLEDPalette;
    AMOLEDPalette.setColor(QPalette::Window, black);
    AMOLEDPalette.setColor(QPalette::WindowText, Qt::white);
    AMOLEDPalette.setColor(QPalette::Base, gray);
    AMOLEDPalette.setColor(QPalette::AlternateBase, black);
    AMOLEDPalette.setColor(QPalette::ToolTipBase, gray);
    AMOLEDPalette.setColor(QPalette::ToolTipText, Qt::white);
    AMOLEDPalette.setColor(QPalette::Text, Qt::white);
    AMOLEDPalette.setColor(QPalette::Button, gray);
    AMOLEDPalette.setColor(QPalette::ButtonText, Qt::white);
    AMOLEDPalette.setColor(QPalette::Link, blue);
    AMOLEDPalette.setColor(QPalette::Highlight, lighterGray);
    AMOLEDPalette.setColor(QPalette::HighlightedText, Qt::white);
    AMOLEDPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

    AMOLEDPalette.setColor(QPalette::Active, QPalette::Button, gray);
    AMOLEDPalette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(Qt::white).darker());
    AMOLEDPalette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(Qt::white).darker());
    AMOLEDPalette.setColor(QPalette::Disabled, QPalette::Text, QColor(Qt::white).darker());
    AMOLEDPalette.setColor(QPalette::Disabled, QPalette::Light, QColor(Qt::white).darker());

    qApp->setPalette(AMOLEDPalette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "darkruby")
  {
    SetThemeAttributes(false, false, true);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor gray(128, 128, 128);
    static constexpr QColor slate(18, 18, 18);
    static constexpr QColor rubyish(172, 21, 31);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, slate);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, slate.lighter());
    darkPalette.setColor(QPalette::AlternateBase, slate.lighter());
    darkPalette.setColor(QPalette::ToolTipBase, slate);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, slate);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, Qt::white);
    darkPalette.setColor(QPalette::Highlight, rubyish);
    darkPalette.setColor(QPalette::HighlightedText, Qt::white);

    darkPalette.setColor(QPalette::Active, QPalette::Button, slate);
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, gray);
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, slate.lighter());

    qApp->setPalette(darkPalette);
    qApp->setStyleSheet(QString());
  }
  else if (theme == "purplerain")
  {
    SetThemeAttributes(false, false, true);
    qApp->setStyle(QStyleFactory::create("Fusion"));

    static constexpr QColor darkPurple(73, 41, 121);
    static constexpr QColor darkerPurple(53, 29, 87);
    static constexpr QColor gold(250, 207, 0);

    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, darkPurple);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, darkerPurple);
    darkPalette.setColor(QPalette::AlternateBase, darkPurple);
    darkPalette.setColor(QPalette::ToolTipBase, darkPurple);
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, darkerPurple);
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::Link, gold);
    darkPalette.setColor(QPalette::Highlight, gold);
    darkPalette.setColor(QPalette::HighlightedText, Qt::black);
    darkPalette.setColor(QPalette::PlaceholderText, QColor(Qt::white).darker());

    darkPalette.setColor(QPalette::Active, QPalette::Button, darkerPurple);
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, darkPurple.lighter());
    darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, darkPurple.lighter());
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, darkPurple.lighter());
    darkPalette.setColor(QPalette::Disabled, QPalette::Light, darkPurple);

    qApp->setPalette(darkPalette);
    qApp->setStyleSheet(QString());
  }
#ifdef _WIN32
  else if (theme == "windowsvista")
  {
    SetThemeAttributes(false, false, false);
    qApp->setStyle(QStyleFactory::create("windowsvista"));
    qApp->setPalette(s_state.unthemed_palette);
    qApp->setStyleSheet(QString());
  }
#endif
  else
  {
    SetThemeAttributes(false, true, false);
    qApp->setStyle(s_state.unthemed_style_name);
    qApp->setPalette(s_state.unthemed_palette);
    qApp->setStyleSheet(GetNativeThemeStylesheet());
  }
}

bool QtHost::IsDarkApplicationTheme()
{
  if (!s_state.is_variable_color_theme)
    return s_state.is_dark_theme;

  const Qt::ColorScheme system_color_scheme = qApp->styleHints()->colorScheme();
  if (system_color_scheme != Qt::ColorScheme::Unknown) [[likely]]
    return (system_color_scheme == Qt::ColorScheme::Dark);

  const QPalette palette = qApp->palette();
  return (palette.windowText().color().value() > palette.window().color().value());
}

bool QtHost::IsStyleSheetApplicationTheme()
{
  return s_state.is_stylesheet_theme;
}

void QtHost::UpdateThemeOnStyleChange()
{
  const QString new_theme_name = IsDarkApplicationTheme() ? QStringLiteral("white") : QStringLiteral("black");
  if (QIcon::themeName() != new_theme_name)
    QIcon::setThemeName(new_theme_name);

  if (NativeThemeStylesheetNeedsUpdate())
  {
    const QString stylesheet = GetNativeThemeStylesheet();
    if (qApp->styleSheet() != stylesheet)
      qApp->setStyleSheet(stylesheet);
  }
}

const char* Host::GetDefaultFullscreenUITheme()
{
  using namespace QtHost;

  const TinyString theme = Host::GetBaseTinyStringSettingValue("UI", "Theme", QtHost::GetDefaultThemeName());

  if (theme == "cobaltsky")
    return "CobaltSky";
  else if (theme == "greymatter")
    return "GreyMatter";
  else if (theme == "greengiant")
    return "GreenGiant";
  else if (theme == "pinkypals")
    return "PinkyPals";
  else if (theme == "purplerain")
    return "PurpleRain";
  else if (theme == "darkruby")
    return "DarkRuby";
  else if (theme == "AMOLED")
    return "AMOLED";
  else
    return IsDarkApplicationTheme() ? "Dark" : "Light";
}

bool QtHost::NativeThemeStylesheetNeedsUpdate()
{
#ifdef __APPLE__
  // See below, only used on MacOS.
  return s_state.is_variable_color_theme;
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
  ret = QStringLiteral(R"(
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
})");
  if (IsDarkApplicationTheme())
  {
    ret += QStringLiteral(R"(
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
})");
  }
  else
  {
    ret += QStringLiteral(R"(
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
})");
  }
#endif
  return ret;
}
