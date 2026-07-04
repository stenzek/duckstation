// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "setupwizarddialog.h"
#include "achievementlogindialog.h"
#include "biossettingswidget.h"
#include "controllerbindingwidgets.h"
#include "controllersettingwidgetbinder.h"
#include "gamelistsettingswidget.h"
#include "gamelistwidget.h"
#include "graphicssettingswidget.h"
#include "interfacesettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/bios.h"
#include "core/controller.h"
#include "core/core.h"

#include "util/input_manager.h"
#include "util/translation.h"

#include "common/file_system.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtCore/QItemSelectionModel>
#include <QtWidgets/QButtonGroup>

#include "moc_setupwizarddialog.cpp"

using namespace Qt::StringLiterals;

SetupWizardDialog::SetupWizardDialog()
{
  setupUi();
  updatePageLabels(-1);
  updatePageButtons();
}

SetupWizardDialog::~SetupWizardDialog() = default;

bool SetupWizardDialog::canShowNextPage()
{
  const int current_page = m_ui.pages->currentIndex();

  switch (current_page)
  {
    case Page_BIOS:
    {
      if (!BIOS::HasAnyBIOSImages())
      {
        if (QtUtils::MessageBoxQuestion(
              this, tr("No BIOS Image Found"),
              tr("No BIOS images were found. DuckStation WILL NOT be able to run games without a BIOS image.\n\nAre "
                 "you sure you wish to continue without selecting a BIOS image?")) != QMessageBox::Yes)
        {
          return false;
        }
      }
    }
    break;

    case Page_GameList:
    {
      if (m_directory_model->rowCount() == 0)
      {
        if (QtUtils::MessageBoxQuestion(
              this, tr("No Game Directories Selected"),
              tr("No game directories have been selected. You will have to manually open any game dumps you "
                 "want to play, DuckStation's list will be empty.\n\nAre you sure you want to continue?")) !=
            QMessageBox::Yes)
        {
          return false;
        }
      }
    }
    break;

    default:
      break;
  }

  return true;
}

void SetupWizardDialog::previousPage()
{
  const int current_page = m_ui.pages->currentIndex();
  if (current_page == 0)
    return;

  m_ui.pages->setCurrentIndex(current_page - 1);
  updatePageLabels(current_page);
  updatePageButtons();
}

void SetupWizardDialog::nextPage()
{
  const int current_page = m_ui.pages->currentIndex();
  if (current_page == Page_Complete)
  {
    accept();
    return;
  }

  if (!canShowNextPage())
    return;

  const int new_page = current_page + 1;
  m_ui.pages->setCurrentIndex(new_page);
  updatePageLabels(current_page);
  updatePageButtons();
}

void SetupWizardDialog::updatePageLabels(int prev_page)
{
  if (prev_page >= 0)
  {
    QFont prev_font = m_page_labels[prev_page]->font();
    prev_font.setBold(false);
    m_page_labels[prev_page]->setFont(prev_font);
  }

  const int page = m_ui.pages->currentIndex();
  QFont font = m_page_labels[page]->font();
  font.setBold(true);
  m_page_labels[page]->setFont(font);
}

void SetupWizardDialog::updatePageButtons()
{
  const int page = m_ui.pages->currentIndex();
  m_ui.next->setText((page == Page_Complete) ? tr("&Finish") : tr("&Next"));
  m_ui.back->setEnabled(page > 0);
}

void SetupWizardDialog::confirmCancel()
{
  if (QtUtils::MessageBoxQuestion(
        this, tr("Cancel Setup"),
        tr("Are you sure you want to cancel DuckStation setup?\n\nAny changes have been saved, and "
           "the wizard will run again next time you start DuckStation.")) != QMessageBox::Yes)
  {
    return;
  }

  reject();
}

