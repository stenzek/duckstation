// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "interfacesettingswidget.h"
#include "autoupdaterdialog.h"
#include "mainwindow.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "scmversion/scmversion.h"

#include "common/error.h"
#include "common/string_util.h"

#include <QtWidgets/QProgressDialog>
#include <ranges>

#include "moc_interfacesettingswidget.cpp"

static constexpr const std::pair<const char*, const char*> BUILTIN_THEMES[] = {
  {"", QT_TRANSLATE_NOOP("MainWindow", "Native")},
#ifdef _WIN32
  {"windowsvista", QT_TRANSLATE_NOOP("MainWindow", "Classic Windows")},
#endif
  {"fusion", QT_TRANSLATE_NOOP("MainWindow", "Fusion")},
  {"darkfusion", QT_TRANSLATE_NOOP("MainWindow", "Dark Fusion (Gray)")},
  {"darkfusionblue", QT_TRANSLATE_NOOP("MainWindow", "Dark Fusion (Blue)")},
  {"darkerfusion", QT_TRANSLATE_NOOP("MainWindow", "Darker Fusion")},
  {"AMOLED", QT_TRANSLATE_NOOP("MainWindow", "AMOLED")},
  {"cobaltsky", QT_TRANSLATE_NOOP("MainWindow", "Cobalt Sky")},
  {"greymatter", QT_TRANSLATE_NOOP("MainWindow", "Grey Matter")},
  {"greengiant", QT_TRANSLATE_NOOP("MainWindow", "Green Giant")},
  {"pinkypals", QT_TRANSLATE_NOOP("MainWindow", "Pinky Pals")},
  {"darkocean", QT_TRANSLATE_NOOP("MainWindow", "Dark Ocean")},
  {"darkruby", QT_TRANSLATE_NOOP("MainWindow", "Dark Ruby")},
  {"purplerain", QT_TRANSLATE_NOOP("MainWindow", "Purple Rain")},
  {"qdarkstyle", QT_TRANSLATE_NOOP("MainWindow", "QDarkStyle")},
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
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.disableBackgroundInput, "Main", "DisableBackgroundInput",
                                               false);
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
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.displayLogInMainWindow, "Main", "DisplayLogInMainWindow",
                                               false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.createSaveStateBackups, "Main", "CreateSaveStateBackups",
                                               Settings::DEFAULT_SAVE_STATE_BACKUPS);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableDiscordPresence, "Main", "EnableDiscordPresence", false);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.automaticallyResizeWindow, "Display", "AutoResizeWindow",
                                               false);
  connect(m_ui.renderToSeparateWindow, &QCheckBox::checkStateChanged, this,
          &InterfaceSettingsWidget::updateRenderToSeparateWindowOptions);
  connect(m_ui.hideMainWindow, &QCheckBox::checkStateChanged, this,
          &InterfaceSettingsWidget::updateRenderToSeparateWindowOptions);

  int next_appearance_row = m_ui.appearanceLayout->rowCount();
  int next_appearance_col = 0;

#if defined(_WIN32)
  QCheckBox* const disable_window_rounded_corners =
    new QCheckBox(tr("Disable Window Rounded Corners"), m_ui.appearanceGroup);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, disable_window_rounded_corners, "Main",
                                               "DisableWindowRoundedCorners", false);
  m_ui.appearanceLayout->addWidget(disable_window_rounded_corners, next_appearance_row, next_appearance_col++ * 2, 1,
                                   2);
#elif defined(__APPLE__)
  QCheckBox* const use_fractional_window_scale = new QCheckBox(tr("Use Fractional Window Scale"), m_ui.appearanceGroup);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, use_fractional_window_scale, "Main", "UseFractionalWindowScale",
                                               false);
  m_ui.appearanceLayout->addWidget(use_fractional_window_scale, next_appearance_row, next_appearance_col++ * 2, 1, 2);
#endif

  if (!m_dialog->isPerGameSettings())
  {
    setupThemeCombo(m_ui.theme);
    setupLanguageCombo(m_ui.language);
    connect(m_ui.language, &QComboBox::currentIndexChanged, this, &InterfaceSettingsWidget::onLanguageChanged);

    // Annoyingly, have to match theme and language properties otherwise the sizes do not match.
    m_ui.theme->setMinimumContentsLength(m_ui.language->minimumContentsLength());
    m_ui.theme->setSizeAdjustPolicy(m_ui.language->sizeAdjustPolicy());

#ifdef __linux__
    QCheckBox* const use_system_font = new QCheckBox(tr("Use System Font"), m_ui.appearanceGroup);
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, use_system_font, "Main", "UseSystemFont", false);
    m_ui.appearanceLayout->addWidget(use_system_font, next_appearance_row, next_appearance_col++ * 2, 1, 2);
    connect(use_system_font, &QCheckBox::checkStateChanged, this, &QtHost::UpdateApplicationTheme,
            Qt::QueuedConnection);
    dialog->registerWidgetHelp(
      use_system_font, tr("Use System Font"), tr("Unchecked"),
      tr("Uses the system font for the interface, instead of the bundled Roboto font. Enabling "
         "this option may cause some UI elements to not fit within windows."));
