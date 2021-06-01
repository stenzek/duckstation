#include "memorycardsettingswidget.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "mainwindow.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "settingsdialog.h"
#include "settingwidgetbinder.h"
#include <QtCore/QUrl>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>

static constexpr char MEMORY_CARD_IMAGE_FILTER[] =
  QT_TRANSLATE_NOOP("MemoryCardSettingsWidget", "All Memory Card Types (*.mcd *.mcr *.mc)");

MemoryCardSettingsWidget::MemoryCardSettingsWidget(QtHostInterface* host_interface, QWidget* parent,
                                                   SettingsDialog* dialog)
  : QWidget(parent), m_host_interface(host_interface)
{
  createUi(dialog);
}

MemoryCardSettingsWidget::~MemoryCardSettingsWidget() = default;

void MemoryCardSettingsWidget::createUi(SettingsDialog* dialog)
{
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
  {
    createPortSettingsUi(dialog, i, &m_port_ui[i]);
    layout->addWidget(m_port_ui[i].container);
  }

  {
    QGroupBox* box = new QGroupBox(tr("Shared Settings"), this);
    QVBoxLayout* box_layout = new QVBoxLayout(box);

    {
      QLabel* label = new QLabel(tr("Memory Card Directory:"), box);
      box_layout->addWidget(label);

      QHBoxLayout* hbox = new QHBoxLayout();
      m_memory_card_directory = new QLineEdit(box);
      SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, m_memory_card_directory, "MemoryCards",
                                                     "Directory");
      if (m_memory_card_directory->text().isEmpty())
      {
        QSignalBlocker sb(m_memory_card_directory);
        m_memory_card_directory->setText(QString::fromStdString(m_host_interface->GetMemoryCardDirectory()));
      }
      hbox->addWidget(m_memory_card_directory);

      QPushButton* browse = new QPushButton(tr("Browse..."), box);
      connect(browse, &QPushButton::clicked, this, &MemoryCardSettingsWidget::onBrowseMemCardsDirectoryClicked);
      hbox->addWidget(browse);

      QPushButton* reset = new QPushButton(tr("Reset"), box);
      connect(reset, &QPushButton::clicked, this, &MemoryCardSettingsWidget::onResetMemCardsDirectoryClicked);
      hbox->addWidget(reset);

      box_layout->addLayout(hbox);
    }

    QCheckBox* playlist_title_as_game_title = new QCheckBox(tr("Use Single Card For Sub-Images"), box);
    SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, playlist_title_as_game_title, "MemoryCards",
                                                 "UsePlaylistTitle", true);
    box_layout->addWidget(playlist_title_as_game_title);
    dialog->registerWidgetHelp(
      playlist_title_as_game_title, tr("Use Single Card For Sub-Images"), tr("Checked"),
      tr("When using a multi-disc format (m3u/pbp) and per-game (title) memory cards, a single memory card "
         "will be used for all discs. If unchecked, a separate card will be used for each disc."));

    box_layout->addWidget(QtUtils::CreateHorizontalLine(box));

    {

      QHBoxLayout* note_layout = new QHBoxLayout();
      QLabel* note_label =
        new QLabel(tr("If one of the \"separate card per game\" memory card types is chosen, these memory "
                      "cards will be saved to the memory cards directory."),
                   box);
      note_label->setWordWrap(true);
      note_layout->addWidget(note_label, 1);

      QPushButton* open_memcards = new QPushButton(tr("Open Directory..."), box);
      connect(open_memcards, &QPushButton::clicked, this, &MemoryCardSettingsWidget::onOpenMemCardsDirectoryClicked);
      note_layout->addWidget(open_memcards);
      box_layout->addLayout(note_layout);
    }

    {
      QHBoxLayout* hbox = new QHBoxLayout();
      QLabel* label = new QLabel(
        tr("The memory card editor enables you to move saves between cards, as well as import cards of other formats."),
        box);
      label->setWordWrap(true);
      hbox->addWidget(label, 1);

      QPushButton* button = new QPushButton(tr("Memory Card Editor..."), box);
      connect(button, &QPushButton::clicked,
              []() { QtHostInterface::GetInstance()->getMainWindow()->openMemoryCardEditor(QString(), QString()); });
      hbox->addWidget(button);
      box_layout->addLayout(hbox);
    }

    layout->addWidget(box);
  }

  layout->addStretch(1);

  setLayout(layout);
}

