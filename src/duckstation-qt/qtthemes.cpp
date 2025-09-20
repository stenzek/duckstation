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
static void SetStyleFromSettings();

namespace {
struct State
{
  std::string current_theme_name;
  QString unthemed_style_name;
  QPalette unthemed_palette;
  bool unthemed_style_name_set = false;
};
} // namespace

static State s_state;

} // namespace QtHost

const char* QtHost::GetDefaultThemeName()
{
  return "darkerfusion";
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
  SetIconThemeFromStyle();
}

void QtHost::SetStyleFromSettings()
{
  const TinyString theme = Host::GetBaseTinyStringSettingValue("UI", "Theme", QtHost::GetDefaultThemeName());

  if (theme == "qdarkstyle")
  {
    qApp->setStyle(s_state.unthemed_style_name);
    qApp->setPalette(s_state.unthemed_palette);
    qApp->setStyleSheet(QString());
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);

    QFile f(QStringLiteral(":qdarkstyle/style.qss"));
    if (f.open(QFile::ReadOnly | QFile::Text))
      qApp->setStyleSheet(f.readAll());
  }
  else if (theme == "fusion")
  {
    qApp->setStyle(QStyleFactory::create("Fusion"));
    qApp->setPalette(s_state.unthemed_palette);
    qApp->setStyleSheet(QString());
    qApp->styleHints()->unsetColorScheme();
  }
  else if (theme == "darkfusion")
  {
    // adapted from https://gist.github.com/QuantumCD/6245215
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  }
  else if (theme == "darkfusionblue")
  {
    // adapted from https://gist.github.com/QuantumCD/6245215
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  }
  else if (theme == "darkerfusion")
  {
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);

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
    )");

    qApp->setStyleSheet(stylesheet);

  }
  else if (theme == "cobaltsky")
  {
    // Custom palette by KamFretoZ, A soothing deep royal blue
    // that are meant to be easy on the eyes as the main color.
    // Alternative dark theme.
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  }
  else if (theme == "greymatter")
  {
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  }
  else if (theme == "greengiant")
  {
    // Custom palette by RedDevilus, Tame (Light/Washed out) Green as main color and Grayish Blue as complimentary.
    // Alternative white theme.
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Light);
  }
  else if (theme == "pinkypals")
  {
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Light);
  }
  else if (theme == "AMOLED")
  {
    // Custom palette by KamFretoZ, A pure concentrated darkness
    // of a theme designed for maximum eye comfort and benefits
    // OLED screens.
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  }
  else if (theme == "darkruby")
  {
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
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  }
  else if (theme == "purplerain")
  {
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
    qApp->setStyleSheet("QToolTip { color: #ffffff; background-color: #505a70; border: 1px solid white; }");
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
  }
#ifdef _WIN32
  else if (theme == "windowsvista")
  {
    qApp->setStyle(QStyleFactory::create("windowsvista"));
    qApp->setPalette(s_state.unthemed_palette);
    qApp->setStyleSheet(QString());
    qApp->styleHints()->setColorScheme(Qt::ColorScheme::Light);
  }
#endif
  else
  {
    qApp->setStyle(s_state.unthemed_style_name);
    qApp->setPalette(s_state.unthemed_palette);
    qApp->setStyleSheet(QString());
    qApp->styleHints()->unsetColorScheme();
  }
}

bool QtHost::IsDarkApplicationTheme()
{
  return (qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark);
}

void QtHost::SetIconThemeFromStyle()
{
  QIcon::setThemeName(IsDarkApplicationTheme() ? QStringLiteral("white") : QStringLiteral("black"));
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