#endif

    m_disable_style_sheets = new QCheckBox(tr("Disable Style Sheets"), m_ui.appearanceGroup);
    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_disable_style_sheets, "Main", "DisableStylesheet", false);
    connect(m_disable_style_sheets, &QCheckBox::checkStateChanged, this, &QtHost::UpdateApplicationTheme);
    m_ui.appearanceLayout->addWidget(m_disable_style_sheets, next_appearance_row, next_appearance_col++ * 2, 1, 2);
    dialog->registerWidgetHelp(m_disable_style_sheets, tr("Disable Style Sheets"), tr("Unchecked"),
                               tr("Disables the use of style sheets in the application, reverting to the original "
                                  "'Fusion' style but retaining the color scheme."));
    connect(m_ui.theme, &QComboBox::currentIndexChanged, this,
            &InterfaceSettingsWidget::updateDisableStyleSheetsEnabled);
    updateDisableStyleSheetsEnabled();

    SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.autoUpdateEnabled, "AutoUpdater", "CheckAtStartup", true);
    for (const auto& [name, desc] : AutoUpdaterDialog::getChannelList())
      m_ui.autoUpdateTag->addItem(desc, name);
    SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.autoUpdateTag, "AutoUpdater", "UpdateTag",
                                                   AutoUpdaterDialog::getDefaultTag());
    connect(m_ui.checkForUpdates, &QPushButton::clicked, this, &InterfaceSettingsWidget::checkForUpdates);

    m_ui.autoUpdateCurrentVersion->setText(tr("%1 (%2)").arg(g_scm_version_str).arg(g_scm_date_str));
  }
  else
  {
    QtUtils::SafeDeleteWidget(m_ui.languageLabel);
    QtUtils::SafeDeleteWidget(m_ui.language);
    QtUtils::SafeDeleteWidget(m_ui.themeLabel);
    QtUtils::SafeDeleteWidget(m_ui.theme);
    if (m_ui.appearanceLayout->isEmpty())
      QtUtils::SafeDeleteWidget(m_ui.appearanceGroup);

    delete m_ui.updatesGroup;
    m_ui.autoUpdateTagLabel = nullptr;
    m_ui.autoUpdateTag = nullptr;
    m_ui.autoUpdateCurrentVersionLabel = nullptr;
    m_ui.autoUpdateCurrentVersion = nullptr;
    m_ui.autoUpdateCheckLayout = nullptr;
    m_ui.autoUpdateEnabled = nullptr;
    m_ui.checkForUpdates = nullptr;
  }

  updateRenderToSeparateWindowOptions();

  dialog->registerWidgetHelp(m_ui.confirmGameClose, tr("Confirm Game Close"), tr("Checked"),
                             tr("Determines whether a prompt will be displayed to confirm closing the game."));
  dialog->registerWidgetHelp(m_ui.saveStateOnGameClose, tr("Save State On Game Close"), tr("Checked"),
                             tr("Automatically saves the system state when closing the game or exiting. You can then "
                                "resume directly from where you left off next time."));
  dialog->registerWidgetHelp(
    m_ui.inhibitScreensaver, tr("Inhibit Screensaver"), tr("Checked"),
    tr("Prevents the screen saver from activating and the host from sleeping while emulation is running."));
  dialog->registerWidgetHelp(m_ui.disableBackgroundInput, tr("Disable Background Input"), tr("Unchecked"),
                             tr("Prevents inputs from being processed when another application is active."));
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
  dialog->registerWidgetHelp(m_ui.hideMouseCursor, tr("Hide Cursor In Fullscreen"), tr("Checked"),
                             tr("Hides the mouse pointer/cursor when the emulator is in fullscreen mode."));
  dialog->registerWidgetHelp(
    m_ui.hideMainWindow, tr("Hide Main Window When Running"), tr("Unchecked"),
    tr("Hides the main window of the application while the game is displayed in a separate window."));
  dialog->registerWidgetHelp(m_ui.displayLogInMainWindow, tr("Display System Log In Main Window"), tr("Unchecked"),
                             tr("Displays the log in the main window of the application while a game is running."));
  dialog->registerWidgetHelp(m_ui.disableWindowResizing, tr("Disable Window Resizing"), tr("Unchecked"),
                             tr("Prevents resizing of the window while a game is running."));
  dialog->registerWidgetHelp(m_ui.automaticallyResizeWindow, tr("Automatically Resize Window"), tr("Unchecked"),
                             tr("Automatically resizes the window to match the internal resolution. <strong>For high "
                                "internal resolutions, this will create very large windows.</strong>"));

#if defined(_WIN32)
  dialog->registerWidgetHelp(
    disable_window_rounded_corners, tr("Disable Window Rounded Corners"), tr("Unchecked"),
    tr(
      "Disables the rounding of windows automatically applied in Windows 11, which may obscure parts of the content."));
#elif defined(__APPLE__)
  dialog->registerWidgetHelp(
    use_fractional_window_scale, tr("Use Fractional Window Scale"), tr("Unchecked"),
    tr("Calculates the true scaling factor for your display, avoiding the downsampling applied by MacOS."));
