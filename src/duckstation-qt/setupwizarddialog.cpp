// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "setupwizarddialog.h"
#include "controllerbindingwidgets.h"
#include "controllersettingwidgetbinder.h"
#include "interfacesettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"

#include "core/controller.h"

#include "util/input_manager.h"

#include "common/file_system.h"

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
  m_page_labels[Page_Complete] = m_ui.labelComplete;

  connect(m_ui.back, &QPushButton::clicked, this, &SetupWizardDialog::previousPage);
  connect(m_ui.next, &QPushButton::clicked, this, &SetupWizardDialog::nextPage);
  connect(m_ui.cancel, &QPushButton::clicked, this, &SetupWizardDialog::confirmCancel);

  setupLanguagePage();
  setupBIOSPage();
  setupGameListPage();
  setupControllerPage(true);
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
      w.type_combo->addItem(QString::fromUtf8(cinfo->GetDisplayName()), QString::fromUtf8(cinfo->name));

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