void SetupWizardDialog::setupUi()
{
  m_ui.setupUi(this);

  const QPixmap app_logo = QtHost::GetAppLogo();
  setWindowIcon(app_logo);
  m_ui.logo->setPixmap(app_logo);
  m_ui.pages->setCurrentIndex(0);

  m_page_labels[Page_Language] = m_ui.labelLanguage;
  m_page_labels[Page_BIOS] = m_ui.labelBIOS;
  m_page_labels[Page_GameList] = m_ui.labelGameList;
  m_page_labels[Page_Controller] = m_ui.labelController;
  m_page_labels[Page_Graphics] = m_ui.labelGraphics;
  m_page_labels[Page_Achievements] = m_ui.labelAchievements;
  m_page_labels[Page_Interface] = m_ui.labelInterface;
  m_page_labels[Page_GameListView] = m_ui.labelGameListView;
  m_page_labels[Page_Complete] = m_ui.labelComplete;

  connect(m_ui.back, &QPushButton::clicked, this, &SetupWizardDialog::previousPage);
  connect(m_ui.next, &QPushButton::clicked, this, &SetupWizardDialog::nextPage);
  connect(m_ui.cancel, &QPushButton::clicked, this, &SetupWizardDialog::confirmCancel);

  setupLanguagePage(true);
  setupBIOSPage();
  setupGameListPage(true);
  setupControllerPage(true);
  setupGraphicsPage(true);
  setupAchievementsPage(true);
  setupInterfacePage();
  setupGameListViewPage();
}

void SetupWizardDialog::setupLanguagePage(bool initial)
{
  SettingWidgetBinder::DisconnectWidget(m_ui.theme);
  m_ui.theme->clear();
  InterfaceSettingsWidget::setupThemeCombo(m_ui.theme);

  SettingWidgetBinder::DisconnectWidget(m_ui.language);
  m_ui.language->clear();
  InterfaceSettingsWidget::setupLanguageCombo(m_ui.language);
  connect(m_ui.language, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SetupWizardDialog::languageChanged);

  if (initial)
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.autoUpdateEnabled, "AutoUpdater", "CheckAtStartup",
                                                 true);
  }
}

void SetupWizardDialog::languageChanged()
{
  // Skip the recreation, since we don't have many dynamic UI elements.
  QtHost::UpdateApplicationLanguage(this);
  m_ui.retranslateUi(this);
  setupLanguagePage(false);
  setupGameListPage(false);
  setupControllerPage(false);
  setupGraphicsPage(false);
  setupAchievementsPage(false);
}

void SetupWizardDialog::setupBIOSPage()
{
  SettingWidgetBinder::BindWidgetToFolderSetting(nullptr, m_ui.biosSearchDirectory, m_ui.browseBiosSearchDirectory,
                                                 tr("Select BIOS Directory"), m_ui.openBiosSearchDirectory,
                                                 m_ui.resetBiosSearchDirectory, "BIOS", "SearchDirectory",
                                                 Path::Combine(EmuFolders::DataRoot, "bios"));

  refreshBiosList();

  connect(m_ui.biosSearchDirectory, &QLineEdit::textChanged, this, &SetupWizardDialog::refreshBiosList);
  connect(m_ui.refreshBiosList, &QPushButton::clicked, this, &SetupWizardDialog::refreshBiosList);
}

void SetupWizardDialog::refreshBiosList()
{
  auto list = BIOS::FindBIOSImagesInDirectory(m_ui.biosSearchDirectory->text().toUtf8().constData());
  BIOSSettingsWidget::populateDropDownForRegion(ConsoleRegion::NTSC_U, m_ui.imageNTSCU, list, false);
  BIOSSettingsWidget::populateDropDownForRegion(ConsoleRegion::NTSC_J, m_ui.imageNTSCJ, list, false);
  BIOSSettingsWidget::populateDropDownForRegion(ConsoleRegion::PAL, m_ui.imagePAL, list, false);

  BIOSSettingsWidget::setDropDownValue(m_ui.imageNTSCU, Core::GetBaseStringSettingValue("BIOS", "PathNTSCU"), false);
  BIOSSettingsWidget::setDropDownValue(m_ui.imageNTSCJ, Core::GetBaseStringSettingValue("BIOS", "PathNTSCJ"), false);
  BIOSSettingsWidget::setDropDownValue(m_ui.imagePAL, Core::GetBaseStringSettingValue("BIOS", "PathPAL"), false);
}

