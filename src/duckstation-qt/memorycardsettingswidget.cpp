// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "memorycardsettingswidget.h"

#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "common/small_string.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtCore/QUrl>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QVBoxLayout>

#include <functional>

#include "moc_memorycardsettingswidget.cpp"

static constexpr char MEMORY_CARD_IMAGE_FILTER[] =
  QT_TRANSLATE_NOOP("MemoryCardSettingsWidget", "All Memory Card Types (*.mcd *.mcr *.mc)");

MemoryCardSettingsWidget::MemoryCardSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  createUi(dialog);
}

MemoryCardSettingsWidget::~MemoryCardSettingsWidget() = default;

void MemoryCardSettingsWidget::createUi(SettingsWindow* dialog)
{
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
  {
    createPortSettingsUi(dialog, i, &m_port_ui[i]);
    layout->addWidget(m_port_ui[i].container);
    onMemoryCardTypeChanged(i);
  }

  {
    QGroupBox* box = new QGroupBox(tr("Game-Specific Card Settings"), this);
    QVBoxLayout* box_layout = new QVBoxLayout(box);
    QPushButton* browse = new QPushButton(tr("Browse..."), box);
    QPushButton* open_memcards = new QPushButton(tr("Open..."), box);
    QPushButton* reset = new QPushButton(tr("Reset"), box);

    {
      QLabel* label = new QLabel(tr("Memory Card Directory:"), box);
      box_layout->addWidget(label);

      QHBoxLayout* hbox = new QHBoxLayout();
      m_memory_card_directory = new QLineEdit(box);

      hbox->addWidget(m_memory_card_directory);
      hbox->addWidget(browse);
      hbox->addWidget(open_memcards);
      hbox->addWidget(reset);

      box_layout->addLayout(hbox);
    }

    QCheckBox* playlist_title_as_game_title = new QCheckBox(tr("Use Single Card For Multi-Disc Games"), box);
    SettingWidgetBinder::BindWidgetToBoolSetting(m_dialog->getSettingsInterface(), playlist_title_as_game_title,
                                                 "MemoryCards", "UsePlaylistTitle", true);
    box_layout->addWidget(playlist_title_as_game_title);
    dialog->registerWidgetHelp(
      playlist_title_as_game_title, tr("Use Single Card For Multi-Disc Games"), tr("Checked"),
      tr("When playing a multi-disc game and using per-game (title) memory cards, a single memory card "
         "will be used for all discs. If unchecked, a separate card will be used for each disc."));

    box_layout->addWidget(QtUtils::CreateHorizontalLine(box));

    {
      QHBoxLayout* hbox = new QHBoxLayout();
      QLabel* label = new QLabel(
        tr("The memory card editor enables you to move saves between cards, as well as import cards of other formats."),
        box);
      label->setWordWrap(true);
      hbox->addWidget(label, 1);

      QPushButton* button = new QPushButton(tr("Memory Card Editor..."), box);
      connect(button, &QPushButton::clicked, []() { g_main_window->openMemoryCardEditor(QString(), QString()); });
      hbox->addWidget(button);
      box_layout->addLayout(hbox);
    }

    layout->addWidget(box);

    SettingWidgetBinder::BindWidgetToFolderSetting(
      m_dialog->getSettingsInterface(), m_memory_card_directory, browse, tr("Select Memory Card Directory"),
      open_memcards, reset, "MemoryCards", "Directory", Path::Combine(EmuFolders::DataRoot, "memcards"));
  }

  layout->addStretch(1);

  setLayout(layout);
}

