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
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QVBoxLayout>

#include <functional>

#include "moc_memorycardsettingswidget.cpp"

using namespace Qt::StringLiterals;

static constexpr char MEMORY_CARD_IMAGE_FILTER[] =
  QT_TRANSLATE_NOOP("MemoryCardSettingsWidget", "All Memory Card Types (*.mcd *.mcr *.mc)");

MemoryCardSettingsWidget::MemoryCardSettingsWidget(SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog)
{
  createUi();
}

MemoryCardSettingsWidget::~MemoryCardSettingsWidget() = default;

void MemoryCardSettingsWidget::createUi()
{
  QVBoxLayout* const layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  {
    QGroupBox* const box = new QGroupBox(this);
    QGridLayout* const box_layout = new QGridLayout(box);
    layout->addWidget(box);

    QLabel* const icon = new QLabel(box);
    icon->setFixedSize(32, 32);
    icon->setPixmap(QIcon(u":/icons/monochrome/svg/memcards-line.svg"_s).pixmap(32));
    box_layout->addWidget(icon, 0, 0, 2, 1);
    QLabel* const label =
      new QLabel(tr("The number of memory cards that can be used is dependent on multitap and game support."), box);
    label->setWordWrap(true);
    box_layout->addWidget(label, 0, 1);
    m_multitap_label = new QLabel(this);
    box_layout->addWidget(m_multitap_label, 1, 1);
  }

  m_port_tabs = new QTabWidget(this);
  m_port_tabs->setDocumentMode(true);
  layout->addWidget(m_port_tabs);
  createPortSettings(m_dialog->getEffectiveMultitapMode());

  {
    QGroupBox* const box = new QGroupBox(tr("Save Locations"), this);
    QGridLayout* const box_layout = new QGridLayout(box);
    layout->addWidget(box);

    {
      QHBoxLayout* const hbox = new QHBoxLayout();
      QLabel* const label = new QLabel(tr("Memory Cards:"), box);
      box_layout->addWidget(label, 0, 0);
      QLineEdit* const directory = new QLineEdit(box);
      QPushButton* const browse = new QPushButton(box);
      browse->setToolTip(tr("Browse..."));
      browse->setIcon(QIcon(u":/icons/monochrome/svg/folder-open-line.svg"_s));
      QPushButton* const open = new QPushButton(box);
      open->setToolTip(tr("Open..."));
      open->setIcon(QIcon(u":/icons/monochrome/svg/open-folder-line.svg"_s));
      QPushButton* const reset = new QPushButton(box);
      reset->setToolTip(qApp->translate("QPlatformTheme", "Reset"));
      reset->setIcon(QIcon(u":/icons/monochrome/svg/delete-back-2-line.svg"_s));
      hbox->addWidget(directory);
      hbox->addWidget(browse);
      hbox->addWidget(open);
      hbox->addWidget(reset);
      box_layout->addLayout(hbox, 0, 1);

      SettingWidgetBinder::BindWidgetToFolderSetting(m_dialog->getSettingsInterface(), directory, browse,
                                                     tr("Select Memory Card Directory"), open, reset, "MemoryCards",
                                                     "Directory", Path::Combine(EmuFolders::DataRoot, "memcards"));

      m_dialog->registerWidgetHelp(directory, tr("Memory Cards Location"), tr("Default"),
                                   tr("Specifies the directory where memory cards will be saved."));
    }

    {
      QHBoxLayout* const hbox = new QHBoxLayout();
      QLabel* const label = new QLabel(tr("Save States:"), box);
      box_layout->addWidget(label, 1, 0);
      QLineEdit* const directory = new QLineEdit(box);
      QPushButton* const browse = new QPushButton(box);
      browse->setToolTip(tr("Browse..."));
      browse->setIcon(QIcon(u":/icons/monochrome/svg/folder-open-line.svg"_s));
      QPushButton* const open = new QPushButton(box);
      open->setToolTip(tr("Open..."));
      open->setIcon(QIcon(u":/icons/monochrome/svg/open-folder-line.svg"_s));
      QPushButton* const reset = new QPushButton(box);
      reset->setToolTip(qApp->translate("QPlatformTheme", "Reset"));
      reset->setIcon(QIcon(u":/icons/monochrome/svg/delete-back-2-line.svg"_s));
      hbox->addWidget(directory);
      hbox->addWidget(browse);
      hbox->addWidget(open);
      hbox->addWidget(reset);
      box_layout->addLayout(hbox, 1, 1);

      SettingWidgetBinder::BindWidgetToFolderSetting(m_dialog->getSettingsInterface(), directory, browse,
                                                     tr("Select Save States Directory"), open, reset, "Folders",
                                                     "SaveStates", Path::Combine(EmuFolders::DataRoot, "savestates"));

      m_dialog->registerWidgetHelp(directory, tr("Save States Location"), tr("Default"),
                                   tr("Specifies the directory where save states will be saved."));
    }
  }

  {
    QGroupBox* const box = new QGroupBox(tr("Settings"), this);
    QGridLayout* const grid_layout = new QGridLayout(box);
    layout->addWidget(box);

    QCheckBox* const create_save_state_backups = new QCheckBox(tr("Create Save State Backups"), box);
    SettingWidgetBinder::BindWidgetToBoolSetting(m_dialog->getSettingsInterface(), create_save_state_backups, "Main",
                                                 "CreateSaveStateBackups", Settings::DEFAULT_SAVE_STATE_BACKUPS);
    grid_layout->addWidget(create_save_state_backups, 0, 0);
    m_dialog->registerWidgetHelp(
      create_save_state_backups, tr("Create Save State Backups"), tr("Checked"),
      tr("Backs up any previous save state when creating a new save state, with a .bak extension."));

    QCheckBox* const enable_global_states = new QCheckBox(tr("Enable Global Save States"), box);
    SettingWidgetBinder::BindWidgetToBoolSetting(m_dialog->getSettingsInterface(), enable_global_states, "Main",
                                                 "EnableGlobalStates", false);
    grid_layout->addWidget(enable_global_states, 0, 1);
    m_dialog->registerWidgetHelp(enable_global_states, tr("Enable Global Save States"), tr("Unchecked"),
                                 tr("When enabled, the legacy global save state slots will be available. These slots "
                                    "are independent of the current game."));

    QCheckBox* playlist_title_as_game_title = new QCheckBox(tr("Use Single Card For Multi-Disc Games"), box);
    SettingWidgetBinder::BindWidgetToBoolSetting(m_dialog->getSettingsInterface(), playlist_title_as_game_title,
                                                 "MemoryCards", "UsePlaylistTitle", true);
    grid_layout->addWidget(playlist_title_as_game_title, 1, 0);
    m_dialog->registerWidgetHelp(
      playlist_title_as_game_title, tr("Use Single Card For Multi-Disc Games"), tr("Checked"),
      tr("When playing a multi-disc game and using per-game (title) memory cards, a single memory card "
         "will be used for all discs. If unchecked, a separate card will be used for each disc."));
  }

  {
    QGroupBox* const box = new QGroupBox(tr("Memory Card Editor"), this);
    QHBoxLayout* const box_layout = new QHBoxLayout(box);
    layout->addWidget(box);

    QLabel* const label = new QLabel(
      tr("The memory card editor enables you to move saves between cards, as well as import cards of other formats."),
      box);
    label->setWordWrap(true);
    box_layout->addWidget(label, 1);

    QPushButton* const button =
      new QPushButton(QIcon(u":/icons/monochrome/svg/memcard-line.svg"_s), tr("Memory Card Editor"), box);
    connect(button, &QPushButton::clicked, []() { g_main_window->openMemoryCardEditor(QString(), QString()); });
    box_layout->addWidget(button);
  }

  layout->addStretch(1);

  setLayout(layout);
}