void SetupWizardDialog::setupGameListPage(bool initial)
{
  m_ui.gameDirectoriesDescription->setText(
    m_ui.gameDirectoriesDescription->text().arg(GameListWidget::getSupportedFormatsString().split('\n').join(", ")));
  if (!initial)
    return;

  m_directory_model = new GameListSearchDirectoriesModel(this);
  m_ui.searchDirectoryList->setModel(m_directory_model);
  QtUtils::SetColumnWidthsForTreeView(m_ui.searchDirectoryList, {-1, 100});

  connect(m_ui.searchDirectoryList, &QTreeView::customContextMenuRequested, this,
          &SetupWizardDialog::onDirectoryListContextMenuRequested);
  connect(m_ui.addSearchDirectoryButton, &QPushButton::clicked, this,
          &SetupWizardDialog::onAddSearchDirectoryButtonClicked);
  connect(m_ui.removeSearchDirectoryButton, &QPushButton::clicked, this,
          &SetupWizardDialog::onRemoveSearchDirectoryButtonClicked);
  connect(m_ui.searchDirectoryList->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          &SetupWizardDialog::onSearchDirectoryListSelectionChanged);

  refreshDirectoryList();
}

void SetupWizardDialog::onDirectoryListContextMenuRequested(const QPoint& point)
{
  const QModelIndex index = m_ui.searchDirectoryList->currentIndex();
  if (!index.isValid())
    return;

  const QString path = QString::fromStdString(m_directory_model->pathForIndex(index));

  QMenu* const menu = QtUtils::NewPopupMenu(this);
  menu->addAction(QIcon(u":/icons/monochrome/svg/folder-reduce-line.svg"_s), tr("Remove"), this,
                  &SetupWizardDialog::onRemoveSearchDirectoryButtonClicked);
  menu->addSeparator();
  menu->addAction(QIcon(u":/icons/monochrome/svg/folder-open-line.svg"_s), tr("Open Directory..."),
                  [this, path]() { QtUtils::OpenURL(this, QUrl::fromLocalFile(path)); });
  menu->popup(m_ui.searchDirectoryList->mapToGlobal(point));
}

