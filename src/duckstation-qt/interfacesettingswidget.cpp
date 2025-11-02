// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "interfacesettingswidget.h"
#include "autoupdaterwindow.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "scmversion/scmversion.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "moc_interfacesettingswidget.cpp"

const char* InterfaceSettingsWidget::THEME_NAMES[] = {
  QT_TRANSLATE_NOOP("MainWindow", "Native"),
#ifdef _WIN32
  QT_TRANSLATE_NOOP("MainWindow", "Classic Windows"),
#endif
  QT_TRANSLATE_NOOP("MainWindow", "Fusion"),
  QT_TRANSLATE_NOOP("MainWindow", "Dark Fusion (Gray)"),
  QT_TRANSLATE_NOOP("MainWindow", "Dark Fusion (Blue)"),
  QT_TRANSLATE_NOOP("MainWindow", "Darker Fusion"),
  QT_TRANSLATE_NOOP("MainWindow", "AMOLED"),
  QT_TRANSLATE_NOOP("MainWindow", "Cobalt Sky"),
  QT_TRANSLATE_NOOP("MainWindow", "Grey Matter"),
  QT_TRANSLATE_NOOP("MainWindow", "Green Giant"),
  QT_TRANSLATE_NOOP("MainWindow", "Pinky Pals"),
  QT_TRANSLATE_NOOP("MainWindow", "Dark Ruby"),
  QT_TRANSLATE_NOOP("MainWindow", "Purple Rain"),
  QT_TRANSLATE_NOOP("MainWindow", "QDarkStyle"),
  nullptr,
};

const char* InterfaceSettingsWidget::THEME_VALUES[] = {
  "",
#ifdef _WIN32
  "windowsvista",
#endif
  "fusion",
  "darkfusion",
  "darkfusionblue",
  "darkerfusion",
  "AMOLED",
  "cobaltsky",
  "greymatter",
  "greengiant",
  "pinkypals",
  "darkruby",
  "purplerain",
  "qdarkstyle",
  nullptr,
};

