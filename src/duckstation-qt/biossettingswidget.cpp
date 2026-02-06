// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "biossettingswidget.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/bios.h"
#include "core/settings.h"

#include <QtCore/QDir>
#include <QtWidgets/QFileDialog>
#include <algorithm>

#include "moc_biossettingswidget.cpp"

using namespace Qt::StringLiterals;

BIOSSettingsWidget::BIOSSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent), m_dialog(dialog)
{
  SettingsInterface* sif = dialog->getSettingsInterface();

  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.enableTTYLogging, "BIOS", "TTYLogging", false);

  SettingWidgetBinder::BindWidgetToEnumSetting(sif, m_ui.pioDeviceType, "PIO", "DeviceType",
                                               &Settings::ParsePIODeviceTypeName, &Settings::GetPIODeviceTypeModeName,
                                               &Settings::GetPIODeviceTypeModeDisplayName,
                                               Settings::DEFAULT_PIO_DEVICE_TYPE, PIODeviceType::MaxCount);
  SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.pioImagePath, "PIO", "FlashImagePath");
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pioSwitchActive, "PIO", "SwitchActive", true);
  SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.pioImageWrites, "PIO", "FlashImageWriteEnable", false);
  connect(m_ui.pioDeviceType, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &BIOSSettingsWidget::onPIODeviceTypeChanged);
  connect(m_ui.pioImagePathBrowse, &QPushButton::clicked, this, &BIOSSettingsWidget::onPIOImagePathBrowseClicked);
  onPIODeviceTypeChanged();

  connect(m_ui.imageNTSCJ, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (m_dialog->isPerGameSettings() && index == 0)
    {
      m_dialog->removeSettingValue("BIOS", "PathNTSCJ");
    }
    else
    {
      m_dialog->setStringSettingValue("BIOS", "PathNTSCJ",
                                      m_ui.imageNTSCJ->itemData(index).toString().toStdString().c_str());
    }
  });
  connect(m_ui.imageNTSCU, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (m_dialog->isPerGameSettings() && index == 0)
    {
      m_dialog->removeSettingValue("BIOS", "PathNTSCU");
    }
    else
    {
      m_dialog->setStringSettingValue("BIOS", "PathNTSCU",
                                      m_ui.imageNTSCU->itemData(index).toString().toStdString().c_str());
    }
  });
  connect(m_ui.imagePAL, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (m_dialog->isPerGameSettings() && index == 0)
    {
      m_dialog->removeSettingValue("BIOS", "PathPAL");
    }
    else
    {
      m_dialog->setStringSettingValue("BIOS", "PathPAL",
                                      m_ui.imagePAL->itemData(index).toString().toStdString().c_str());
    }
  });

  connect(m_ui.rescan, &QPushButton::clicked, this, &BIOSSettingsWidget::refreshList);

  if (!m_dialog->isPerGameSettings())
  {
    SettingWidgetBinder::BindWidgetToFolderSetting(
      sif, m_ui.searchDirectory, m_ui.browseSearchDirectory, tr("Select BIOS Directory"), m_ui.searchDirectoryOpen,
      m_ui.searchDirectoryReset, "BIOS", "SearchDirectory", Path::Combine(EmuFolders::DataRoot, "bios"));
    connect(m_ui.searchDirectory, &QLineEdit::textChanged, this, &BIOSSettingsWidget::refreshList);
  }
  else
  {
    m_ui.mainLayout->removeWidget(m_ui.directoryGroupBox);
    delete m_ui.directoryGroupBox;
    m_ui.directoryGroupBox = nullptr;
    m_ui.directoryGroupBoxLabel = nullptr;
    m_ui.directoryGroupBoxLayout = nullptr;
    m_ui.directoryGroupBoxHorizontalLayout = nullptr;
    m_ui.searchDirectory = nullptr;
    m_ui.browseSearchDirectory = nullptr;
  }

  refreshList();

  dialog->registerWidgetHelp(m_ui.pioDeviceType, tr("Device Type"), tr("None"),
                             tr("Simulates a device plugged into the console's parallel port. Usually these are flash "
                                "cartridges, and require some sort of image dump to function."));
  dialog->registerWidgetHelp(m_ui.pioImagePath, tr("Image Path"), tr("Empty"),
                             tr("Sets the path to the image used for flash cartridges."));
  dialog->registerWidgetHelp(m_ui.pioSwitchActive, tr("Cartridge Switch On"), tr("Checked"),
                             tr("Simulates the position of the switch on the cartridge. Most cartridges require the "
                                "switch to be on for it to activate on startup."));
  dialog->registerWidgetHelp(
    m_ui.pioImageWrites, tr("Allow Image Writes"), tr("Unchecked"),
    tr("Stores any images made to the cartridge's flash storage back to the host's file system. <strong>This will "
       "overwrite your cartridge dump,</strong> you should ensure you have a backup first."));
  dialog->registerWidgetHelp(m_ui.enableTTYLogging, tr("Enable TTY Logging"), tr("Unchecked"),
                             tr("Logs BIOS calls to printf(). Not all games contain debugging messages."));
}

