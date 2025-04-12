// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "setupwizarddialog.h"
#include "achievementlogindialog.h"
#include "controllerbindingwidgets.h"
#include "controllersettingwidgetbinder.h"
#include "graphicssettingswidget.h"
#include "interfacesettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"

#include "core/achievements.h"
#include "core/controller.h"

#include "util/input_manager.h"

#include "common/file_system.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtWidgets/QMessageBox>

SetupWizardDialog::SetupWizardDialog()
{
  setupUi();
  updatePageLabels(-1);
  updatePageButtons();
}

SetupWizardDialog::~SetupWizardDialog() = default;

void SetupWizardDialog::resizeEvent(QResizeEvent* event)
{
  QDialog::resizeEvent(event);
  resizeDirectoryListColumns();
}

bool SetupWizardDialog::canShowNextPage()
{
  const int current_page = m_ui.pages->currentIndex();

  switch (current_page)
  {
    case Page_BIOS:
    {
      if (!BIOS::HasAnyBIOSImages())
      {
        if (QMessageBox::question(
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
        if (QMessageBox::question(
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
  pageChangedTo(new_page);
}

void SetupWizardDialog::pageChangedTo(int page)
{
  switch (page)
  {
    case Page_GameList:
      resizeDirectoryListColumns();
      break;

    default:
      break;
  }
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
  if (QMessageBox::question(this, tr("Cancel Setup"),
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

  setupLanguagePage();
  setupBIOSPage();
  setupGameListPage();
  setupControllerPage(true);
  setupGraphicsPage(true);
  setupAchievementsPage(true);
}

void SetupWizardDialog::setupLanguagePage()
{
  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.theme, "UI", "Theme", InterfaceSettingsWidget::THEME_NAMES,
                                               InterfaceSettingsWidget::THEME_VALUES,
                                               InterfaceSettingsWidget::DEFAULT_THEME_NAME, "InterfaceSettingsWidget");
  connect(m_ui.theme, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SetupWizardDialog::themeChanged);

  InterfaceSettingsWidget::populateLanguageDropdown(m_ui.language);
  SettingWidgetBinder::BindWidgetToStringSetting(nullptr, m_ui.language, "Main", "Language",
                                                 QtHost::GetDefaultLanguage());
  connect(m_ui.language, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SetupWizardDialog::languageChanged);

  SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.autoUpdateEnabled, "AutoUpdater", "CheckAtStartup", true);
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

  QMenu menu;
  menu.addAction(tr("Remove"), [this]() { onRemoveSearchDirectoryButtonClicked(); });
  menu.addSeparator();
  menu.addAction(tr("Open Directory..."), [this, row]() {
    QtUtils::OpenURL(this, QUrl::fromLocalFile(m_ui.searchDirectoryList->item(row, 0)->text()));
  });
  menu.exec(m_ui.searchDirectoryList->mapToGlobal(point));
}

void SetupWizardDialog::onAddSearchDirectoryButtonClicked()
{
  QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select Search Directory")));

  if (dir.isEmpty())
    return;

  QMessageBox::StandardButton selection =
    QMessageBox::question(this, tr("Scan Recursively?"),
                          tr("Would you like to scan the directory \"%1\" recursively?\n\nScanning recursively takes "
                             "more time, but will identify files in subdirectories.")
                            .arg(dir),
                          QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
  if (selection == QMessageBox::Cancel)
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

void SetupWizardDialog::resizeDirectoryListColumns()
{
  QtUtils::ResizeColumnsForTableView(m_ui.searchDirectoryList, {-1, 100});
}

void SetupWizardDialog::setupControllerPage(bool initial)
{
  static constexpr u32 NUM_PADS = 2;

  struct PadWidgets
  {
    QComboBox* type_combo;
    QLabel* mapping_result;
    QToolButton* mapping_button;
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
  QMenu menu(this);
  bool added = false;

  for (const InputDeviceListModel::Device& dev : g_emu_thread->getInputDeviceListModel()->getDeviceList())
  {
    // we set it as data, because the device list could get invalidated while the menu is up
    QAction* action = menu.addAction(QStringLiteral("%1 (%2)").arg(dev.identifier).arg(dev.display_name));
    action->setIcon(InputDeviceListModel::getIconForKey(dev.key));
    action->setData(dev.identifier);
    connect(action, &QAction::triggered, this, [this, port, update_label, action]() {
      doDeviceAutomaticBinding(port, update_label, action->data().toString());
    });
    added = true;
  }

  if (added)
  {
    QAction* action = menu.addAction(tr("Multiple Devices..."));
    connect(action, &QAction::triggered, this,
            [this, port, update_label]() { doMultipleDeviceAutomaticBinding(port, update_label); });
  }
  else
  {
    QAction* action = menu.addAction(tr("No devices available"));
    action->setEnabled(false);
  }

  menu.exec(QCursor::pos());
}

void SetupWizardDialog::doDeviceAutomaticBinding(u32 port, QLabel* update_label, const QString& device)
{
  std::vector<std::pair<GenericInputBinding, std::string>> mapping =
    InputManager::GetGenericBindingMapping(device.toStdString());
  if (mapping.empty())
  {
    QMessageBox::critical(
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
  if (!ControllerBindingWidget::doMultipleDeviceAutomaticBinding(this, nullptr, port))
    return;

  update_label->setText(findCurrentDeviceForPort(port));
}

void SetupWizardDialog::setupGraphicsPage(bool initial)
{
  m_ui.renderer->disconnect();
  m_ui.renderer->clear();

  for (u32 i = 0; i < static_cast<u32>(GPURenderer::Count); i++)
  {
    m_ui.renderer->addItem(QString::fromUtf8(Settings::GetRendererDisplayName(static_cast<GPURenderer>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.renderer, "GPU", "Renderer", &Settings::ParseRendererName,
                                               &Settings::GetRendererName, Settings::DEFAULT_GPU_RENDERER);

  m_ui.resolutionScale->disconnect();
  m_ui.resolutionScale->clear();
  GraphicsSettingsWidget::populateUpscalingModes(m_ui.resolutionScale, 16);
  SettingWidgetBinder::BindWidgetToIntSetting(nullptr, m_ui.resolutionScale, "GPU", "ResolutionScale", 1);

  m_ui.textureFiltering->disconnect();
  m_ui.textureFiltering->clear();
  m_ui.spriteTextureFiltering->disconnect();
  m_ui.spriteTextureFiltering->clear();

  for (u32 i = 0; i < static_cast<u32>(GPUTextureFilter::Count); i++)
  {
    m_ui.textureFiltering->addItem(
      QString::fromUtf8(Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i))));
    m_ui.spriteTextureFiltering->addItem(
      QString::fromUtf8(Settings::GetTextureFilterDisplayName(static_cast<GPUTextureFilter>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.textureFiltering, "GPU", "TextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER);
  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.spriteTextureFiltering, "GPU", "SpriteTextureFilter",
                                               &Settings::ParseTextureFilterName, &Settings::GetTextureFilterName,
                                               Settings::DEFAULT_GPU_TEXTURE_FILTER);

  m_ui.gpuDitheringMode->disconnect();
  m_ui.gpuDitheringMode->clear();

  for (u32 i = 0; i < static_cast<u32>(GPUDitheringMode::MaxCount); i++)
  {
    m_ui.gpuDitheringMode->addItem(
      QString::fromUtf8(Settings::GetGPUDitheringModeDisplayName(static_cast<GPUDitheringMode>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.gpuDitheringMode, "GPU", "DitheringMode",
                                               &Settings::ParseGPUDitheringModeName, &Settings::GetGPUDitheringModeName,
                                               Settings::DEFAULT_GPU_DITHERING_MODE);

  m_ui.displayAspectRatio->disconnect();
  m_ui.displayAspectRatio->clear();

  for (u32 i = 0; i < static_cast<u32>(DisplayAspectRatio::Count); i++)
  {
    m_ui.displayAspectRatio->addItem(
      QString::fromUtf8(Settings::GetDisplayAspectRatioDisplayName(static_cast<DisplayAspectRatio>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.displayAspectRatio, "Display", "AspectRatio",
                                               &Settings::ParseDisplayAspectRatio, &Settings::GetDisplayAspectRatioName,
                                               Settings::DEFAULT_DISPLAY_ASPECT_RATIO);
  SettingWidgetBinder::BindWidgetToIntSetting(nullptr, m_ui.customAspectRatioNumerator, "Display",
                                              "CustomAspectRatioNumerator", 1);
  SettingWidgetBinder::BindWidgetToIntSetting(nullptr, m_ui.customAspectRatioDenominator, "Display",
                                              "CustomAspectRatioDenominator", 1);
  connect(m_ui.displayAspectRatio, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &SetupWizardDialog::onGraphicsAspectRatioChanged);
  onGraphicsAspectRatioChanged();

  m_ui.displayCropMode->disconnect();
  m_ui.displayCropMode->clear();

  for (u32 i = 0; i < static_cast<u32>(DisplayCropMode::MaxCount); i++)
  {
    m_ui.displayCropMode->addItem(
      QString::fromUtf8(Settings::GetDisplayCropModeDisplayName(static_cast<DisplayCropMode>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.displayCropMode, "Display", "CropMode",
                                               &Settings::ParseDisplayCropMode, &Settings::GetDisplayCropModeName,
                                               Settings::DEFAULT_DISPLAY_CROP_MODE);

  m_ui.displayScaling->disconnect();
  m_ui.displayScaling->clear();

  for (u32 i = 0; i < static_cast<u32>(DisplayScalingMode::Count); i++)
  {
    m_ui.displayScaling->addItem(
      QString::fromUtf8(Settings::GetDisplayScalingDisplayName(static_cast<DisplayScalingMode>(i))));
  }

  SettingWidgetBinder::BindWidgetToEnumSetting(nullptr, m_ui.displayScaling, "Display", "Scaling",
                                               &Settings::ParseDisplayScaling, &Settings::GetDisplayScalingName,
                                               Settings::DEFAULT_DISPLAY_SCALING);

  if (initial)
  {
    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.pgxpEnable, "GPU", "PGXPEnable", false);
    SettingWidgetBinder::BindWidgetToBoolSetting(nullptr, m_ui.widescreenHack, "GPU", "WidescreenHack", false);
  }
}

void SetupWizardDialog::onGraphicsAspectRatioChanged()
{
  const DisplayAspectRatio ratio =
    Settings::ParseDisplayAspectRatio(
      Host::GetBaseStringSettingValue("Display", "AspectRatio",
                                      Settings::GetDisplayAspectRatioName(Settings::DEFAULT_DISPLAY_ASPECT_RATIO))
        .c_str())
      .value_or(Settings::DEFAULT_DISPLAY_ASPECT_RATIO);

  const bool is_custom = (ratio == DisplayAspectRatio::Custom);

  m_ui.customAspectRatioNumerator->setVisible(is_custom);
  m_ui.customAspectRatioDenominator->setVisible(is_custom);
  m_ui.customAspectRatioSeparator->setVisible(is_custom);
}

void SetupWizardDialog::setupAchievementsPage(bool initial)
{
  if (initial)
  {
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
    const QDateTime login_timestamp(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(login_unix_timestamp)));
    m_ui.loginStatus->setText(tr("Username: %1\nLogin token generated on %2.")
                                .arg(QString::fromStdString(username))
                                .arg(login_timestamp.toString(Qt::TextDate)));
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

  AchievementLoginDialog login(this, Achievements::LoginRequestReason::UserInitiated);
  int res = login.exec();
  if (res != 0)
    return;

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
