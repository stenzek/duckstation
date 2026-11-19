// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "folderssettingswidget.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/settings.h"

#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <tuple>

#include "moc_folderssettingswidget.cpp"

using namespace Qt::StringLiterals;

// key, folder, title, description
static constexpr const std::tuple<const char*, const std::string*, const char*, const char*> s_folders[] = {
  {"Cache", &EmuFolders::Cache, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Cache Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget",
                     "Used for storing compiled shaders, game list data, and downloaded achievement images.")},
  {"Cheats", &EmuFolders::Cheats, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Cheats Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget",
                     "Stores cheat files (.cht) that you have created or imported. These are used in addition to the "
                     "built-in cheat database.")},
  {"Covers", &EmuFolders::Covers, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Covers Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Used for storing covers in the game grid/Big Picture UIs.")},
  {"GameIcons", &EmuFolders::GameIcons, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Game Icons Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget",
                     "Stores game list icons extracted from memory card saves. A PNG named after a game's serial can "
                     "be placed here to use a custom icon.")},
  {"GameSettings", &EmuFolders::GameSettings, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Game Settings Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Stores per-game settings files, named by game serial.")},
  {"InputProfiles", &EmuFolders::InputProfiles, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Input Profiles Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Stores controller input profiles.")},
  {"Patches", &EmuFolders::Patches, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Patches Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget",
                     "Stores patch files (.cht) that you have created or imported. These are used in addition to the "
                     "built-in patch database.")},
  {"Shaders", &EmuFolders::Shaders, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Shaders Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget",
                     "Searched for post-processing shaders: GLSL shaders in this directory, ReShade shaders in "
                     "reshade/Shaders, and slang presets in slang.")},
  {"Subchannels", &EmuFolders::Subchannels, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "Subchannels Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget",
                     "Searched for SBI/LSD subchannel files, which are required by LibCrypt-protected games, when they "
                     "are not found next to the disc image.")},
  {"UserResources", &EmuFolders::UserResources, QT_TRANSLATE_NOOP("FoldersSettingsWidget", "User Resources Directory"),
   QT_TRANSLATE_NOOP("FoldersSettingsWidget",
                     "Files placed here override the bundled resources. Also used for custom themes and Big Picture "
                     "backgrounds.")},
};

FoldersSettingsWidget::FoldersSettingsWidget(SettingsWindow* dialog, QWidget* parent) : QWidget(parent)
{
  QVBoxLayout* const layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  QScrollArea* const scroll_area = new QScrollArea(this);
  scroll_area->setFrameShape(QFrame::NoFrame);
  scroll_area->setWidgetResizable(true);
  scroll_area->viewport()->setBackgroundRole(QPalette::Window);
  layout->addWidget(scroll_area);

  QWidget* const contents = new QWidget(scroll_area);
  contents->setBackgroundRole(QPalette::Window);
  QVBoxLayout* const groups_layout = new QVBoxLayout(contents);
  // leave a gap so the right edge of the group boxes is not hidden against the scroll bar
  groups_layout->setContentsMargins(0, 0, groups_layout->spacing(), 0);
  scroll_area->setWidget(contents);

  for (const auto& [key, folder, title_str, description_str] : s_folders)
  {
    const QString title = tr(title_str);
    const QString description_text = tr(description_str);

    QGroupBox* const group = new QGroupBox(title, contents);
    QGridLayout* const grid = new QGridLayout(group);
    grid->setColumnStretch(0, 1);
    groups_layout->addWidget(group);

    QLabel* const description = new QLabel(description_text, group);
    description->setWordWrap(true);
    grid->addWidget(description, 0, 0, 1, 2);

    QLineEdit* const directory = new QLineEdit(group);
    directory->setAccessibleName(title);
    QPushButton* const browse =
      new QPushButton(QIcon(u":/icons/monochrome/svg/folder-open-line.svg"_s), tr("Browse..."), group);
    QPushButton* const more = new QPushButton(QIcon(u":/icons/monochrome/svg/more-fill.svg"_s), {}, group);
    more->setToolTip(tr("More Options"));

    grid->addWidget(more, 0, 2, Qt::AlignRight | Qt::AlignTop);
    grid->addWidget(directory, 1, 0);
    grid->addWidget(browse, 1, 1, 1, 2);

    SettingWidgetBinder::BindWidgetToFolderSetting(nullptr, directory, browse, tr("Select %1").arg(title), more,
                                                   EmuFolders::CONFIG_SECTION_NAME, key,
                                                   EmuFolders::GetDefaultPath(folder));
    dialog->registerWidgetHelp(directory, title, tr("Default"), description_text);
  }

  groups_layout->addStretch();
}