void MemoryCardSettingsWidget::createPortSettingsUi(SettingsDialog* dialog, int index, PortSettingsUI* ui)
{
  ui->container = new QGroupBox(tr("Memory Card %1").arg(index + 1), this);
  ui->layout = new QVBoxLayout(ui->container);

  ui->memory_card_type = new QComboBox(ui->container);
  for (int i = 0; i < static_cast<int>(MemoryCardType::Count); i++)
  {
    ui->memory_card_type->addItem(
      qApp->translate("MemoryCardType", Settings::GetMemoryCardTypeDisplayName(static_cast<MemoryCardType>(i))));
  }

  const MemoryCardType default_value = (index == 0) ? MemoryCardType::PerGameTitle : MemoryCardType::None;
  SettingWidgetBinder::BindWidgetToEnumSetting(
    m_host_interface, ui->memory_card_type, "MemoryCards", StringUtil::StdStringFromFormat("Card%dType", index + 1),
    &Settings::ParseMemoryCardTypeName, &Settings::GetMemoryCardTypeName, default_value);
  ui->layout->addWidget(new QLabel(tr("Memory Card Type:"), ui->container));
  ui->layout->addWidget(ui->memory_card_type);

  QHBoxLayout* memory_card_layout = new QHBoxLayout();
  ui->memory_card_path = new QLineEdit(ui->container);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, ui->memory_card_path, "MemoryCards",
                                                 StringUtil::StdStringFromFormat("Card%dPath", index + 1));
  if (ui->memory_card_path->text().isEmpty())
  {
    QSignalBlocker sb(ui->memory_card_path);
    ui->memory_card_path->setText(
      QString::fromStdString(m_host_interface->GetSharedMemoryCardPath(static_cast<u32>(index))));
  }
  memory_card_layout->addWidget(ui->memory_card_path);

  QPushButton* memory_card_path_browse = new QPushButton(tr("Browse..."), ui->container);
  connect(memory_card_path_browse, &QPushButton::clicked, [this, index]() { onBrowseMemoryCardPathClicked(index); });
  memory_card_layout->addWidget(memory_card_path_browse);

  QPushButton* memory_card_path_reset = new QPushButton(tr("Reset"), ui->container);
  connect(memory_card_path_reset, &QPushButton::clicked, [this, index]() { onResetMemoryCardPathClicked(index); });
  memory_card_layout->addWidget(memory_card_path_reset);

  ui->layout->addWidget(new QLabel(tr("Shared Memory Card Path:"), ui->container));
  ui->layout->addLayout(memory_card_layout);

  ui->layout->addStretch(1);
}

void MemoryCardSettingsWidget::onBrowseMemoryCardPathClicked(int index)
{
  QString path = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select path to memory card image"),
                                                                       QString(), tr(MEMORY_CARD_IMAGE_FILTER)));
  if (path.isEmpty())
    return;

  m_port_ui[index].memory_card_path->setText(path);
}

void MemoryCardSettingsWidget::onResetMemoryCardPathClicked(int index)
{
  m_host_interface->RemoveSettingValue("MemoryCards", TinyString::FromFormat("Card%dPath", index + 1));
  m_host_interface->applySettings();

  QSignalBlocker db(m_port_ui[index].memory_card_path);
  m_port_ui[index].memory_card_path->setText(QString::fromStdString(m_host_interface->GetSharedMemoryCardPath(index)));
}

void MemoryCardSettingsWidget::onOpenMemCardsDirectoryClicked()
{
  QtUtils::OpenURL(this, QUrl::fromLocalFile(m_memory_card_directory->text()));
}

void MemoryCardSettingsWidget::onBrowseMemCardsDirectoryClicked()
{
  QString path =
    QDir::toNativeSeparators(QFileDialog::getExistingDirectory(this, tr("Select path to memory card directory")));
  if (path.isEmpty())
    return;

  m_memory_card_directory->setText(path);
  m_host_interface->applySettings();
}

void MemoryCardSettingsWidget::onResetMemCardsDirectoryClicked()
{
  m_host_interface->RemoveSettingValue("MemoryCards", "Directory");
  m_host_interface->applySettings();

  // This sucks.. settings are applied asynchronously, so we have to manually build the path here.
  QString memory_card_directory(m_host_interface->getUserDirectoryRelativePath(QStringLiteral("memcards")));
  QSignalBlocker db(m_memory_card_directory);
  m_memory_card_directory->setText(memory_card_directory);
}
