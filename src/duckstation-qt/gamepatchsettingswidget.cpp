// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamepatchsettingswidget.h"
#include "gamecheatsettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"

#include "core/cheats.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"

#include <algorithm>

#include "moc_gamepatchsettingswidget.cpp"

using namespace Qt::StringLiterals;

GamePatchDetailsWidget::GamePatchDetailsWidget(std::string name, const std::string& author,
                                               const std::string& description, bool disallowed_for_achievements,
                                               bool enabled, bool removable, SettingsWindow* dialog, QWidget* parent)
  : QWidget(parent), m_dialog(dialog), m_name(std::move(name))
{
  m_ui.setupUi(this);

  QFont title_font(m_ui.name->font());
  title_font.setPixelSize(17);
  title_font.setBold(true);
  m_ui.name->setFont(title_font);
  m_ui.name->setText(QString::fromStdString(m_name));
  m_ui.description->setText(
    tr("<strong>Author: </strong>%1%2<br>%3")
      .arg(author.empty() ? tr("Unknown") : QString::fromStdString(author))
      .arg(disallowed_for_achievements ? tr("<br><strong>Not permitted in RetroAchievements hardcore mode.</strong>") :
                                         QString())
      .arg(description.empty() ? tr("No description provided.") : QString::fromStdString(description)));
  m_ui.description->setEnabled(false);
  m_ui.edit->setVisible(removable);
  m_ui.remove->setVisible(removable);

  DebugAssert(dialog->getSettingsInterface());
  m_ui.enabled->setChecked(enabled);
  connect(m_ui.enabled, &ToggleButton::checkStateChanged, this, &GamePatchDetailsWidget::onEnabledStateChanged);
  connect(m_ui.edit, &QToolButton::clicked, this, &GamePatchDetailsWidget::editRequested);
  connect(m_ui.remove, &QToolButton::clicked, this, &GamePatchDetailsWidget::removeRequested);
}

GamePatchDetailsWidget::~GamePatchDetailsWidget() = default;

void GamePatchDetailsWidget::onEnabledStateChanged(Qt::CheckState state)
{
  INISettingsInterface* si = m_dialog->getSettingsInterface();
  if (state == Qt::Checked)
    si->AddToStringList(Cheats::PATCHES_CONFIG_SECTION, "Enable", m_name.c_str());
  else
    si->RemoveFromStringList(Cheats::PATCHES_CONFIG_SECTION, "Enable", m_name.c_str());

  QtHost::SaveSettingsInterface(si, true, true);
}

GamePatchSettingsWidget::GamePatchSettingsWidget(SettingsWindow* dialog, QWidget* parent) : m_dialog(dialog)
{
  m_ui.setupUi(this);

  connect(m_ui.reload, &QPushButton::clicked, this, &GamePatchSettingsWidget::onReloadClicked);
  connect(m_ui.add, &QPushButton::clicked, this, &GamePatchSettingsWidget::onAddClicked);
  connect(m_ui.disableAllPatches, &QPushButton::clicked, this, &GamePatchSettingsWidget::disableAllPatches);

  reloadList();
}

GamePatchSettingsWidget::~GamePatchSettingsWidget() = default;

void GamePatchSettingsWidget::onReloadClicked()
{
  reloadList();

  // reload it on the emu thread too, so it picks up any changes
  QtHost::ReloadCheatFiles(m_dialog->getGameSerial(), false, true);
}

std::string GamePatchSettingsWidget::getPathForSavingPatches(std::string_view existing_name) const
{
  // Existing codes are written back to the file they came from.
  if (!existing_name.empty())
  {
    std::string path =
      Cheats::FindChtFileContainingCode(m_dialog->getGameSerial(), m_dialog->getGameHash(), false, existing_name);
    if (!path.empty())
      return path;
  }

  // Check for the path without the hash first. If we have one of those, keep using it.
  std::string path = Cheats::GetChtFilename(m_dialog->getGameSerial(), std::nullopt, false);
  if (!FileSystem::FileExists(path.c_str()) && m_dialog->isGameHashStable())
    path = Cheats::GetChtFilename(m_dialog->getGameSerial(), m_dialog->getGameHash(), false);
  return path;
}

void GamePatchSettingsWidget::onAddClicked()
{
  editPatch(std::string());
}

