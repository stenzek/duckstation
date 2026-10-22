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

class QStandardItem;
class QStandardItemModel;

class GameCheatSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GameCheatSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GameCheatSettingsWidget() override;

  const Cheats::CodeInfo* getCodeInfo(const std::string_view name) const;
  void setCodeOption(const std::string_view name, u32 value);

  /// Returns the path to write codes to. Existing codes are written back to the file they came from.
  static std::string getPathForSavingCodes(const SettingsWindow* dialog, bool cheats,
                                           std::string_view existing_name = {});

  /// Asks for confirmation, then removes a code from the user's file, along with its enable state and option value.
  static bool removeCodeFromFile(QWidget* parent, SettingsWindow* dialog, bool cheats, const std::string& name);

  void disableAllCheats();

private:
  void onEnableCheatsChanged(Qt::CheckState state);
  void onSortCheatsClicked(bool checked);
  void onSearchFilterChanged(const QString& text);
  void onLoadDatabaseCheatsChanged(Qt::CheckState state);
  void onCheatListItemDoubleClicked(const QModelIndex& index);
  void onCheatListItemChanged(QStandardItem* item);
  void onCheatListContextMenuRequested(const QPoint& pos);
  void onRemoveCodeClicked();
  void onReloadClicked();
  void onImportClicked();
  void onImportFromFileTriggered();
  void onImportFromTextTriggered();
  void onExportClicked();
  void onClearClicked();
  void reloadList();

  bool shouldLoadFromDatabase() const;
  void checkForMasterDisable();

  Cheats::CodeInfo* getSelectedCode();
  QStandardItem* getTreeWidgetParent(const std::string_view parent);
  void populateTreeWidgetItem(QStandardItem* parent, const Cheats::CodeInfo& pi, bool enabled);
  void expandAllItems();

  void setCheatEnabled(std::string name, bool enabled, bool save_and_reload_settings);
  void setStateForAll(bool enabled);
  void setStateRecursively(QStandardItem* parent, bool enabled);
  void importCodes(const std::string& file_contents);
  void newCode();
  void editCode(const std::string_view code_name);
  void removeCode(const std::string_view code_name, bool confirm);

  Ui::GameCheatSettingsWidget m_ui;
  SettingsWindow* m_dialog;

  UnorderedStringMap<QStandardItem*> m_parent_map;
  Cheats::CodeInfoList m_codes;
  QStandardItemModel* m_codes_model;
  QSortFilterProxyModel* m_sort_model;
  std::vector<std::string> m_enabled_codes;

  bool m_master_enable_ignored = false;
};

class CheatCodeEditorDialog final : public QDialog
{
  Q_OBJECT

public:
  CheatCodeEditorDialog(QWidget* parent, SettingsWindow* dialog, bool cheats,
                        const Cheats::CodeInfoList& existing_codes, Cheats::CodeInfo code);
  ~CheatCodeEditorDialog() override;

private:
  void onGroupSelectedIndexChanged(int index);
  void saveClicked();

  void onCodeTypeChanged(int index);
  void onOptionTypeChanged(int index);
  void onRangeMinChanged(int value);
  void onRangeMaxChanged(int value);
  void onEditChoiceClicked();

  void setupAdditionalUi();
  void fillUi();
  void renameCodeInSettings(const std::string& old_name);

  Ui::GameCheatCodeEditorDialog m_ui;

  SettingsWindow* m_dialog;
  const Cheats::CodeInfoList& m_existing_codes;
  bool m_cheats;

  Cheats::CodeInfo m_code;
  Cheats::CodeOptionList m_new_options;
};

class GameCheatCodeChoiceEditorDialog final : public QDialog
{
  Q_OBJECT

public:
  GameCheatCodeChoiceEditorDialog(QWidget* parent, const Cheats::CodeOptionList& options);
  ~GameCheatCodeChoiceEditorDialog() override;

  Cheats::CodeOptionList getNewOptions() const;

private:
  void onAddClicked();
  void onRemoveClicked();
  void onSaveClicked();

  Ui::GameCheatCodeChoiceEditorDialog m_ui;
};
