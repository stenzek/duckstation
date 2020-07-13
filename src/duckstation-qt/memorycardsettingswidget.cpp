#include "memorycardsettingswidget.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"
#include <QtCore/QUrl>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>

static constexpr char MEMORY_CARD_IMAGE_FILTER[] = "All Memory Card Types (*.mcd *.mcr *.mc)";

MemoryCardSettingsWidget::MemoryCardSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  createUi();
}

MemoryCardSettingsWidget::~MemoryCardSettingsWidget() = default;

void MemoryCardSettingsWidget::createUi()
{
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
  {
    createPortSettingsUi(i, &m_port_ui[i]);
    layout->addWidget(m_port_ui[i].container);
  }

  {
    QGroupBox* note_box = new QGroupBox(this);
    QHBoxLayout* note_layout = new QHBoxLayout(note_box);
    QLabel* note_label =
      new QLabel(tr("If one of the \"separate card per game\" memory card modes is chosen, these memory "
                    "cards will be saved to the memcards directory."),
                 note_box);
    note_label->setWordWrap(true);
    note_layout->addWidget(note_label, 1);

    QPushButton* open_memcards = new QPushButton(tr("Open..."), note_box);
    connect(open_memcards, &QPushButton::clicked, this, &MemoryCardSettingsWidget::onOpenMemCardsDirectoryClicked);
    note_layout->addWidget(open_memcards);
    layout->addWidget(note_box);
  }

  layout->addStretch(1);

  setLayout(layout);
}

void MemoryCardSettingsWidget::createPortSettingsUi(int index, PortSettingsUI* ui)
{
  ui->container = new QGroupBox(tr("Memory Card %1").arg(index + 1), this);
  ui->layout = new QVBoxLayout(ui->container);

  ui->memory_card_type = new QComboBox(ui->container);
  for (int i = 0; i < static_cast<int>(MemoryCardType::Count); i++)
  {
    ui->memory_card_type->addItem(
      QString::fromUtf8(Settings::GetMemoryCardTypeDisplayName(static_cast<MemoryCardType>(i))));
  }

  const MemoryCardType default_value = (index == 0) ? MemoryCardType::PerGameTitle : MemoryCardType::None;
  SettingWidgetBinder::BindWidgetToEnumSetting(
    m_host_interface, ui->memory_card_type, QStringLiteral("MemoryCards"), QStringLiteral("Card%1Type").arg(index + 1),
    &Settings::ParseMemoryCardTypeName, &Settings::GetMemoryCardTypeName, default_value);
  ui->layout->addWidget(new QLabel(tr("Memory Card Type:"), ui->container));
  ui->layout->addWidget(ui->memory_card_type);

  QHBoxLayout* memory_card_layout = new QHBoxLayout();
  ui->memory_card_path = new QLineEdit(ui->container);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, ui->memory_card_path,
                                                 QStringLiteral("MemoryCards"), QStringLiteral("Card%1Path").arg(index + 1));
  memory_card_layout->addWidget(ui->memory_card_path);

  QPushButton* memory_card_path_browse = new QPushButton(tr("Browse..."), ui->container);
  connect(memory_card_path_browse, &QPushButton::clicked, [this, index]() { onBrowseMemoryCardPathClicked(index); });
  memory_card_layout->addWidget(memory_card_path_browse);

  ui->layout->addWidget(new QLabel(tr("Shared Memory Card Path:"), ui->container));
  ui->layout->addLayout(memory_card_layout);

  ui->layout->addStretch(1);
}

void MemoryCardSettingsWidget::onBrowseMemoryCardPathClicked(int index)
{
  QString path =
    QFileDialog::getOpenFileName(this, tr("Select path to memory card image"), QString(), tr(MEMORY_CARD_IMAGE_FILTER));
  if (path.isEmpty())
    return;

  m_port_ui[index].memory_card_path->setText(path);
}

void MemoryCardSettingsWidget::onOpenMemCardsDirectoryClicked()
{
  QtUtils::OpenURL(this,
                   QUrl::fromLocalFile(m_host_interface->getUserDirectoryRelativePath(QStringLiteral("memcards"))));
}
