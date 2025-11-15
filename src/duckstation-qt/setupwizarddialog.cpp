// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "setupwizarddialog.h"
#include "achievementlogindialog.h"
#include "biossettingswidget.h"
#include "controllerbindingwidgets.h"
#include "controllersettingwidgetbinder.h"
#include "graphicssettingswidget.h"
#include "interfacesettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/bios.h"
#include "core/controller.h"

#include "util/input_manager.h"

#include "common/file_system.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include "moc_setupwizarddialog.cpp"

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
              this, tr("Warning"),
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
      if (m_ui.searchDirectoryList->rowCount() == 0)
      {
        if (QtUtils::MessageBoxQuestion(
              this, tr("Warning"),
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

  m_ui.logo->setPixmap(
    QPixmap(QString::fromUtf8(Path::Combine(EmuFolders::Resources, "images" FS_OSPATH_SEPARATOR_STR "duck.png"))));

  m_ui.pages->setCurrentIndex(0);

  m_page_labels[Page_Language] = m_ui.labelLanguage;
  m_page_labels[Page_BIOS] = m_ui.labelBIOS;
  m_page_labels[Page_GameList] = m_ui.labelGameList;
  m_page_labels[Page_Controller] = m_ui.labelController;
  m_page_labels[Page_Graphics] = m_ui.labelGraphics;
  m_page_labels[Page_Achievements] = m_ui.labelAchievements;
  m_page_labels[Page_Complete] = m_ui.labelComplete;

  connect(m_ui.back, &QPushButton::clicked, this, &SetupWizardDialog::previousPage);
  connect(m_ui.next, &QPushButton::clicked, this, &SetupWizardDialog::nextPage);
  connect(m_ui.cancel, &QPushButton::clicked, this, &SetupWizardDialog::confirmCancel);

  setupLanguagePage(true);
  setupBIOSPage();
  setupGameListPage();
  setupControllerPage(true);
  setupGraphicsPage(true);
  setupAchievementsPage(true);
}

void SetupWizardDialog::setupLanguagePage(bool initial)
{
  SettingWidgetBinder::DisconnectWidget(m_ui.theme);
  m_ui.theme->clear();
  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.theme, "UI", "Theme", InterfaceSettingsWidget::THEME_NAMES,
                                               InterfaceSettingsWidget::THEME_VALUES, QtHost::GetDefaultThemeName(),
                                               "MainWindow");
  connect(m_ui.theme, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWizardDialog::themeChanged);

  SettingWidgetBinder::DisconnectWidget(m_ui.language);
  m_ui.language->clear();
  InterfaceSettingsWidget::populateLanguageDropdown(m_ui.language);
  SettingWidgetBinder::BindWidgetToStringSetting(nullptr, m_ui.language, "Main", "Language", {});
  connect(m_ui.language, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SetupWizardDialog::languageChanged);

  if (initial)
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.autoUpdateEnabled, "AutoUpdater", "CheckAtStartup",
                                                 true);
  }
}

void SetupWizardDialog::themeChanged()
{
  // Main window gets recreated at the end here anyway, so it's fine to just yolo it.
  QtHost::UpdateApplicationTheme();
}

void SetupWizardDialog::languageChanged()
{
  // Skip the recreation, since we don't have many dynamic UI elements.
  QtHost::UpdateApplicationLanguage(this);
  m_ui.retranslateUi(this);
  setupLanguagePage(false);
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

  BIOSSettingsWidget::setDropDownValue(m_ui.imageNTSCU, Host::GetBaseStringSettingValue("BIOS", "PathNTSCU"), false);
  BIOSSettingsWidget::setDropDownValue(m_ui.imageNTSCJ, Host::GetBaseStringSettingValue("BIOS", "PathNTSCJ"), false);
  BIOSSettingsWidget::setDropDownValue(m_ui.imagePAL, Host::GetBaseStringSettingValue("BIOS", "PathPAL"), false);
}

void SetupWizardDialog::setupGameListPage()
{
  m_ui.searchDirectoryList->setSelectionMode(QAbstractItemView::SingleSelection);
  m_ui.searchDirectoryList->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_ui.searchDirectoryList->setAlternatingRowColors(true);
  m_ui.searchDirectoryList->setShowGrid(false);
  m_ui.searchDirectoryList->horizontalHeader()->setHighlightSections(false);
  m_ui.searchDirectoryList->verticalHeader()->hide();
  m_ui.searchDirectoryList->setCurrentIndex({});
  m_ui.searchDirectoryList->setContextMenuPolicy(Qt::ContextMenuPolicy::CustomContextMenu);
  QtUtils::SetColumnWidthsForTableView(m_ui.searchDirectoryList, {-1, 100});

  connect(m_ui.searchDirectoryList, &QTableWidget::customContextMenuRequested, this,
          &SetupWizardDialog::onDirectoryListContextMenuRequested);
  connect(m_ui.addSearchDirectoryButton, &QPushButton::clicked, this,
          &SetupWizardDialog::onAddSearchDirectoryButtonClicked);
  connect(m_ui.removeSearchDirectoryButton, &QPushButton::clicked, this,
          &SetupWizardDialog::onRemoveSearchDirectoryButtonClicked);
  connect(m_ui.searchDirectoryList, &QTableWidget::itemSelectionChanged, this,
          &SetupWizardDialog::onSearchDirectoryListSelectionChanged);

  refreshDirectoryList();
}

