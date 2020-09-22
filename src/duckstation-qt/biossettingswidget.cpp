#include "biossettingswidget.h"
#include "core/bios.h"
#include "qthostinterface.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtWidgets/QFileDialog>
#include <algorithm>

static void populateDropDownForRegion(ConsoleRegion region, QComboBox* cb,
                                      std::vector<std::pair<std::string, const BIOS::ImageInfo*>>& images)
{
  cb->addItem(QIcon(QStringLiteral(":/icons/system-search.png")),
              QT_TRANSLATE_NOOP("BIOSSettingsWidget", "Auto-Detect"));

  std::sort(images.begin(), images.end(), [region](const auto& left, const auto& right) {
    const bool left_region_match = (left.second && left.second->region == region);
    const bool right_region_match = (right.second && right.second->region == region);
    if (left_region_match && !right_region_match)
      return true;
    else if (right_region_match && left_region_match)
      return false;

    return left.first < right.first;
  });

  for (const auto& [name, info] : images)
  {
    QIcon icon;
    if (info)
    {
      switch (info->region)
      {
        case ConsoleRegion::NTSC_J:
          icon = QIcon(QStringLiteral(":/icons/flag-jp.png"));
          break;
        case ConsoleRegion::PAL:
          icon = QIcon(QStringLiteral(":/icons/flag-eu.png"));
          break;
        case ConsoleRegion::NTSC_U:
          icon = QIcon(QStringLiteral(":/icons/flag-uc.png"));
          break;
        default:
          icon = QIcon(QStringLiteral(":/icons/applications-other.png"));
          break;
      }
    }
    else
    {
      icon = QIcon(QStringLiteral(":/icons/applications-other.png"));
    }

    QString name_str(QString::fromStdString(name));
    cb->addItem(icon,
                QStringLiteral("%1 (%2)")
                  .arg(info ? QString(info->description) : qApp->translate("BIOSSettingsWidget", "Unknown"))
                  .arg(name_str),
                QVariant(name_str));
  }
}

static void setDropDownValue(QComboBox* cb, const std::string& name)
{
  QSignalBlocker sb(cb);

  if (name.empty())
  {
    cb->setCurrentIndex(0);
    return;
  }

  QString qname(QString::fromStdString(name));
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

BIOSSettingsWidget::BIOSSettingsWidget(QtHostInterface* host_interface, QWidget* parent, SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  m_ui.setupUi(this);

  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.enableTTYOutput, "BIOS", "PatchTTYEnable");
  SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, m_ui.fastBoot, "BIOS", "PatchFastBoot");

  dialog->registerWidgetHelp(m_ui.fastBoot, tr("Fast Boot"), tr("Unchecked"),
                             tr("Patches the BIOS to skip the console's boot animation. Does not work with all games, "
                                "but usually safe to enabled."));

  auto images = m_host_interface->FindBIOSImagesInUserDirectory();
  populateDropDownForRegion(ConsoleRegion::NTSC_J, m_ui.imageNTSCJ, images);
  populateDropDownForRegion(ConsoleRegion::NTSC_U, m_ui.imageNTSCU, images);
  populateDropDownForRegion(ConsoleRegion::PAL, m_ui.imagePAL, images);

  setDropDownValue(m_ui.imageNTSCJ, m_host_interface->GetStringSettingValue("BIOS", "PathNTSCJ", ""));
  setDropDownValue(m_ui.imageNTSCU, m_host_interface->GetStringSettingValue("BIOS", "PathNTSCU", ""));
  setDropDownValue(m_ui.imagePAL, m_host_interface->GetStringSettingValue("BIOS", "PathPAL", ""));

  connect(m_ui.imageNTSCJ, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    m_host_interface->SetStringSettingValue("BIOS", "PathNTSCJ",
                                            m_ui.imageNTSCJ->itemData(index).toString().toStdString().c_str());
    m_host_interface->applySettings();
  });
  connect(m_ui.imageNTSCU, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    m_host_interface->SetStringSettingValue("BIOS", "PathNTSCU",
                                            m_ui.imageNTSCU->itemData(index).toString().toStdString().c_str());
    m_host_interface->applySettings();
  });
  connect(m_ui.imagePAL, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    m_host_interface->SetStringSettingValue("BIOS", "PathPAL",
                                            m_ui.imagePAL->itemData(index).toString().toStdString().c_str());
    m_host_interface->applySettings();
  });
}

BIOSSettingsWidget::~BIOSSettingsWidget() = default;