void MemoryCardSettingsWidget::createPortSettings(MultitapMode mtap_mode)
{
  m_port_tabs->clear();
  for (PortSettingsUI& port : m_port_ui)
  {
    delete port.container;
    port = {};
  }

  const auto mtap_enabled = Controller::GetMultitapEnabledPorts(mtap_mode);
  u32 total_num_cards = 0;
  for (u32 i = 0; i < static_cast<u32>(m_port_ui.size()); i++)
  {
    if (Controller::PadIsMultitapSlot(i))
    {
      const auto [port, slot] = Controller::ConvertPadToPortAndSlot(i);
      if (!mtap_enabled[port])
        continue;
    }

    PortSettingsUI& port_ui = m_port_ui[i];
    createPortSettingsUi(i, &port_ui);
    m_port_tabs->addTab(port_ui.container, tr("Port %1").arg(QtUtils::StringViewToQStringView(
                                             Controller::GetPortDisplayName(i, mtap_mode))));
    total_num_cards++;
  }

  m_multitap_label->setText(tr("Current Multitap Mode: %1 (%n Cards)", "Card Count", static_cast<int>(total_num_cards))
                              .arg(QtUtils::StringViewToQStringView(Settings::GetMultitapModeDisplayName(mtap_mode))));
}

void MemoryCardSettingsWidget::createPortSettingsUi(u32 index, PortSettingsUI* ui)
{
  ui->container = new QGroupBox(m_port_tabs);
  ui->layout = new QVBoxLayout(ui->container);
  ui->layout->setContentsMargins(9, 9, 9, 9);

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
          [this, index]() { onMemoryCardTypeChanged(index); });
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

  ui->memory_card_path_browse = new QPushButton(ui->container);
  ui->memory_card_path_browse->setIcon(QIcon(u":/icons/monochrome/svg/folder-open-line.svg"_s));
  ui->memory_card_path_browse->setToolTip(tr("Browse..."));
  connect(ui->memory_card_path_browse, &QPushButton::clicked, this,
          [this, index]() { onBrowseMemoryCardPathClicked(index); });
  memory_card_layout->addWidget(ui->memory_card_path_browse);

  ui->memory_card_path_reset = new QPushButton(ui->container);
  ui->memory_card_path_reset->setIcon(QIcon(u":/icons/monochrome/svg/delete-back-2-line.svg"_s));
  ui->memory_card_path_reset->setToolTip(qApp->translate("QPlatformTheme", "Reset"));
  connect(ui->memory_card_path_reset, &QPushButton::clicked, this,
          [this, index]() { onResetMemoryCardPathClicked(index); });
  memory_card_layout->addWidget(ui->memory_card_path_reset);

  ui->memory_card_path_label = new QLabel(tr("Shared Memory Card Path:"), ui->container);
  ui->layout->addWidget(ui->memory_card_path_label);
  ui->layout->addLayout(memory_card_layout);

  onMemoryCardTypeChanged(index);
}