#endif

  if (!m_dialog->isPerGameSettings())
  {
    dialog->registerWidgetHelp(m_ui.language, tr("Language"), tr("System Language"),
                               tr("Selects the language for the application. Please note that not all parts of the "
                                  "application may be translated for a given language."));

    const char* default_theme_cname = QtHost::GetDefaultThemeName();
    QString default_theme_name;
    for (const auto& [name, display_name] : BUILTIN_THEMES)
    {
      if (std::strcmp(name, default_theme_cname) == 0)
      {
        default_theme_name = qApp->translate("MainWindow", display_name);
        break;
      }
    }
    dialog->registerWidgetHelp(m_ui.theme, tr("Theme"), default_theme_name,
                               tr("Selects the theme for the application."));

    dialog->registerWidgetHelp(m_ui.autoUpdateTag, tr("Update Channel"),
                               QString::fromStdString(AutoUpdaterDialog::getDefaultTag()),
                               tr("Selects the channel that will be checked for updates to the application. The "
                                  "<strong>preview</strong> channel contains the latest changes, and may be unstable. "
                                  "The <strong>latest</strong> channel tracks the latest release."));
    dialog->registerWidgetHelp(m_ui.autoUpdateEnabled, tr("Enable Automatic Update Check"), tr("Checked"),
                               tr("Automatically checks for updates to the program on startup. Updates can be deferred "
                                  "until later or skipped entirely."));
  }
}

InterfaceSettingsWidget::~InterfaceSettingsWidget() = default;

void InterfaceSettingsWidget::setupLanguageCombo(QComboBox* const cb)
{
  const auto language_list = Host::GetAvailableLanguageList();

  // Instantiating the fonts used for languages takes ~170ms on my machine, so we do this to avoid a delay in opening
  // the window. It effectively just shifts the delay to opening the dropdown instead, but most users are going to be
  // changing the language less frequently than opening the settings window.
  cb->setMinimumContentsLength(static_cast<int>(std::ranges::max(
    std::views::transform(language_list, [](const auto& it) { return StringUtil::GetUTF8CharacterCount(it.first); }))));
  cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  if (QListView* const view = qobject_cast<QListView*>(cb->view()))
  {
    view->setUniformItemSizes(true);
    view->setLayoutMode(QListView::Batched);
  }

  for (const auto& [language, code] : language_list)
  {
    cb->addItem(QtUtils::GetIconForTranslationLanguage(code), QString::fromUtf8(Host::GetLanguageName(code)),
                QString::fromLatin1(code));
  }

  SettingWidgetBinder::BindWidgetToStringSetting(nullptr, cb, "Main", "Language", {});
}

void InterfaceSettingsWidget::setupThemeCombo(QComboBox* const cb)
{
  for (const auto& [name, display_name] : BUILTIN_THEMES)
    cb->addItem(qApp->translate("MainWindow", display_name), QString::fromLatin1(name));
  for (const QString& name : QtHost::GetCustomThemeList())
  {
    if (cb->findData(name) < 0)
      cb->addItem(name, name);
  }

  SettingWidgetBinder::BindWidgetToStringSetting(nullptr, cb, "UI", "Theme", QtHost::GetDefaultThemeName());
  connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), cb, &QtHost::UpdateApplicationTheme);
}

void InterfaceSettingsWidget::updateRenderToSeparateWindowOptions()
{
  const bool render_to_separate_window = m_dialog->getEffectiveBoolValue("Main", "RenderToSeparateWindow", false);
  const bool hide_main_window = m_dialog->getEffectiveBoolValue("Main", "HideMainWindowWhenRunning", false);
  m_ui.hideMainWindow->setEnabled(render_to_separate_window);
  m_ui.displayLogInMainWindow->setEnabled(render_to_separate_window && !hide_main_window);
}

void InterfaceSettingsWidget::onLanguageChanged()
{
  QtHost::UpdateApplicationLanguage(this);
  g_main_window->recreate();
}

void InterfaceSettingsWidget::updateDisableStyleSheetsEnabled()
{
  m_disable_style_sheets->setEnabled(QtHost::IsStylesheetTheme(m_ui.theme->currentData().toString().toStdString()));
}

void InterfaceSettingsWidget::checkForUpdates()
{
  // this will return null if there's already a check in progress
  AutoUpdaterDialog* const dlg = g_main_window->createAutoUpdaterDialog(this, true);
  if (!dlg)
    return;

  QProgressDialog* const pdlg = new QProgressDialog(qApp->translate("MainWindow", "Checking for updates..."),
                                                    qApp->translate("QPlatformTheme", "Cancel"), 0, 0, this);
  pdlg->setWindowTitle(m_dialog->windowTitle());
  pdlg->setAttribute(Qt::WA_DeleteOnClose);
  pdlg->setMinimumWidth(400);
  pdlg->open();

  connect(pdlg, &QProgressDialog::canceled, dlg, &AutoUpdaterDialog::cancel);
  connect(dlg, &AutoUpdaterDialog::updateCheckAboutToComplete, pdlg, &QProgressDialog::close);

  dlg->queueUpdateCheck(true, true);
}