void SetupWizardDialog::onDirectoryListContextMenuRequested(const QPoint& point)
{
  QModelIndexList selection = m_ui.searchDirectoryList->selectionModel()->selectedIndexes();
  if (selection.size() < 1)
    return;

  const int row = selection[0].row();

  QMenu* const menu = QtUtils::NewPopupMenu(this);
  menu->addAction(tr("Remove"), [this]() { onRemoveSearchDirectoryButtonClicked(); });
  menu->addSeparator();
  menu->addAction(tr("Open Directory..."), [this, row]() {
    QtUtils::OpenURL(this, QUrl::fromLocalFile(m_ui.searchDirectoryList->item(row, 0)->text()));
  });
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

  const bool recursive = (selection == QMessageBox::Yes);
  const std::string spath = dir.toStdString();
  Host::RemoveValueFromBaseStringListSetting("GameList", recursive ? "Paths" : "RecursivePaths", spath.c_str());
  Host::AddValueToBaseStringListSetting("GameList", recursive ? "RecursivePaths" : "Paths", spath.c_str());
  Host::CommitBaseSettingChanges();
  refreshDirectoryList();
}

void SetupWizardDialog::onRemoveSearchDirectoryButtonClicked()
{
  const int row = m_ui.searchDirectoryList->currentRow();
  std::unique_ptr<QTableWidgetItem> item((row >= 0) ? m_ui.searchDirectoryList->takeItem(row, 0) : nullptr);
  if (!item)
    return;

  const std::string spath = item->text().toStdString();
  if (!Host::RemoveValueFromBaseStringListSetting("GameList", "Paths", spath.c_str()) &&
      !Host::RemoveValueFromBaseStringListSetting("GameList", "RecursivePaths", spath.c_str()))
  {
    return;
  }

  Host::CommitBaseSettingChanges();
  refreshDirectoryList();
}

void SetupWizardDialog::onSearchDirectoryListSelectionChanged()
{
  m_ui.removeSearchDirectoryButton->setEnabled(!m_ui.searchDirectoryList->selectedItems().isEmpty());
}

void SetupWizardDialog::addPathToTable(const std::string& path, bool recursive)
{
  const int row = m_ui.searchDirectoryList->rowCount();
  m_ui.searchDirectoryList->insertRow(row);

  QTableWidgetItem* item = new QTableWidgetItem();
  item->setText(QString::fromStdString(path));
  item->setFlags(item->flags() & ~(Qt::ItemIsEditable));
  m_ui.searchDirectoryList->setItem(row, 0, item);

  QCheckBox* cb = new QCheckBox(m_ui.searchDirectoryList);
  m_ui.searchDirectoryList->setCellWidget(row, 1, cb);
  cb->setChecked(recursive);

  connect(cb, &QCheckBox::checkStateChanged, [item](Qt::CheckState state) {
    const std::string path(item->text().toStdString());
    if (state == Qt::Checked)
    {
      Host::RemoveValueFromBaseStringListSetting("GameList", "Paths", path.c_str());
      Host::AddValueToBaseStringListSetting("GameList", "RecursivePaths", path.c_str());
    }
    else
    {
      Host::RemoveValueFromBaseStringListSetting("GameList", "RecursivePaths", path.c_str());
      Host::AddValueToBaseStringListSetting("GameList", "Paths", path.c_str());
    }
    Host::CommitBaseSettingChanges();
  });
}

void SetupWizardDialog::refreshDirectoryList()
{
  QSignalBlocker sb(m_ui.searchDirectoryList);
  while (m_ui.searchDirectoryList->rowCount() > 0)
    m_ui.searchDirectoryList->removeRow(0);

  std::vector<std::string> path_list = Host::GetBaseStringListSetting("GameList", "Paths");
  for (const std::string& entry : path_list)
    addPathToTable(entry, false);

  path_list = Host::GetBaseStringListSetting("GameList", "RecursivePaths");
  for (const std::string& entry : path_list)
    addPathToTable(entry, true);

  m_ui.searchDirectoryList->sortByColumn(0, Qt::AscendingOrder);
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
}

void SetupWizardDialog::updateStylesheets()
{
}

QString SetupWizardDialog::findCurrentDeviceForPort(u32 port) const
{
  auto lock = Host::GetSettingsLock();
  return QString::fromStdString(
    InputManager::GetPhysicalDeviceForController(*Host::Internal::GetBaseSettingsLayer(), port));
}