InterfaceSettingsWidget::InterfaceSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.inhibitScreensaver, "Main", "InhibitScreensaver", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pauseOnFocusLoss, "Main", "PauseOnFocusLoss", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pauseOnControllerDisconnection, "Main",
                                               "PauseOnControllerDisconnection", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pauseOnStart, "Main", "StartPaused", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.saveStateOnGameClose, "Main", "SaveStateOnExit", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.confirmGameClose, "Main", "ConfirmPowerOff", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.startFullscreen, "Main", "StartFullscreen", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.doubleClickTogglesFullscreen, "Main",
                                               "DoubleClickTogglesFullscreen", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.renderToSeparateWindow, "Main", "RenderToSeparateWindow",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.hideMainWindow, "Main", "HideMainWindowWhenRunning", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableWindowResizing, "Main", "DisableWindowResize", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.hideMouseCursor, "Main", "HideCursorInFullscreen", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.createSaveStateBackups, "Main", "CreateSaveStateBackups",
                                               Settings::DEFAULT_SAVE_STATE_BACKUPS);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableDiscordPresence, "Main", "EnableDiscordPresence", false);
  connect(m_ui.renderToSeparateWindow, &QCheckBox::checkStateChanged, this,
          &InterfaceSettingsWidget::onRenderToSeparateWindowChanged);

  if (!m_dialog->isPerGameSettings())
  {
    SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.theme, "UI", "Theme", THEME_NAMES, THEME_VALUES,
                                                 QtHost::GetDefaultThemeName(), "MainWindow");
    connect(m_ui.theme, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() { emit themeChanged(); });

    populateLanguageDropdown(m_ui.language);
    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.language, "Main", "Language", {});
    connect(m_ui.language, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &InterfaceSettingsWidget::onLanguageChanged);

    m_ui.autoUpdateCurrentVersion->setText(tr("%1 (%2)").arg(g_scm_version_str).arg(g_scm_date_str));
  }

  onRenderToSeparateWindowChanged();

  dialog->registerWidgetHelp(m_ui.confirmGameClose, tr("Confirm Game Close"), tr("Checked"),
                             tr("Determines whether a prompt will be displayed to confirm closing the game."));
  dialog->registerWidgetHelp(m_ui.saveStateOnGameClose, tr("Save State On Game Close"), tr("Checked"),
                             tr("Automatically saves the system state when closing the game or exiting. You can then "
                                "resume directly from where you left off next time."));
  dialog->registerWidgetHelp(
    m_ui.inhibitScreensaver, tr("Inhibit Screensaver"), tr("Checked"),
    tr("Prevents the screen saver from activating and the host from sleeping while emulation is running."));
  dialog->registerWidgetHelp(m_ui.pauseOnStart, tr("Pause On Start"), tr("Unchecked"),
                             tr("Pauses the emulator when a game is started."));
  dialog->registerWidgetHelp(m_ui.pauseOnFocusLoss, tr("Pause On Focus Loss"), tr("Unchecked"),
                             tr("Pauses the emulator when you minimize the window or switch to another application, "
                                "and unpauses when you switch back."));
  dialog->registerWidgetHelp(m_ui.pauseOnControllerDisconnection, tr("Pause On Controller Disconnection"),
                             tr("Unchecked"),
                             tr("Pauses the emulator when a controller with bindings is disconnected."));
  dialog->registerWidgetHelp(
    m_ui.createSaveStateBackups, tr("Create Save State Backups"), tr("Checked"),
    tr("Backs up any previous save state when creating a new save state, with a .bak extension."));
  dialog->registerWidgetHelp(m_ui.enableDiscordPresence, tr("Enable Discord Presence"), tr("Unchecked"),
                             tr("Shows the game you are currently playing as part of your profile in Discord."));

  dialog->registerWidgetHelp(m_ui.startFullscreen, tr("Start Fullscreen"), tr("Unchecked"),
                             tr("Automatically switches to fullscreen mode when a game is started."));
  dialog->registerWidgetHelp(m_ui.doubleClickTogglesFullscreen, tr("Double-Click Toggles Fullscreen"), tr("Checked"),
                             tr("Switches between full screen and windowed when the window is double-clicked."));
  dialog->registerWidgetHelp(
    m_ui.renderToSeparateWindow, tr("Render To Separate Window"), tr("Checked"),
    tr("Renders the display of the simulated console to the main window of the application, over "
       "the game list. If checked, the display will render in a separate window."));
  dialog->registerWidgetHelp(
    m_ui.hideMainWindow, tr("Hide Main Window When Running"), tr("Unchecked"),
    tr("Hides the main window of the application while the game is displayed in a separate window."));
  dialog->registerWidgetHelp(m_ui.disableWindowResizing, tr("Disable Window Resizing"), tr("Unchecked"),
                             tr("Prevents resizing of the window while a game is running."));
  dialog->registerWidgetHelp(m_ui.hideMouseCursor, tr("Hide Cursor In Fullscreen"), tr("Checked"),
                             tr("Hides the mouse pointer/cursor when the emulator is in fullscreen mode."));

  if (!m_dialog->isPerGameSettings())
  {
    if (AutoUpdaterWindow::isSupported())
    {
      SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.autoUpdateEnabled, "AutoUpdater", "CheckAtStartup", true);
      m_ui.autoUpdateTag->addItems(AutoUpdaterWindow::getTagList());
      SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.autoUpdateTag, "AutoUpdater", "UpdateTag",
                                                     AutoUpdaterWindow::getDefaultTag());
      connect(m_ui.checkForUpdates, &QPushButton::clicked, this, []() { g_main_window->checkForUpdates(true); });
    }
    else
    {
      m_ui.autoUpdateTag->addItem(tr("Unavailable"));
      m_ui.autoUpdateEnabled->setEnabled(false);
      m_ui.autoUpdateTag->setEnabled(false);
      m_ui.checkForUpdates->setEnabled(false);
      m_ui.updatesGroup->setEnabled(false);
    }

    dialog->registerWidgetHelp(m_ui.language, tr("Language"), tr("System Language"),
                               tr("Selects the language for the application. Please note that not all parts of the "
                                  "application may be translated for a given language."));

    const char* default_theme_cname = QtHost::GetDefaultThemeName();
    QString default_theme_name;
    for (size_t i = 0; THEME_VALUES[i] != nullptr; ++i)
    {
      if (std::strcmp(THEME_VALUES[i], default_theme_cname) == 0)
      {
        default_theme_name = tr(THEME_NAMES[i]);
        break;
      }
    }
    dialog->registerWidgetHelp(m_ui.theme, tr("Theme"), default_theme_name,
                               tr("Selects the theme for the application."));

    dialog->registerWidgetHelp(m_ui.autoUpdateTag, tr("Update Channel"),
                               QString::fromStdString(AutoUpdaterWindow::getDefaultTag()),
                               tr("Selects the channel that will be checked for updates to the application. The "
                                  "<strong>preview</strong> channel contains the latest changes, and may be unstable. "
                                  "The <strong>latest</strong> channel tracks the latest release."));
    dialog->registerWidgetHelp(m_ui.autoUpdateEnabled, tr("Enable Automatic Update Check"), tr("Checked"),
                               tr("Automatically checks for updates to the program on startup. Updates can be deferred "
                                  "until later or skipped entirely."));
  }
  else
  {
    delete m_ui.appearanceGroup;
    m_ui.appearanceGroup = nullptr;
    m_ui.languageLabel = nullptr;
    m_ui.language = nullptr;
    m_ui.themeLabel = nullptr;
    m_ui.theme = nullptr;

    delete m_ui.updatesGroup;
    m_ui.autoUpdateTagLabel = nullptr;
    m_ui.autoUpdateTag = nullptr;
    m_ui.autoUpdateCurrentVersionLabel = nullptr;
    m_ui.autoUpdateCurrentVersion = nullptr;
    m_ui.autoUpdateCheckLayout = nullptr;
    m_ui.autoUpdateEnabled = nullptr;
    m_ui.checkForUpdates = nullptr;
  }
}

InterfaceSettingsWidget::~InterfaceSettingsWidget() = default;

void InterfaceSettingsWidget::populateLanguageDropdown(QComboBox* cb)
{
  for (const auto& [language, code] : Host::GetAvailableLanguageList())
  {
    cb->addItem(QtUtils::GetIconForTranslationLanguage(code), QString::fromUtf8(Host::GetLanguageName(code)),
                QString::fromLatin1(code));
  }
}

void InterfaceSettingsWidget::onRenderToSeparateWindowChanged()
{
  m_ui.hideMainWindow->setEnabled(m_ui.renderToSeparateWindow->isChecked());
}

void InterfaceSettingsWidget::onLanguageChanged()
{
  QtHost::UpdateApplicationLanguage(QtUtils::GetRootWidget(this));
  g_main_window->recreate();
}
