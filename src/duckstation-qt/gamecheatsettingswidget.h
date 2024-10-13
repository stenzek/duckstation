// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_gamecheatcodechoiceeditordialog.h"
#include "ui_gamecheatcodeeditordialog.h"
#include "ui_gamecheatsettingswidget.h"

#include "core/cheats.h"

#include "common/heterogeneous_containers.h"

#include <QtCore/QStringList>
#include <QtWidgets/QDialog>
#include <QtWidgets/QWidget>

#include <string>
#include <string_view>
#include <vector>

namespace GameList {
struct Entry;
}

class SettingsWindow;

class GameCheatSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GameCheatSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GameCheatSettingsWidget() override;

  const Cheats::CodeInfo* getCodeInfo(const std::string_view name) const;
  void setCodeOption(const std::string_view name, u32 value);
  std::string getPathForSavingCheats() const;
  QStringList getGroupNames() const;
  bool hasCodeWithName(const std::string_view name) const;
  void disableAllCheats();

protected:
  void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
  void onEnableCheatsChanged(Qt::CheckState state);
  void onLoadDatabaseCheatsChanged(Qt::CheckState state);
  void onCheatListItemDoubleClicked(QTreeWidgetItem* item, int column);
  void onCheatListItemChanged(QTreeWidgetItem* item, int column);
  void onCheatListContextMenuRequested(const QPoint& pos);
  void onRemoveCodeClicked();
  void onReloadClicked();
  void onImportClicked();
  void onImportFromFileTriggered();
  void onImportFromTextTriggered();
  void onExportClicked();
  void reloadList();

private:
  bool shouldLoadFromDatabase() const;

  Cheats::CodeInfo* getSelectedCode();
  QTreeWidgetItem* getTreeWidgetParent(const std::string_view parent);
  void populateTreeWidgetItem(QTreeWidgetItem* item, const Cheats::CodeInfo& pi, bool enabled);
  void setCheatEnabled(std::string name, bool enabled, bool save_and_reload_settings);
  void setStateForAll(bool enabled);
  void setStateRecursively(QTreeWidgetItem* parent, bool enabled);
  void importCodes(const std::string& file_contents);
  void newCode();
  void editCode(const std::string_view code_name);
  void removeCode(const std::string_view code_name, bool confirm);

  Ui::GameCheatSettingsWidget m_ui;
  SettingsWindow* m_dialog;

  UnorderedStringMap<QTreeWidgetItem*> m_parent_map;
  Cheats::CodeInfoList m_codes;
  std::vector<std::string> m_enabled_codes;
};

class CheatCodeEditorDialog : public QDialog
{
  Q_OBJECT

public:
  CheatCodeEditorDialog(GameCheatSettingsWidget* parent, Cheats::CodeInfo* code, const QStringList& group_names);
  ~CheatCodeEditorDialog() override;

private Q_SLOTS:
  void onGroupSelectedIndexChanged(int index);
  void saveClicked();
  void cancelClicked();

  void onOptionTypeChanged(int index);
  void onRangeMinChanged(int value);
  void onRangeMaxChanged(int value);
  void onEditChoiceClicked();

private:
  void setupAdditionalUi(const QStringList& group_names);
  void fillUi();

  GameCheatSettingsWidget* m_parent;
  Ui::GameCheatCodeEditorDialog m_ui;

  Cheats::CodeInfo* m_code;
  Cheats::CodeOptionList m_new_options;
};

class GameCheatCodeChoiceEditorDialog : public QDialog
{
  Q_OBJECT

public:
  GameCheatCodeChoiceEditorDialog(QWidget* parent, const Cheats::CodeOptionList& options);
  ~GameCheatCodeChoiceEditorDialog() override;

  Cheats::CodeOptionList getNewOptions() const;

protected:
  void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
  void onAddClicked();
  void onRemoveClicked();
  void onSaveClicked();

private:
  Ui::GameCheatCodeChoiceEditorDialog m_ui;
};