void GamePatchSettingsWidget::editPatch(const std::string& name)
{
  const Cheats::CodeInfo* code = name.empty() ? nullptr : Cheats::FindCodeInInfoList(m_patches, name);
  if (!name.empty() && !code)
    return;

  QStringList group_names;
  for (const std::string_view& prefix : Cheats::GetCodeListUniquePrefixes(m_patches, false))
    group_names.push_back(QtUtils::StringViewToQString(prefix));

  CheatCodeEditorDialog* const dlg = new CheatCodeEditorDialog(this, getPathForSavingPatches(name), m_patches,
                                                               code ? *code : Cheats::CodeInfo(), group_names);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(tr("Patch Editor"));

  connect(dlg, &QDialog::accepted, this, [this] {
    reloadList();
    QtHost::ReloadCheatFiles(m_dialog->getGameSerial(), false, true);
  });

  dlg->open();
}

void GamePatchSettingsWidget::removePatch(const std::string& name)
{
  if (QtUtils::MessageBoxQuestion(
        this, tr("Confirm Removal"),
        tr("You are removing the patch named '%1'. You cannot undo this action, are you sure you "
           "wish to delete this patch?")
          .arg(QString::fromStdString(name))) != QMessageBox::Yes)
  {
    return;
  }

  Error error;
  if (!Cheats::UpdateCodeInFile(getPathForSavingPatches(name).c_str(), name, nullptr, &error))
  {
    QtUtils::AsyncMessageBox(this, QMessageBox::Critical, tr("Error"),
                             tr("Failed to save file:\n%1").arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  // disable if it was enabled
  INISettingsInterface* si = m_dialog->getSettingsInterface();
  if (si->RemoveFromStringList(Cheats::PATCHES_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY, name.c_str()))
    QtHost::SaveSettingsInterface(si, true, true);

  reloadList();
  QtHost::ReloadCheatFiles(m_dialog->getGameSerial(), false, true);
}

void GamePatchSettingsWidget::disableAllPatches()
{
  SettingsInterface* sif = m_dialog->getSettingsInterface();
  sif->RemoveSection(Cheats::PATCHES_CONFIG_SECTION);
  m_dialog->saveGameSettings();
  reloadList();
}

void GamePatchSettingsWidget::reloadList()
{
  m_patches = Cheats::GetCodeInfoList(m_dialog->getGameSerial(), m_dialog->getGameHash(), false, true, true);
  std::vector<std::string> enabled_list =
    m_dialog->getSettingsInterface()->GetStringList(Cheats::PATCHES_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);

  QWidget* container = new QWidget(m_ui.scrollArea);
  container->setObjectName(u"patches_container"_s);
  QVBoxLayout* layout = new QVBoxLayout(container);
  m_ui.scrollArea->setWidget(container);

  // for some reason, has to happen after the setWidget()...
  container->setAutoFillBackground(false);

  if (!m_patches.empty())
  {
    layout->setContentsMargins(0, 0, 0, 0);

    bool first = true;
    for (const Cheats::CodeInfo& pi : m_patches)
    {
      if (!first)
      {
        QFrame* frame = new QFrame(container);
        frame->setFrameStyle(QFrame::HLine | QFrame::Sunken);
        layout->addWidget(frame);
      }
      else
      {
        first = false;
      }

      const bool enabled = (std::find(enabled_list.begin(), enabled_list.end(), pi.name) != enabled_list.end());
      GamePatchDetailsWidget* it =
        new GamePatchDetailsWidget(pi.name, pi.author, pi.description, pi.disallow_for_achievements, enabled,
                                   !pi.from_database, m_dialog, container);
      layout->addWidget(it);

      // queued, because reloading the list destroys the widget that sent the signal
      connect(
        it, &GamePatchDetailsWidget::editRequested, this, [this, name = pi.name]() { editPatch(name); },
        Qt::QueuedConnection);
      connect(
        it, &GamePatchDetailsWidget::removeRequested, this, [this, name = pi.name]() { removePatch(name); },
        Qt::QueuedConnection);
    }
  }
  else
  {
    QLabel* label = new QLabel(tr("No patches are available for this game."), container);
    QFont font(label->font());
    font.setPixelSize(15);
    font.setBold(true);
    label->setFont(font);
    layout->addWidget(label);
  }

  layout->addStretch();
}