void SetupWizardDialog::onAddSearchDirectoryButtonClicked()
{
  QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select Search Directory")));

  if (dir.isEmpty())
    return;

  QMessageBox::StandardButton selection = QtUtils::MessageBoxQuestion(
    this, tr("Scan Recursively?"),
    tr("Would you like to scan the directory \"%1\" recursively?\n\nScanning recursively takes "
       "more time, but will identify files in subdirectories.")
      .arg(dir),
    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (selection != QMessageBox::Yes && selection != QMessageBox::No)
    return;

  m_directory_model->addPath(dir.toStdString(), selection == QMessageBox::Yes);
}

void SetupWizardDialog::onRemoveSearchDirectoryButtonClicked()
{
  const QModelIndex index = m_ui.searchDirectoryList->currentIndex();
  if (!index.isValid())
    return;

  m_directory_model->removePath(index);
}

void SetupWizardDialog::onSearchDirectoryListSelectionChanged()
{
  m_ui.removeSearchDirectoryButton->setEnabled(m_ui.searchDirectoryList->selectionModel()->hasSelection());
}

void SetupWizardDialog::refreshDirectoryList()
{
  m_directory_model->reload();
  m_ui.removeSearchDirectoryButton->setEnabled(false);
}

void SetupWizardDialog::setupControllerPage(bool initial)
{
  static constexpr u32 NUM_PADS = 2;

  struct PadWidgets
  {
    QComboBox* type_combo;
    QLabel* mapping_result;
    QPushButton* mapping_button;
  };
  const PadWidgets pad_widgets[NUM_PADS] = {
    {m_ui.controller1Type, m_ui.controller1Mapping, m_ui.controller1AutomaticMapping},
    {m_ui.controller2Type, m_ui.controller2Mapping, m_ui.controller2AutomaticMapping},
  };

  if (!initial)
  {
    for (const PadWidgets& w : pad_widgets)
    {
      w.type_combo->blockSignals(true);
      w.type_combo->clear();
    }
  }

  for (u32 port = 0; port < NUM_PADS; port++)
  {
    const std::string section = fmt::format("Pad{}", port + 1);
    const PadWidgets& w = pad_widgets[port];

    for (const Controller::ControllerInfo* cinfo : Controller::GetControllerInfoList())
      w.type_combo->addItem(QtUtils::StringViewToQString(cinfo->GetDisplayName()), QString::fromUtf8(cinfo->name));

    ControllerSettingWidgetBinder::BindWidgetToInputProfileString(
      nullptr, w.type_combo, section, "Type",
      Controller::GetControllerInfo(Settings::GetDefaultControllerType(port)).name);

    w.mapping_result->setText(findCurrentDeviceForPort(port));

    if (initial)
    {
      connect(w.mapping_button, &QAbstractButton::clicked, this,
              [this, port, label = w.mapping_result]() { openAutomaticMappingMenu(port, label); });
    }
  }

  if (!initial)
  {
    for (const PadWidgets& w : pad_widgets)
      w.type_combo->blockSignals(false);
  }

  m_ui.pauseMenuHotkey->initialize(nullptr, InputBindingInfo::Type::Button, "Hotkeys", "OpenPauseMenu",
                                   tr("Open Pause Menu"));
}

void SetupWizardDialog::updateStylesheets()
{
}

QString SetupWizardDialog::findCurrentDeviceForPort(u32 port) const
{
  const auto lock = Core::GetSettingsLock();
  return QString::fromStdString(InputManager::GetPhysicalDeviceForController(*Core::GetBaseSettingsLayer(), port));
}

void SetupWizardDialog::openAutomaticMappingMenu(u32 port, QLabel* update_label)
{
  QMenu* const menu = QtUtils::NewPopupMenu(this);
  bool added = false;

  for (const InputDeviceListModel::Device& dev : g_core_thread->getInputDeviceListModel()->getDeviceList())
  {
    // we set it as data, because the device list could get invalidated while the menu is up
    menu->addAction(
      InputDeviceListModel::getIconForKey(dev.key), QStringLiteral("%1 (%2)").arg(dev.identifier).arg(dev.display_name),
      [this, port, update_label, device = dev.identifier]() { doDeviceAutomaticBinding(port, update_label, device); });
    added = true;
  }

  if (added)
  {
    menu->addAction(tr("Multiple Devices..."),
                    [this, port, update_label]() { doMultipleDeviceAutomaticBinding(port, update_label); });
  }
  else
  {
    menu->addAction(tr("No devices available"))->setEnabled(false);
  }

  menu->popup(QCursor::pos());
}

void SetupWizardDialog::doDeviceAutomaticBinding(u32 port, QLabel* update_label, const QString& device)
{
  std::vector<std::pair<GenericInputBinding, std::string>> mapping =
    InputManager::GetGenericBindingMapping(device.toStdString());
  if (mapping.empty())
  {
    QtUtils::AsyncMessageBox(
      this, QMessageBox::Critical, tr("Automatic Binding Failed"),
      tr("No generic bindings were generated for device '%1'. The controller/source may not support automatic "
         "mapping.")
        .arg(device));
    return;
  }

  bool result;
  {
    const auto lock = Core::GetSettingsLock();
    result = InputManager::MapController(*Core::GetBaseSettingsLayer(), port, mapping, true);
  }
  if (!result)
    return;

  Host::CommitBaseSettingChanges();

  update_label->setText(device);
}

void SetupWizardDialog::doMultipleDeviceAutomaticBinding(u32 port, QLabel* update_label)
{
  QDialog* const dialog = new MultipleDeviceAutobindDialog(this, nullptr, port);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(dialog, &QDialog::accepted, this,
          [this, port, update_label] { update_label->setText(findCurrentDeviceForPort(port)); });
  dialog->open();
}

void SetupWizardDialog::setupGraphicsPage(bool initial)
{
  if (initial)
  {
    m_ui.resolutionScaleWarningIcon->setPixmap(
      QIcon(QtHost::GetResourceQPath("images/warning.svg", true)).pixmap(16, 16));
  }
  m_ui.resolutionScaleWarningIcon->setToolTip(
    tr("PGXP is not enabled. Increasing the resolution without enabling PGXP will result in visible polygon "
       "glitches."));

  SettingWidgetBinder::DisconnectWidget(m_ui.resolutionScale);
  m_ui.resolutionScale->clear();
  GraphicsSettingsWidget::populateUpscalingModes(m_ui.resolutionScale);
  SettingWidgetBinder::BindWidgetToIntSetting(nullptr, m_ui.resolutionScale, "GPU", "ResolutionScale", 1);
  connect(m_ui.resolutionScale, &QComboBox::currentIndexChanged, this,
          &SetupWizardDialog::updateResolutionScaleWarning);

  SettingWidgetBinder::DisconnectWidget(m_ui.textureFiltering);
  m_ui.textureFiltering->clear();
  SettingWidgetBinder::DisconnectWidget(m_ui.spriteTextureFiltering);
  m_ui.spriteTextureFiltering->clear();

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.textureFiltering, "GPU", "TextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               &Settings::GetTextureFilterDisplayName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER, GPUTextureFilter::Count);
  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.spriteTextureFiltering, "GPU", "SpriteTextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               &Settings::GetTextureFilterDisplayName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER, GPUTextureFilter::Count);

  SettingWidgetBinder::DisconnectWidget(m_ui.gpuDitheringMode);
  m_ui.gpuDitheringMode->clear();

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.gpuDitheringMode, "GPU", "DitheringMode",
                                               &Settings::ParseGPUDitheringModeName, &Settings::GetGPUDitheringModeName,
                                               &Settings::GetGPUDitheringModeDisplayName,
                                               Settings::DEFAULT_GPU_DITHERING_MODE, GPUDitheringMode::MaxCount);

  SettingWidgetBinder::DisconnectWidget(m_ui.displayAspectRatio);
  m_ui.displayAspectRatio->clear();

  GraphicsSettingsWidget::createAspectRatioSetting(m_ui.displayAspectRatio, m_ui.customAspectRatioNumerator,
                                                   m_ui.customAspectRatioSeparator, m_ui.customAspectRatioDenominator,
                                                   nullptr);

  SettingWidgetBinder::DisconnectWidget(m_ui.displayCropMode);
  m_ui.displayCropMode->clear();

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.displayCropMode, "Display", "CropMode",
                                               &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName,
                                               &Settings::GetDisplayCropModeDisplayName,
                                               Settings::DEFAULT_DISPLAY_CROP_MODE, DisplayCropMode::MaxCount);

  SettingWidgetBinder::DisconnectWidget(m_ui.displayScaling);
  m_ui.displayScaling->clear();

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.displayScaling, "Display", "Scaling",
                                               &Settings::ParseDisplayScaling, &Settings::GetDisplayScalingName,
                                               &Settings::GetDisplayScalingDisplayName,
                                               Settings::DEFAULT_DISPLAY_SCALING, DisplayScalingMode::Count);

  SettingWidgetBinder::DisconnectWidget(m_ui.displayScaling24Bit);
  m_ui.displayScaling24Bit->clear();
  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.displayScaling24Bit, "Display", "Scaling24Bit",
                                               &Settings::ParseDisplayScaling, &Settings::GetDisplayScalingName,
                                               &Settings::GetDisplayScalingDisplayName,
                                               Settings::DEFAULT_DISPLAY_SCALING, DisplayScalingMode::Count);

  if (initial)
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.pgxpEnable, "GPU", "PGXPEnable", false);
    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.widescreenHack, "GPU", "WidescreenHack", false);
    connect(m_ui.pgxpEnable, &QCheckBox::checkStateChanged, this, &SetupWizardDialog::updateResolutionScaleWarning);
  }

  updateResolutionScaleWarning();
}

