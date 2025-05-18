// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamepatchsettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/cheats.h"

#include "common/assert.h"

#include <algorithm>

GamePatchDetailsWidget::GamePatchDetailsWidget(std::string name, const std::string& author,
                                               const std::string& description, bool disallowed_for_achievements,
                                               bool enabled, SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog), m_name(name)
{
  m_ui.setupUi(this);

  QFont title_font(m_ui.name->font());
  title_font.setPointSizeF(title_font.pointSizeF() + 4.0f);
  title_font.setBold(true);
  m_ui.name->setText(QString::fromStdString(name));
  m_ui.name->setFont(title_font);
  m_ui.description->setText(
    tr("<strong>Author: </strong>%1%2<br>%3")
      .arg(author.empty() ? tr("Unknown") : QString::fromStdString(author))
      .arg(disallowed_for_achievements ? tr("<br><strong>Not permitted in RetroAchievements hardcore mode.</strong>") :
                                         QString())
      .arg(description.empty() ? tr("No description provided.") : QString::fromStdString(description)));

  DebugAssert(dialog->getSettingsInterface());
  m_ui.enabled->setChecked(enabled);
  connect(m_ui.enabled, &QCheckBox::checkStateChanged, this, &GamePatchDetailsWidget::onEnabledStateChanged);
}

GamePatchDetailsWidget::~GamePatchDetailsWidget() = default;

void GamePatchDetailsWidget::onEnabledStateChanged(int state)
{
  SettingsInterface* si = m_dialog->getSettingsInterface();
  if (state == Qt::Checked)
    si->AddToStringList("Patches", "Enable", m_name.c_str());
  else
    si->RemoveFromStringList("Patches", "Enable", m_name.c_str());

  si->Save();
  g_emu_thread->reloadGameSettings();
}

GamePatchSettingsWidget::GamePatchSettingsWidget(SettingsWindow* dialog, QWidget* parent) : m_dialog(dialog)
{
  m_ui.setupUi(this);
  m_ui.scrollArea->setFrameShape(QFrame::WinPanel);
  m_ui.scrollArea->setFrameShadow(QFrame::Sunken);

  connect(m_ui.reload, &QPushButton::clicked, this, &GamePatchSettingsWidget::onReloadClicked);
  connect(m_ui.disableAllPatches, &QPushButton::clicked, this, &GamePatchSettingsWidget::disableAllPatches);

  reloadList();
}

GamePatchSettingsWidget::~GamePatchSettingsWidget() = default;

void GamePatchSettingsWidget::onReloadClicked()
{
  reloadList();

  // reload it on the emu thread too, so it picks up any changes
  g_emu_thread->reloadCheats(true, false, true, true);
}

void GamePatchSettingsWidget::disableAllPatches()
{
  SettingsInterface* sif = m_dialog->getSettingsInterface();
  sif->RemoveSection(Cheats::PATCHES_CONFIG_SECTION);
  m_dialog->saveAndReloadGameSettings();
  reloadList();
}

void GamePatchSettingsWidget::reloadList()
{
  std::vector<Cheats::CodeInfo> patches =
    Cheats::GetCodeInfoList(m_dialog->getGameSerial(), m_dialog->getGameHash(), false, true, true);
  std::vector<std::string> enabled_list =
    m_dialog->getSettingsInterface()->GetStringList(Cheats::PATCHES_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);

  delete m_ui.scrollArea->takeWidget();

  QWidget* container = new QWidget(m_ui.scrollArea);
  m_ui.scrollArea->setWidget(container);

  QVBoxLayout* layout = new QVBoxLayout(container);

  if (!patches.empty())
  {
    layout->setContentsMargins(0, 0, 0, 0);

    bool first = true;

    for (Cheats::CodeInfo& pi : patches)
    {
      if (!first)
      {
        QFrame* frame = new QFrame(container);
        frame->setFrameShape(QFrame::HLine);
        frame->setFrameShadow(QFrame::Sunken);
        layout->addWidget(frame);
      }
      else
      {
        first = false;
      }

      const bool enabled = (std::find(enabled_list.begin(), enabled_list.end(), pi.name) != enabled_list.end());
      GamePatchDetailsWidget* it = new GamePatchDetailsWidget(
        std::move(pi.name), pi.author, pi.description, pi.disallow_for_achievements, enabled, m_dialog, container);
      layout->addWidget(it);
    }
  }
  else
  {
    QLabel* label = new QLabel(tr("No patches are available for this game."), container);
    QFont font(label->font());
    font.setPointSizeF(font.pointSizeF() + 2.0f);
    font.setBold(true);
    label->setFont(font);
    layout->addWidget(label);
  }

  layout->addSpacerItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
}