void SetupWizardDialog::openAutomaticMappingMenu(u32 port, QLabel* update_label)
{
  QMenu* const menu = QtUtils::NewPopupMenu(this);
  bool added = false;

  for (const InputDeviceListModel::Device& dev : g_emu_thread->getInputDeviceListModel()->getDeviceList())
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
    QtUtils::MessageBoxCritical(
      this, tr("Automatic Binding"),
      tr("No generic bindings were generated for device '%1'. The controller/source may not support automatic "
         "mapping.")
        .arg(device));
    return;
  }

  bool result;
  {
    auto lock = Host::GetSettingsLock();
    result = InputManager::MapController(*Host::Internal::GetBaseSettingsLayer(), port, mapping, true);
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
  SettingWidgetBinder::DisconnectWidget(m_ui.resolutionScale);
  m_ui.resolutionScale->clear();
  GraphicsSettingsWidget::populateUpscalingModes(m_ui.resolutionScale, 16);
  SettingWidgetBinder::BindWidgetToIntSetting(nullptr, m_ui.resolutionScale, "GPU", "ResolutionScale", 1);

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
  }
}

void SetupWizardDialog::setupAchievementsPage(bool initial)
{
  if (initial)
  {
    m_ui.achievementsIconLabel->setPixmap(
      QPixmap(QString::fromStdString(QtHost::GetResourcePath("images/ra-icon.webp", true))));
    QFont title_font(m_ui.achievementsTitleLabel->font());
    title_font.setBold(true);
    title_font.setPixelSize(20);
    m_ui.achievementsTitleLabel->setFont(title_font);

    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.enable, "Cheevos", "Enabled", false);
    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.hardcoreMode, "Cheevos", "ChallengeMode", false);
    connect(m_ui.enable, &QCheckBox::checkStateChanged, this, &SetupWizardDialog::updateAchievementsEnableState);
    connect(m_ui.loginButton, &QPushButton::clicked, this, &SetupWizardDialog::onAchievementsLoginLogoutClicked);
    connect(m_ui.viewProfile, &QPushButton::clicked, this, &SetupWizardDialog::onAchievementsViewProfileClicked);
  }

  updateAchievementsEnableState();
  updateAchievementsLoginState();
}

void SetupWizardDialog::updateAchievementsEnableState()
{
  const bool enabled = Host::GetBaseBoolSettingValue("Cheevos", "Enabled", false);
  m_ui.hardcoreMode->setEnabled(enabled);
}

void SetupWizardDialog::updateAchievementsLoginState()
{
  const std::string username(Host::GetBaseStringSettingValue("Cheevos", "Username"));
  const bool logged_in = !username.empty();

  if (logged_in)
  {
    const u64 login_unix_timestamp =
      StringUtil::FromChars<u64>(Host::GetBaseStringSettingValue("Cheevos", "LoginTimestamp", "0")).value_or(0);
    const QString login_timestamp =
      QtHost::FormatNumber(Host::NumberFormatType::ShortDateTime, static_cast<s64>(login_unix_timestamp));
    m_ui.loginStatus->setText(
      tr("Username: %1\nLogin token generated on %2.").arg(QString::fromStdString(username)).arg(login_timestamp));
    m_ui.loginButton->setText(tr("Logout"));
  }
  else
  {
    m_ui.loginStatus->setText(tr("Not Logged In."));
    m_ui.loginButton->setText(tr("Login..."));
  }

  m_ui.viewProfile->setEnabled(logged_in);
}

void SetupWizardDialog::onAchievementsLoginLogoutClicked()
{
  if (!Host::GetBaseStringSettingValue("Cheevos", "Username").empty())
  {
    Host::RunOnCPUThread([]() { Achievements::Logout(); }, true);
    updateAchievementsLoginState();
    return;
  }

  AchievementLoginDialog* login = new AchievementLoginDialog(this, Achievements::LoginRequestReason::UserInitiated);
  connect(login, &AchievementLoginDialog::accepted, this, &SetupWizardDialog::onAchievementsLoginCompleted);
  login->open();
}

void SetupWizardDialog::onAchievementsLoginCompleted()
{
  updateAchievementsEnableState();
  updateAchievementsLoginState();

  // Login can enable achievements/hardcore.
  if (!m_ui.enable->isChecked() && Host::GetBaseBoolSettingValue("Cheevos", "Enabled", false))
  {
    QSignalBlocker sb(m_ui.enable);
    m_ui.enable->setChecked(true);
    updateAchievementsLoginState();
  }
  if (!m_ui.hardcoreMode->isChecked() && Host::GetBaseBoolSettingValue("Cheevos", "ChallengeMode", false))
  {
    QSignalBlocker sb(m_ui.hardcoreMode);
    m_ui.hardcoreMode->setChecked(true);
  }
}

void SetupWizardDialog::onAchievementsViewProfileClicked()
{
  const std::string username(Host::GetBaseStringSettingValue("Cheevos", "Username"));
  if (username.empty())
    return;

  const QByteArray encoded_username(QUrl::toPercentEncoding(QString::fromStdString(username)));
  QtUtils::OpenURL(
    QtUtils::GetRootWidget(this),
    QUrl(QStringLiteral("https://retroachievements.org/user/%1").arg(QString::fromUtf8(encoded_username))));
}