void SetupWizardDialog::updateResolutionScaleWarning()
{
  m_ui.resolutionScaleWarningIcon->setVisible(m_ui.resolutionScale->currentIndex() != 1 &&
                                              !m_ui.pgxpEnable->isChecked());
}

void SetupWizardDialog::setupAchievementsPage(bool initial)
{
  if (initial)
  {
    m_ui.achievementsIconLabel->setPixmap(QPixmap(QtHost::GetResourceQPath("images/ra-icon.webp", true)));
    QFont title_font(m_ui.achievementsTitleLabel->font());
    title_font.setBold(true);
    title_font.setPixelSize(20);
    m_ui.achievementsTitleLabel->setFont(title_font);

    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.enable, "Cheevos", "Enabled", false);
    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.hardcoreMode, "Cheevos", "ChallengeMode", false);
    connect(m_ui.enable, &QCheckBox::checkStateChanged, this, &SetupWizardDialog::updateAchievementsEnableState);
    connect(m_ui.achievementsLoginButton, &QPushButton::clicked, this, &SetupWizardDialog::onAchievementsLoginPressed);
    connect(m_ui.achievementsLogoutButton, &QPushButton::clicked, this,
            &SetupWizardDialog::onAchievementsLogoutPressed);
    connect(m_ui.achievementsRegisterUserButton, &QPushButton::clicked, this,
            &SetupWizardDialog::onAchievementsRegisterUserPressed);
    connect(m_ui.achievementsViewProfileButton, &QPushButton::clicked, this,
            &SetupWizardDialog::onAchievementsViewProfilePressed);
  }

  updateAchievementsEnableState();
  updateAchievementsLoginState();
}