void MemoryCardSettingsWidget::createPortSettingsUi(SettingsWindow* dialog, int index, PortSettingsUI* ui)
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
  SettingWidgetBinder::BindWidgetToEnumSetting(m_dialog->getSettingsInterface(), ui->memory_card_type, "MemoryCards",
                                               fmt::format("Card{}Type", index + 1), &Settings::ParseMemoryCardTypeName,
                                               &Settings::GetMemoryCardTypeName, default_value);
  connect(ui->memory_card_type, &QComboBox::currentIndexChanged, this,
          std::bind(&MemoryCardSettingsWidget::onMemoryCardTypeChanged, this, index));
  ui->layout->addWidget(new QLabel(tr("Memory Card Type:"), ui->container));
  ui->layout->addWidget(ui->memory_card_type);

  QHBoxLayout* memory_card_layout = new QHBoxLayout();
  ui->memory_card_path = new QLineEdit(ui->container);
  updateMemoryCardPath(index);
  connect(ui->memory_card_path, &QLineEdit::textChanged, this, [this, index]() { onMemoryCardPathChanged(index); });
  if (ui->memory_card_path->text().isEmpty())
  {
    QSignalBlocker sb(ui->memory_card_path);
    ui->memory_card_path->setText(QString::fromStdString(g_settings.GetSharedMemoryCardPath(static_cast<u32>(index))));
  }
  memory_card_layout->addWidget(ui->memory_card_path);

  ui->memory_card_path_browse = new QPushButton(tr("Browse..."), ui->container);
  connect(ui->memory_card_path_browse, &QPushButton::clicked, this,
          [this, index]() { onBrowseMemoryCardPathClicked(index); });
  memory_card_layout->addWidget(ui->memory_card_path_browse);

  ui->memory_card_path_reset = new QPushButton(tr("Reset"), ui->container);
  connect(ui->memory_card_path_reset, &QPushButton::clicked, this,
          [this, index]() { onResetMemoryCardPathClicked(index); });
  memory_card_layout->addWidget(ui->memory_card_path_reset);

  ui->memory_card_path_label = new QLabel(tr("Shared Memory Card Path:"), ui->container);
  ui->layout->addWidget(ui->memory_card_path_label);
  ui->layout->addLayout(memory_card_layout);

  ui->layout->addStretch(1);
}

void MemoryCardSettingsWidget::onMemoryCardTypeChanged(int index)
{
  const MemoryCardType default_type =
    (index == 0) ? Settings::DEFAULT_MEMORY_CARD_1_TYPE : Settings::DEFAULT_MEMORY_CARD_2_TYPE;
  const MemoryCardType type =
    Settings::ParseMemoryCardTypeName(m_dialog
                                        ->getEffectiveStringValue("MemoryCards",
                                                                  TinyString::from_format("Card{}Type", index + 1),
                                                                  Settings::GetMemoryCardTypeName(default_type))
                                        .c_str())
      .value_or(default_type);
  const bool shared_enabled = (type == MemoryCardType::Shared);
  m_port_ui[index].memory_card_path_label->setEnabled(shared_enabled);
  m_port_ui[index].memory_card_path->setEnabled(shared_enabled);
  m_port_ui[index].memory_card_path_browse->setEnabled(shared_enabled);
  m_port_ui[index].memory_card_path_reset->setEnabled(shared_enabled);
}

void MemoryCardSettingsWidget::onBrowseMemoryCardPathClicked(int index)
{
  QString path = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select path to memory card image"),
                                                                       QString(), tr(MEMORY_CARD_IMAGE_FILTER)));
  if (path.isEmpty())
    return;

  m_port_ui[index].memory_card_path->setText(path);
}

void MemoryCardSettingsWidget::onMemoryCardPathChanged(int index)
{
  const auto key = TinyString::from_format("Card{}Path", index + 1);
  std::string relative_path(
    Path::MakeRelative(m_port_ui[index].memory_card_path->text().toStdString(), EmuFolders::MemoryCards));
  m_dialog->setStringSettingValue("MemoryCards", key, relative_path.c_str());
}

void MemoryCardSettingsWidget::onResetMemoryCardPathClicked(int index)
{
  const auto key = TinyString::from_format("Card{}Path", index + 1);
  if (m_dialog->isPerGameSettings())
    m_dialog->removeSettingValue("MemoryCards", key);
  else
    m_dialog->setStringSettingValue("MemoryCards", key, Settings::GetDefaultSharedMemoryCardName(index).c_str());

  updateMemoryCardPath(index);
}

void MemoryCardSettingsWidget::updateMemoryCardPath(int index)
{
  const auto key = TinyString::from_format("Card{}Path", index + 1);
  std::string path(
    m_dialog->getEffectiveStringValue("MemoryCards", key, Settings::GetDefaultSharedMemoryCardName(index).c_str()));
  if (!Path::IsAbsolute(path))
    path = Path::Canonicalize(Path::Combine(EmuFolders::MemoryCards, path));

  QSignalBlocker db(m_port_ui[index].memory_card_path);
  m_port_ui[index].memory_card_path->setText(QString::fromStdString(path));
}