BIOSSettingsWidget::~BIOSSettingsWidget() = default;

void BIOSSettingsWidget::refreshList()
{
  auto images = BIOS::FindBIOSImagesInDirectory(EmuFolders::Bios.c_str());
  populateDropDownForRegion(ConsoleRegion::NTSC_J, m_ui.imageNTSCJ, images, m_dialog->isPerGameSettings());
  populateDropDownForRegion(ConsoleRegion::NTSC_U, m_ui.imageNTSCU, images, m_dialog->isPerGameSettings());
  populateDropDownForRegion(ConsoleRegion::PAL, m_ui.imagePAL, images, m_dialog->isPerGameSettings());

  setDropDownValue(m_ui.imageNTSCJ, m_dialog->getStringValue("BIOS", "PathNTSCJ", std::nullopt),
                   m_dialog->isPerGameSettings());
  setDropDownValue(m_ui.imageNTSCU, m_dialog->getStringValue("BIOS", "PathNTSCU", std::nullopt),
                   m_dialog->isPerGameSettings());
  setDropDownValue(m_ui.imagePAL, m_dialog->getStringValue("BIOS", "PathPAL", std::nullopt),
                   m_dialog->isPerGameSettings());
}

void BIOSSettingsWidget::populateDropDownForRegion(ConsoleRegion region, QComboBox* cb,
                                                   std::vector<std::pair<std::string, const BIOS::ImageInfo*>>& images,
                                                   bool per_game)
{
  QSignalBlocker sb(cb);
  cb->clear();

  if (per_game)
    cb->addItem(QIcon(":/icons/system-search.png"_L1), tr("Use Global Setting"));

  cb->addItem(QIcon(":/icons/system-search.png"_L1), tr("Auto-Detect"));

  std::sort(images.begin(), images.end(), [region](const auto& left, const auto& right) {
    const bool left_region_match = (left.second && left.second->region == region);
    const bool right_region_match = (right.second && right.second->region == region);
    if (left_region_match && !right_region_match)
      return true;
    else if (right_region_match && !left_region_match)
      return false;

    return left.first < right.first;
  });

  for (const auto& [name, info] : images)
  {
    QString name_str(QString::fromStdString(name));
    cb->addItem(QtUtils::GetIconForRegion(info ? info->region : ConsoleRegion::Count),
                QStringLiteral("%1 (%2)")
                  .arg(info ? QString(info->description) : qApp->translate("BIOSSettingsWidget", "Unknown"))
                  .arg(name_str),
                QVariant(name_str));
  }
}

void BIOSSettingsWidget::setDropDownValue(QComboBox* cb, const std::optional<std::string>& name, bool per_game)
{
  QSignalBlocker sb(cb);

  if (!name.has_value() || name->empty())
  {
    cb->setCurrentIndex((per_game && name.has_value()) ? 1 : 0);
    return;
  }

  QString qname(QString::fromStdString(name.value()));
  for (int i = 1; i < cb->count(); i++)
  {
    if (cb->itemData(i) == qname)
    {
      cb->setCurrentIndex(i);
      return;
    }
  }

  cb->addItem(qname, QVariant(qname));
  cb->setCurrentIndex(cb->count() - 1);
}

void BIOSSettingsWidget::onPIODeviceTypeChanged()
{
  const PIODeviceType type =
    Settings::ParsePIODeviceTypeName(
      m_dialog
        ->getEffectiveStringValue("PIO", "DeviceType",
                                  Settings::GetPIODeviceTypeModeName(Settings::DEFAULT_PIO_DEVICE_TYPE))
        .c_str())
      .value_or(Settings::DEFAULT_PIO_DEVICE_TYPE);
  const bool has_image = (type == PIODeviceType::XplorerCart);
  const bool has_switch = (type == PIODeviceType::XplorerCart);
  m_ui.pioImagePathLabel->setEnabled(has_image);
  m_ui.pioImagePath->setEnabled(has_image);
  m_ui.pioImagePathBrowse->setEnabled(has_image);
  m_ui.pioImageWrites->setEnabled(has_image);
  m_ui.pioSwitchActive->setEnabled(has_switch);
}

void BIOSSettingsWidget::onPIOImagePathBrowseClicked()
{
  const QString path =
    QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select PIO Image"), m_ui.pioImagePath->text()));
  if (path.isEmpty())
    return;

  m_ui.pioImagePath->setText(path);
}