void SetupWizardDialog::updateAchievementsEnableState()
{
  const bool enabled = Core::GetBaseBoolSettingValue("Cheevos", "Enabled", false);
  m_ui.hardcoreMode->setEnabled(enabled);
}

void SetupWizardDialog::updateAchievementsLoginState()
{
  m_ui.achievementsUserBadge->setPixmap(QPixmap(QtHost::GetResourceQPath("images/ra-generic-user.png", true)));

  QString qusername;
  QString qbadge_path;

  {
    const auto lock = Achievements::GetLock();
    if (Achievements::IsLoggedIn())
    {
      qusername = QString::fromStdString(Achievements::GetLoggedInUserName());
      QtUtils::SetLabelPixmapPathOrURL(m_ui.achievementsUserBadge, Achievements::GetLoggedInUserIconURL(), true);
    }
    else
    {
      qusername = QString::fromStdString(Core::GetBaseStringSettingValue("Cheevos", "Username"));
    }
  }

  const bool logged_in = !qusername.isEmpty();

  if (logged_in)
  {
    const u64 login_unix_timestamp =
      StringUtil::FromChars<u64>(Core::GetBaseStringSettingValue("Cheevos", "LoginTimestamp", "0")).value_or(0);
    const QString login_timestamp =
      QtHost::FormatNumber(Host::NumberFormatType::ShortDateTime, static_cast<s64>(login_unix_timestamp));
    m_ui.achievementsLoginStatus->setText(
      tr("Logged in as %1\nToken generated at %2").arg(qusername).arg(login_timestamp));
  }
  else
  {
    m_ui.achievementsLoginStatus->setText(tr("Not Logged In."));
  }

  m_ui.achievementsViewProfileButton->setVisible(logged_in);
  m_ui.achievementsViewProfileButton->setEnabled(logged_in);
  m_ui.achievementsLogoutButton->setVisible(logged_in);
  m_ui.achievementsLogoutButton->setEnabled(logged_in);
  m_ui.achievementsRegisterUserButton->setVisible(!logged_in);
  m_ui.achievementsRegisterUserButton->setEnabled(!logged_in);
  m_ui.achievementsLoginButton->setVisible(!logged_in);
  m_ui.achievementsLoginButton->setEnabled(!logged_in);
}