void MemoryCardSettingsWidget::onMemoryCardTypeChanged(u32 index)
{
  const MemoryCardType default_type =
    (index == 0) ? Settings::DEFAULT_MEMORY_CARD_1_TYPE : Settings::DEFAULT_MEMORY_CARD_2_TYPE;
  const MemoryCardType type =
    Settings::ParseMemoryCardTypeName(
      m_dialog->getEffectiveStringValue("MemoryCards", TinyString::from_format("Card{}Type", index + 1)))
      .value_or(default_type);
  const bool shared_enabled = (type == MemoryCardType::Shared);
  m_port_ui[index].memory_card_path_label->setEnabled(shared_enabled);
  m_port_ui[index].memory_card_path->setEnabled(shared_enabled);
  m_port_ui[index].memory_card_path_browse->setEnabled(shared_enabled);
  m_port_ui[index].memory_card_path_reset->setEnabled(shared_enabled);
}

void MemoryCardSettingsWidget::onBrowseMemoryCardPathClicked(u32 index)
{
  QString path = QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Select path to memory card image"),
                                                                       QString(), tr(MEMORY_CARD_IMAGE_FILTER)));
  if (path.isEmpty())
    return;

  m_port_ui[index].memory_card_path->setText(path);
}

void MemoryCardSettingsWidget::onMemoryCardPathChanged(u32 index)
{
  const auto key = TinyString::from_format("Card{}Path", index + 1);
  std::string relative_path(
    Path::MakeRelative(m_port_ui[index].memory_card_path->text().toStdString(), EmuFolders::MemoryCards));
  m_dialog->setStringSettingValue("MemoryCards", key, relative_path.c_str());
}

void MemoryCardSettingsWidget::onResetMemoryCardPathClicked(u32 index)
{
  const auto key = TinyString::from_format("Card{}Path", index + 1);
  if (m_dialog->isPerGameSettings())
    m_dialog->removeSettingValue("MemoryCards", key);
  else
    m_dialog->setStringSettingValue("MemoryCards", key, Settings::GetDefaultSharedMemoryCardName(index).c_str());

  updateMemoryCardPath(index);
}

void MemoryCardSettingsWidget::updateMemoryCardPath(u32 index)
{
  const auto key = TinyString::from_format("Card{}Path", index + 1);
  std::string path(
    m_dialog->getEffectiveStringValue("MemoryCards", key, Settings::GetDefaultSharedMemoryCardName(index).c_str()));
  if (!Path::IsAbsolute(path))
    path = Path::Canonicalize(Path::Combine(EmuFolders::MemoryCards, path));

  QSignalBlocker db(m_port_ui[index].memory_card_path);
  m_port_ui[index].memory_card_path->setText(QString::fromStdString(path));
}