void SetupWizardDialog::onAchievementsLoginPressed()
{
  AchievementLoginDialog* login = new AchievementLoginDialog(this, Achievements::LoginRequestReason::UserInitiated);
  connect(login, &AchievementLoginDialog::accepted, this, &SetupWizardDialog::onAchievementsLoginCompleted);
  login->open();
}

void SetupWizardDialog::onAchievementsLogoutPressed()
{
  if (Core::GetBaseStringSettingValue("Cheevos", "Username").empty())
    return;

  // Really should do this on the core thread, but it's luckily not doing anything at this point.
  Achievements::Logout();
  updateAchievementsLoginState();
}

void SetupWizardDialog::onAchievementsLoginCompleted()
{
  updateAchievementsLoginState();

  // Login can enable achievements/hardcore.
  if (!m_ui.enable->isChecked() && Core::GetBaseBoolSettingValue("Cheevos", "Enabled", false))
  {
    m_ui.enable->setChecked(true);
    updateAchievementsEnableState();
  }
  if (!m_ui.hardcoreMode->isChecked() && Core::GetBaseBoolSettingValue("Cheevos", "ChallengeMode", false))
    m_ui.hardcoreMode->setChecked(true);
}

void SetupWizardDialog::onAchievementsRegisterUserPressed()
{
  QtUtils::OpenURL(this, QUrl(QString::fromLatin1(Achievements::RA_REGISTER_URL)));
}

void SetupWizardDialog::onAchievementsViewProfilePressed()
{
  const std::string username(Core::GetBaseStringSettingValue("Cheevos", "Username"));
  if (username.empty())
    return;

  QtUtils::OpenURL(this, QUrl(QString::fromStdString(Achievements::GetProfileURL(username))));
}

void SetupWizardDialog::setupGameListViewPage()
{
  const bool use_grid = Core::GetBaseBoolSettingValue("UI", "GameListGridView", false);
  m_ui.listView->setChecked(!use_grid);
  m_ui.gridView->setChecked(use_grid);

  connect(m_ui.listView, &QRadioButton::toggled, this, &SetupWizardDialog::onGridViewChanged);
  connect(m_ui.gridView, &QRadioButton::toggled, this, &SetupWizardDialog::onGridViewChanged);
}

void SetupWizardDialog::onGridViewChanged(bool checked)
{
  if (!checked)
    return;

  bool setting_value;
  if (const QObject* const sender_widget = sender(); sender_widget == m_ui.listView)
    setting_value = false;
  else if (sender_widget == m_ui.gridView)
    setting_value = true;
  else
    return;

  // NOTE: No settings apply here, we explicitly change the layout.
  Core::SetBaseBoolSettingValue("UI", "GameListGridView", setting_value);
  Core::SetBaseUIntSettingValue("Main", "DefaultFullscreenUIGameView", setting_value ? 0 : 1);
  Host::CommitBaseSettingChanges();
  g_main_window->getGameListWidget()->reloadViewModeFromSettings();
}

void SetupWizardDialog::setupInterfacePage()
{
  const bool use_big_picture = Core::GetBaseBoolSettingValue("Main", "StartFullscreenUI", false);
  m_ui.desktopMode->setChecked(!use_big_picture);
  m_ui.bigPictureMode->setChecked(use_big_picture);

  connect(m_ui.desktopMode, &QRadioButton::toggled, this, &SetupWizardDialog::onStartFullscreenUIChanged);
  connect(m_ui.bigPictureMode, &QRadioButton::toggled, this, &SetupWizardDialog::onStartFullscreenUIChanged);
}

void SetupWizardDialog::onStartFullscreenUIChanged(bool checked)
{
  if (!checked)
    return;

  bool setting_value;
  if (const QObject* const sender_widget = sender(); sender_widget == m_ui.desktopMode)
    setting_value = false;
  else if (sender_widget == m_ui.bigPictureMode)
    setting_value = true;
  else
    return;

  // NOTE: No settings apply here, this is queried after the wizard completes.
  Core::SetBaseBoolSettingValue("Main", "StartFullscreenUI", setting_value);
  Host::CommitBaseSettingChanges();
}
