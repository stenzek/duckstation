// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "gamecheatsettingswidget.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"
#include "settingswindow.h"
#include "settingwidgetbinder.h"

#include "core/cheats.h"

#include "common/error.h"
#include "common/string_util.h"

#include "fmt/format.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QPainter>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QStyledItemDelegate>

namespace {
class CheatListOptionDelegate : public QStyledItemDelegate
{
public:
  CheatListOptionDelegate(GameCheatSettingsWidget* parent, QTreeWidget* treeview);

  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
  void setEditorData(QWidget* editor, const QModelIndex& index) const override;
  void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;
  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
  std::string getCodeNameForRow(const QModelIndex& index) const;
  const Cheats::CodeInfo* getCodeInfoForRow(const QModelIndex& index) const;

  GameCheatSettingsWidget* m_parent;
  QTreeWidget* m_treeview;
};
}; // namespace

CheatListOptionDelegate::CheatListOptionDelegate(GameCheatSettingsWidget* parent, QTreeWidget* treeview)
  : QStyledItemDelegate(parent), m_parent(parent), m_treeview(treeview)
{
}

std::string CheatListOptionDelegate::getCodeNameForRow(const QModelIndex& index) const
{
  return index.siblingAtColumn(0).data(Qt::UserRole).toString().toStdString();
}

const Cheats::CodeInfo* CheatListOptionDelegate::getCodeInfoForRow(const QModelIndex& index) const
{
  return m_parent->getCodeInfo(getCodeNameForRow(index));
}

QWidget* CheatListOptionDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option,
                                               const QModelIndex& index) const
{
  // only edit the value, don't want the title becoming editable
  if (index.column() != 1)
    return nullptr;

  const QVariant data = index.data(Qt::UserRole);
  if (data.isNull())
    return nullptr;

  // if it's a uint, it's a range, otherwise string => combobox
  if (data.typeId() == QMetaType::QString)
    return new QComboBox(parent);
  else if (data.typeId() == QMetaType::UInt)
    return new QSpinBox(parent);
  else
    return QStyledItemDelegate::createEditor(parent, option, index);
}

void CheatListOptionDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
  if (index.column() != 1)
    return;

  if (QComboBox* cb = qobject_cast<QComboBox*>(editor))
  {
    const Cheats::CodeInfo* ci = getCodeInfoForRow(index);
    if (ci)
    {
      int current_index = 0;
      const QString selected_name = index.data(Qt::UserRole).toString();
      for (const Cheats::CodeOption& opt : ci->options)
      {
        const QString name = QString::fromStdString(opt.first);
        cb->addItem(name, QVariant(static_cast<uint>(opt.second)));
        if (name == selected_name)
          cb->setCurrentIndex(current_index);
        current_index++;
      }
    }
  }
  else if (QSpinBox* sb = qobject_cast<QSpinBox*>(editor))
  {
    const Cheats::CodeInfo* ci = getCodeInfoForRow(index);
    if (ci)
    {
      sb->setMinimum(ci->option_range_start);
      sb->setMaximum(ci->option_range_end);
      sb->setValue(index.data(Qt::UserRole).toUInt());
    }
  }
  else
  {
    return QStyledItemDelegate::setEditorData(editor, index);
  }
}

void CheatListOptionDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const
{
  if (index.column() != 1)
    return;

  if (QComboBox* cb = qobject_cast<QComboBox*>(editor))
  {
    const QString value = cb->currentText();
    const Cheats::CodeInfo* ci = getCodeInfoForRow(index);
    if (ci)
    {
      m_parent->setCodeOption(ci->name, ci->MapOptionNameToValue(value.toStdString()));
      model->setData(index, value, Qt::UserRole);
    }
  }
  else if (QSpinBox* sb = qobject_cast<QSpinBox*>(editor))
  {
    const u32 value = static_cast<u32>(sb->value());
    m_parent->setCodeOption(getCodeNameForRow(index), value);
    model->setData(index, static_cast<uint>(value), Qt::UserRole);
  }
  else
  {
    return QStyledItemDelegate::setModelData(editor, model, index);
  }
}

void CheatListOptionDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
{
  if (index.column() == 0)
  {
    // skip for editable rows
    if (index.flags() & Qt::ItemIsEditable)
      return QStyledItemDelegate::paint(painter, option, index);

    // expand the width to full for those without options
    QStyleOptionViewItem option_copy(option);
    option_copy.rect.setWidth(option_copy.rect.width() + m_treeview->columnWidth(1));
    return QStyledItemDelegate::paint(painter, option_copy, index);
  }
  else
  {
    if (!(index.flags() & Qt::ItemIsEditable))
    {
      // disable painting this column, so we can expand it (see above)
      return;
    }

    // just draw the number or label as a string
    const QVariant data = index.data(Qt::UserRole);
    painter->drawText(option.rect, 0, data.toString());
  }
}

GameCheatSettingsWidget::GameCheatSettingsWidget(SettingsWindow* dialog, QWidget* parent) : m_dialog(dialog)
{
  m_ui.setupUi(this);
  m_ui.cheatList->setItemDelegate(new CheatListOptionDelegate(this, m_ui.cheatList));

  reloadList();

  SettingsInterface* sif = m_dialog->getSettingsInterface();

  // We don't use the binder here, because they're binary - either enabled, or not in the file.
  m_ui.enableCheats->setChecked(sif->GetBoolValue("Cheats", "EnableCheats", false));
  m_ui.loadDatabaseCheats->setChecked(sif->GetBoolValue("Cheats", "LoadCheatsFromDatabase", true));

  connect(m_ui.enableCheats, &QCheckBox::checkStateChanged, this, &GameCheatSettingsWidget::onEnableCheatsChanged);
  connect(m_ui.loadDatabaseCheats, &QCheckBox::checkStateChanged, this,
          &GameCheatSettingsWidget::onLoadDatabaseCheatsChanged);
  connect(m_ui.cheatList, &QTreeWidget::itemDoubleClicked, this,
          &GameCheatSettingsWidget::onCheatListItemDoubleClicked);
  connect(m_ui.cheatList, &QTreeWidget::customContextMenuRequested, this,
          &GameCheatSettingsWidget::onCheatListContextMenuRequested);
  connect(m_ui.cheatList, &QTreeWidget::itemChanged, this, &GameCheatSettingsWidget::onCheatListItemChanged);
  connect(m_ui.add, &QToolButton::clicked, this, &GameCheatSettingsWidget::newCode);
  connect(m_ui.remove, &QToolButton::clicked, this, &GameCheatSettingsWidget::onRemoveCodeClicked);
  connect(m_ui.disableAll, &QToolButton::clicked, this, &GameCheatSettingsWidget::disableAllCheats);
  connect(m_ui.reloadCheats, &QToolButton::clicked, this, &GameCheatSettingsWidget::onReloadClicked);
  connect(m_ui.importCheats, &QPushButton::clicked, this, &GameCheatSettingsWidget::onImportClicked);
  connect(m_ui.exportCheats, &QPushButton::clicked, this, &GameCheatSettingsWidget::onExportClicked);
}

GameCheatSettingsWidget::~GameCheatSettingsWidget() = default;

const Cheats::CodeInfo* GameCheatSettingsWidget::getCodeInfo(const std::string_view name) const
{
  return Cheats::FindCodeInInfoList(m_codes, name);
}

void GameCheatSettingsWidget::setCodeOption(const std::string_view name, u32 value)
{
  const Cheats::CodeInfo* info = getCodeInfo(name);
  if (!info)
    return;

  m_dialog->getSettingsInterface()->SetUIntValue("Cheats", info->name.c_str(), value);
  m_dialog->saveAndReloadGameSettings();
}

std::string GameCheatSettingsWidget::getPathForSavingCheats() const
{
  // Check for the path without the hash first. If we have one of those, keep using it.
  std::string path = Cheats::GetChtFilename(m_dialog->getGameSerial(), std::nullopt, true);
  if (!FileSystem::FileExists(path.c_str()))
    path = Cheats::GetChtFilename(m_dialog->getGameSerial(), m_dialog->getGameHash(), true);
  return path;
}

QStringList GameCheatSettingsWidget::getGroupNames() const
{
  std::vector<std::string_view> unique_prefixes = Cheats::GetCodeListUniquePrefixes(m_codes, false);

  QStringList ret;
  if (!unique_prefixes.empty())
  {
    ret.reserve(unique_prefixes.size());
    for (const std::string_view& prefix : unique_prefixes)
      ret.push_back(QtUtils::StringViewToQString(prefix));
  }
  return ret;
}

bool GameCheatSettingsWidget::hasCodeWithName(const std::string_view name) const
{
  return (Cheats::FindCodeInInfoList(m_codes, name) != nullptr);
}

void GameCheatSettingsWidget::onEnableCheatsChanged(Qt::CheckState state)
{
  if (state == Qt::Checked)
    m_dialog->getSettingsInterface()->SetBoolValue("Cheats", "EnableCheats", true);
  else
    m_dialog->getSettingsInterface()->DeleteValue("Cheats", "EnableCheats");
  m_dialog->saveAndReloadGameSettings();
}

void GameCheatSettingsWidget::onLoadDatabaseCheatsChanged(Qt::CheckState state)
{
  // Default is enabled.
  if (state == Qt::Checked)
    m_dialog->getSettingsInterface()->DeleteValue("Cheats", "LoadCheatsFromDatabase");
  else
    m_dialog->getSettingsInterface()->SetBoolValue("Cheats", "LoadCheatsFromDatabase", false);
  m_dialog->saveAndReloadGameSettings();
  reloadList();
}

void GameCheatSettingsWidget::onCheatListItemDoubleClicked(QTreeWidgetItem* item, int column)
{
  const QVariant item_data = item->data(0, Qt::UserRole);
  if (!item_data.isValid())
    return;

  editCode(item_data.toString().toStdString());
}

void GameCheatSettingsWidget::onCheatListItemChanged(QTreeWidgetItem* item, int column)
{
  const QVariant item_data = item->data(0, Qt::UserRole);
  if (!item_data.isValid())
    return;

  std::string cheat_name = item_data.toString().toStdString();
  const bool current_enabled =
    (std::find(m_enabled_codes.begin(), m_enabled_codes.end(), cheat_name) != m_enabled_codes.end());
  const bool current_checked = (item->checkState(0) == Qt::Checked);
  if (current_enabled == current_checked)
    return;

  setCheatEnabled(std::move(cheat_name), current_checked, true);
}

void GameCheatSettingsWidget::onCheatListContextMenuRequested(const QPoint& pos)
{
  Cheats::CodeInfo* selected = getSelectedCode();
  const std::string selected_code = selected ? selected->name : std::string();

  QMenu context_menu(m_ui.cheatList);

  QAction* add = context_menu.addAction(QIcon::fromTheme("add-line"), tr("Add Cheat..."));
  connect(add, &QAction::triggered, this, &GameCheatSettingsWidget::newCode);
  QAction* edit = context_menu.addAction(QIcon::fromTheme("mag-line"), tr("Edit Cheat..."));
  edit->setEnabled(selected != nullptr);
  connect(edit, &QAction::triggered, this, [this, &selected_code]() { editCode(selected_code); });
  QAction* remove = context_menu.addAction(QIcon::fromTheme("minus-line"), tr("Remove Cheat"));
  remove->setEnabled(selected != nullptr);
  connect(remove, &QAction::triggered, this, [this, &selected_code]() { removeCode(selected_code, true); });
  context_menu.addSeparator();

  QAction* disable_all = context_menu.addAction(QIcon::fromTheme("chat-off-line"), tr("Disable All Cheats"));
  connect(disable_all, &QAction::triggered, this, &GameCheatSettingsWidget::disableAllCheats);

  QAction* reload = context_menu.addAction(QIcon::fromTheme("refresh-line"), tr("Reload Cheats"));
  connect(reload, &QAction::triggered, this, &GameCheatSettingsWidget::onReloadClicked);

  context_menu.exec(m_ui.cheatList->mapToGlobal(pos));
}

void GameCheatSettingsWidget::onRemoveCodeClicked()
{
  Cheats::CodeInfo* selected = getSelectedCode();
  if (!selected)
    return;

  removeCode(selected->name, true);
}

void GameCheatSettingsWidget::onReloadClicked()
{
  reloadList();
  g_emu_thread->reloadCheats(true, false, true, true);
}

bool GameCheatSettingsWidget::shouldLoadFromDatabase() const
{
  return m_dialog->getSettingsInterface()->GetBoolValue("Cheats", "LoadCheatsFromDatabase", true);
}

Cheats::CodeInfo* GameCheatSettingsWidget::getSelectedCode()
{
  const QList<QTreeWidgetItem*> selected = m_ui.cheatList->selectedItems();
  if (selected.size() != 1)
    return nullptr;

  const QVariant item_data = selected[0]->data(0, Qt::UserRole);
  if (!item_data.isValid())
    return nullptr;

  return Cheats::FindCodeInInfoList(m_codes, item_data.toString().toStdString());
}

void GameCheatSettingsWidget::disableAllCheats()
{
  setStateForAll(false);
}

void GameCheatSettingsWidget::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  QtUtils::ResizeColumnsForTreeView(m_ui.cheatList, {-1, 150});
}

void GameCheatSettingsWidget::setCheatEnabled(std::string name, bool enabled, bool save_and_reload_settings)
{
  SettingsInterface* si = m_dialog->getSettingsInterface();
  const auto it = std::find(m_enabled_codes.begin(), m_enabled_codes.end(), name);

  if (enabled)
  {
    si->AddToStringList(Cheats::CHEATS_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY, name.c_str());
    if (it == m_enabled_codes.end())
      m_enabled_codes.push_back(std::move(name));
  }
  else
  {
    si->RemoveFromStringList(Cheats::CHEATS_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY, name.c_str());
    if (it != m_enabled_codes.end())
      m_enabled_codes.erase(it);
  }

  if (save_and_reload_settings)
    m_dialog->saveAndReloadGameSettings();
}

void GameCheatSettingsWidget::setStateForAll(bool enabled)
{
  QSignalBlocker sb(m_ui.cheatList);
  setStateRecursively(nullptr, enabled);
  m_dialog->saveAndReloadGameSettings();
}

void GameCheatSettingsWidget::setStateRecursively(QTreeWidgetItem* parent, bool enabled)
{
  const int count = parent ? parent->childCount() : m_ui.cheatList->topLevelItemCount();
  for (int i = 0; i < count; i++)
  {
    QTreeWidgetItem* item = parent ? parent->child(i) : m_ui.cheatList->topLevelItem(i);
    const QVariant item_data = item->data(0, Qt::UserRole);
    if (item_data.isValid())
    {
      if ((item->checkState(0) == Qt::Checked) != enabled)
      {
        item->setCheckState(0, enabled ? Qt::Checked : Qt::Unchecked);
        setCheatEnabled(item_data.toString().toStdString(), enabled, false);
      }
    }
    else
    {
      setStateRecursively(item, enabled);
    }
  }
}

void GameCheatSettingsWidget::reloadList()
{
  // Show all hashes, since the ini is shared.
  m_codes = Cheats::GetCodeInfoList(m_dialog->getGameSerial(), std::nullopt, true, shouldLoadFromDatabase(), true);
  m_enabled_codes =
    m_dialog->getSettingsInterface()->GetStringList(Cheats::CHEATS_CONFIG_SECTION, Cheats::PATCH_ENABLE_CONFIG_KEY);

  m_parent_map.clear();
  while (m_ui.cheatList->topLevelItemCount() > 0)
    delete m_ui.cheatList->takeTopLevelItem(0);

  for (const Cheats::CodeInfo& ci : m_codes)
  {
    const bool enabled = (std::find(m_enabled_codes.begin(), m_enabled_codes.end(), ci.name) != m_enabled_codes.end());

    const std::string_view parent_part = ci.GetNameParentPart();

    QTreeWidgetItem* parent = getTreeWidgetParent(parent_part);
    QTreeWidgetItem* item = new QTreeWidgetItem();
    populateTreeWidgetItem(item, ci, enabled);
    if (parent)
      parent->addChild(item);
    else
      m_ui.cheatList->addTopLevelItem(item);
  }

  // Hide root indicator when there's no groups, frees up some whitespace.
  m_ui.cheatList->setRootIsDecorated(!m_parent_map.empty());
}

void GameCheatSettingsWidget::onImportClicked()
{
  QMenu menu(this);
  connect(menu.addAction(tr("From File...")), &QAction::triggered, this,
          &GameCheatSettingsWidget::onImportFromFileTriggered);
  connect(menu.addAction(tr("From Text...")), &QAction::triggered, this,
          &GameCheatSettingsWidget::onImportFromTextTriggered);
  menu.exec(QCursor::pos());
}

void GameCheatSettingsWidget::onImportFromFileTriggered()
{
  const QString filter(tr("PCSXR/Libretro Cheat Files (*.cht *.txt);;All Files (*.*)"));
  const QString filename =
    QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Import Cheats"), QString(), filter));
  if (filename.isEmpty())
    return;

  Error error;
  const std::optional<std::string> file_contents = FileSystem::ReadFileToString(filename.toStdString().c_str(), &error);
  if (!file_contents.has_value())
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to read file:\n%1").arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  importCodes(file_contents.value());
}

void GameCheatSettingsWidget::onImportFromTextTriggered()
{
  const QString text = QInputDialog::getMultiLineText(this, tr("Import Cheats"), tr("Cheat File Text:"));
  if (text.isEmpty())
    return;

  importCodes(text.toStdString());
}

void GameCheatSettingsWidget::importCodes(const std::string& file_contents)
{
  Error error;
  Cheats::CodeInfoList new_codes;
  if (!Cheats::ImportCodesFromString(&new_codes, file_contents, Cheats::FileFormat::Unknown, true, &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to parse file:\n%1").arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  Cheats::MergeCheatList(&m_codes, std::move(new_codes));
  if (!Cheats::SaveCodesToFile(getPathForSavingCheats().c_str(), m_codes, &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save file:\n%1").arg(QString::fromStdString(error.GetDescription())));
  }

  reloadList();
}

void GameCheatSettingsWidget::newCode()
{
  Cheats::CodeInfo new_code;
  CheatCodeEditorDialog dlg(this, &new_code, getGroupNames());
  if (!dlg.exec())
  {
    // cancelled
    return;
  }

  // no need to reload cheats yet, it's not active. just refresh the list
  reloadList();
}

void GameCheatSettingsWidget::editCode(const std::string_view code_name)
{
  Cheats::CodeInfo* code = Cheats::FindCodeInInfoList(m_codes, code_name);
  if (!code)
    return;

  CheatCodeEditorDialog dlg(this, code, getGroupNames());
  if (!dlg.exec())
  {
    // no changes
    return;
  }

  reloadList();
  g_emu_thread->reloadCheats(true, true, false, true);
}

void GameCheatSettingsWidget::removeCode(const std::string_view code_name, bool confirm)
{
  Cheats::CodeInfo* code = Cheats::FindCodeInInfoList(m_codes, code_name);
  if (!code)
    return;

  if (code->from_database)
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("This code is from the built-in cheat database, and cannot be removed. To hide this code, "
                             "uncheck the \"Load Database Cheats\" option."));
    return;
  }

  if (QMessageBox::question(this, tr("Confirm Removal"),
                            tr("You are removing the code named '%1'. You cannot undo this action, are you sure you "
                               "wish to delete this code?")) != QMessageBox::Yes)
  {
    return;
  }

  Error error;
  if (!Cheats::UpdateCodeInFile(getPathForSavingCheats().c_str(), code->name, nullptr, &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save file:\n%1").arg(QString::fromStdString(error.GetDescription())));
    return;
  }

  reloadList();
  g_emu_thread->reloadCheats(true, true, false, true);
}

void GameCheatSettingsWidget::onExportClicked()
{
  const QString filter(tr("PCSXR Cheat Files (*.cht);;All Files (*.*)"));
  const QString filename =
    QDir::toNativeSeparators(QFileDialog::getSaveFileName(this, tr("Export Cheats"), QString(), filter));
  if (filename.isEmpty())
    return;

  Error error;
  if (!Cheats::ExportCodesToFile(filename.toStdString(), m_codes, &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save cheat file:\n%1").arg(QString::fromStdString(error.GetDescription())));
  }
}

QTreeWidgetItem* GameCheatSettingsWidget::getTreeWidgetParent(const std::string_view parent)
{
  if (parent.empty())
    return nullptr;

  auto it = m_parent_map.find(parent);
  if (it != m_parent_map.end())
    return it->second;

  std::string_view this_part = parent;
  QTreeWidgetItem* parent_to_this = nullptr;
  const std::string_view::size_type pos = parent.rfind('\\');
  if (pos != std::string::npos && pos != (parent.size() - 1))
  {
    // go up the chain until we find the real parent, then back down
    parent_to_this = getTreeWidgetParent(parent.substr(0, pos));
    this_part = parent.substr(pos + 1);
  }

  QTreeWidgetItem* item = new QTreeWidgetItem();
  item->setText(0, QString::fromUtf8(this_part.data(), this_part.length()));

  if (parent_to_this)
    parent_to_this->addChild(item);
  else
    m_ui.cheatList->addTopLevelItem(item);

  // Must be called after adding.
  item->setExpanded(true);
  m_parent_map.emplace(parent, item);
  return item;
}

void GameCheatSettingsWidget::populateTreeWidgetItem(QTreeWidgetItem* item, const Cheats::CodeInfo& pi, bool enabled)
{
  const std::string_view name_part = pi.GetNamePart();
  item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemNeverHasChildren);
  item->setCheckState(0, enabled ? Qt::Checked : Qt::Unchecked);
  item->setData(0, Qt::UserRole, QString::fromStdString(pi.name));
  if (!pi.description.empty())
    item->setToolTip(0, QString::fromStdString(pi.description));
  if (!name_part.empty())
    item->setText(0, QtUtils::StringViewToQString(name_part));

  if (pi.HasOptionChoices())
  {
    // need to resolve the value back to a name
    const std::string_view option_name =
      pi.MapOptionValueToName(m_dialog->getSettingsInterface()->GetTinyStringValue("Cheats", pi.name.c_str()));
    item->setData(1, Qt::UserRole, QtUtils::StringViewToQString(option_name));
    item->setFlags(item->flags() | Qt::ItemIsEditable);
  }
  else if (pi.HasOptionRange())
  {
    const u32 value = m_dialog->getSettingsInterface()->GetUIntValue("Cheats", pi.name.c_str(), pi.option_range_start);
    item->setData(1, Qt::UserRole, static_cast<uint>(value));
    item->setFlags(item->flags() | Qt::ItemIsEditable);
  }
}

CheatCodeEditorDialog::CheatCodeEditorDialog(GameCheatSettingsWidget* parent, Cheats::CodeInfo* code,
                                             const QStringList& group_names)
  : QDialog(parent), m_parent(parent), m_code(code)
{
  m_ui.setupUi(this);
  setupAdditionalUi(group_names);
  fillUi();

  connect(m_ui.group, &QComboBox::currentIndexChanged, this, &CheatCodeEditorDialog::onGroupSelectedIndexChanged);
  connect(m_ui.optionsType, &QComboBox::currentIndexChanged, this, &CheatCodeEditorDialog::onOptionTypeChanged);
  connect(m_ui.rangeMin, &QSpinBox::valueChanged, this, &CheatCodeEditorDialog::onRangeMinChanged);
  connect(m_ui.rangeMax, &QSpinBox::valueChanged, this, &CheatCodeEditorDialog::onRangeMaxChanged);
  connect(m_ui.editChoice, &QPushButton::clicked, this, &CheatCodeEditorDialog::onEditChoiceClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::accepted, this, &CheatCodeEditorDialog::saveClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &CheatCodeEditorDialog::cancelClicked);
}

CheatCodeEditorDialog::~CheatCodeEditorDialog() = default;

void CheatCodeEditorDialog::onGroupSelectedIndexChanged(int index)
{
  if (index != (m_ui.group->count() - 1))
    return;

  // new item...
  const QString text = QInputDialog::getText(
    this, tr("Enter Group Name"), tr("Enter name for the code group. Using backslashes (\\) will create sub-trees."));

  // don't want this re-triggering
  QSignalBlocker sb(m_ui.group);

  if (text.isEmpty())
  {
    // cancelled...
    m_ui.group->setCurrentIndex(0);
    return;
  }

  const int existing_index = m_ui.group->findText(text);
  if (existing_index >= 0)
  {
    m_ui.group->setCurrentIndex(existing_index);
    return;
  }

  m_ui.group->insertItem(index, text);
  m_ui.group->setCurrentIndex(index);
}

void CheatCodeEditorDialog::saveClicked()
{
  std::string new_name = m_ui.name->text().toStdString();
  if (new_name.empty())
  {
    QMessageBox::critical(this, tr("Error"), tr("Name cannot be empty."));
    return;
  }

  std::string new_body = m_ui.instructions->toPlainText().toStdString();
  if (new_body.empty())
  {
    QMessageBox::critical(this, tr("Error"), tr("Instructions cannot be empty."));
    return;
  }

  // name actually includes the prefix
  if (const int index = m_ui.group->currentIndex(); index != 0)
  {
    const std::string prefix = m_ui.group->currentText().toStdString();
    if (!prefix.empty())
      new_name = fmt::format("{}\\{}", prefix, new_name);
  }

  // if the name has changed, then we need to make sure it hasn't already been used
  if (new_name != m_code->name && m_parent->hasCodeWithName(new_name))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("A code with the name '%1' already exists.").arg(QString::fromStdString(new_name)));
    return;
  }

  std::string old_name = std::move(m_code->name);

  // cheats coming from the database need to be copied into the user's file
  if (m_code->from_database)
  {
    m_code->from_database = false;
    old_name.clear();
  }

  m_code->name = std::move(new_name);
  m_code->description = m_ui.description->toPlainText().replace('\n', ' ').toStdString();
  m_code->type = static_cast<Cheats::CodeType>(m_ui.type->currentIndex());
  m_code->activation = static_cast<Cheats::CodeActivation>(m_ui.activation->currentIndex());
  m_code->body = std::move(new_body);

  m_code->option_range_start = 0;
  m_code->option_range_end = 0;
  m_code->options = {};
  if (m_ui.optionsType->currentIndex() == 1)
  {
    // choices
    m_code->options = std::move(m_new_options);
  }
  else if (m_ui.optionsType->currentIndex() == 2)
  {
    // range
    m_code->option_range_start = static_cast<u16>(m_ui.rangeMin->value());
    m_code->option_range_end = static_cast<u16>(m_ui.rangeMax->value());
  }

  std::string path = m_parent->getPathForSavingCheats();
  Error error;
  if (!Cheats::UpdateCodeInFile(path.c_str(), old_name, m_code, &error))
  {
    QMessageBox::critical(this, tr("Error"),
                          tr("Failed to save cheat code:\n%1").arg(QString::fromStdString(error.GetDescription())));
  }

  done(1);
}

void CheatCodeEditorDialog::cancelClicked()
{
  done(0);
}

void CheatCodeEditorDialog::onOptionTypeChanged(int index)
{
  m_ui.editChoice->setVisible(index == 1);
  m_ui.rangeMin->setVisible(index == 2);
  m_ui.rangeMax->setVisible(index == 2);
}

void CheatCodeEditorDialog::onRangeMinChanged(int value)
{
  m_ui.rangeMax->setValue(std::max(m_ui.rangeMax->value(), value));
}

void CheatCodeEditorDialog::onRangeMaxChanged(int value)
{
  m_ui.rangeMin->setValue(std::min(m_ui.rangeMin->value(), value));
}

void CheatCodeEditorDialog::onEditChoiceClicked()
{
  GameCheatCodeChoiceEditorDialog dlg(this, m_new_options);
  if (dlg.exec())
    m_new_options = dlg.getNewOptions();
}

void CheatCodeEditorDialog::setupAdditionalUi(const QStringList& group_names)
{
  for (u32 i = 0; i < static_cast<u32>(Cheats::CodeType::Count); i++)
    m_ui.type->addItem(Cheats::GetTypeDisplayName(static_cast<Cheats::CodeType>(i)));

  for (u32 i = 0; i < static_cast<u32>(Cheats::CodeActivation::Count); i++)
    m_ui.activation->addItem(Cheats::GetActivationDisplayName(static_cast<Cheats::CodeActivation>(i)));

  m_ui.group->addItem(tr("Ungrouped"));

  if (!group_names.isEmpty())
    m_ui.group->addItems(group_names);

  m_ui.group->addItem(tr("New..."));
}

void CheatCodeEditorDialog::fillUi()
{
  m_ui.name->setText(QtUtils::StringViewToQString(m_code->GetNamePart()));
  m_ui.description->setPlainText(QString::fromStdString(m_code->description));

  const std::string_view group = m_code->GetNameParentPart();
  if (group.empty())
  {
    // ungrouped is always first
    m_ui.group->setCurrentIndex(0);
  }
  else
  {
    const QString group_qstr(QtUtils::StringViewToQString(group));
    int index = m_ui.group->findText(group_qstr);
    if (index < 0)
    {
      // shouldn't happen...
      index = m_ui.group->count() - 1;
      m_ui.group->insertItem(index, group_qstr);
    }

    m_ui.group->setCurrentIndex(index);
  }

  m_ui.type->setCurrentIndex(static_cast<int>(m_code->type));
  m_ui.activation->setCurrentIndex(static_cast<int>(m_code->activation));

  m_ui.instructions->setPlainText(QString::fromStdString(m_code->body));

  m_ui.rangeMin->setValue(static_cast<int>(m_code->option_range_start));
  m_ui.rangeMax->setValue(static_cast<int>(m_code->option_range_end));
  m_new_options = m_code->options;

  m_ui.optionsType->setCurrentIndex(m_code->HasOptionRange() ? 2 : (m_code->HasOptionChoices() ? 1 : 0));
  onOptionTypeChanged(m_ui.optionsType->currentIndex());
}

GameCheatCodeChoiceEditorDialog::GameCheatCodeChoiceEditorDialog(QWidget* parent, const Cheats::CodeOptionList& options)
  : QDialog(parent)
{
  m_ui.setupUi(this);

  connect(m_ui.add, &QToolButton::clicked, this, &GameCheatCodeChoiceEditorDialog::onAddClicked);
  connect(m_ui.remove, &QToolButton::clicked, this, &GameCheatCodeChoiceEditorDialog::onRemoveClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::accepted, this, &GameCheatCodeChoiceEditorDialog::onSaveClicked);
  connect(m_ui.buttonBox, &QDialogButtonBox::rejected, this, &CheatCodeEditorDialog::reject);

  m_ui.optionList->setRootIsDecorated(false);
  for (const Cheats::CodeOption& opt : options)
  {
    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    item->setText(0, QString::fromStdString(opt.first));
    item->setText(1, QString::number(opt.second));
    m_ui.optionList->addTopLevelItem(item);
  }
}

GameCheatCodeChoiceEditorDialog::~GameCheatCodeChoiceEditorDialog() = default;

void GameCheatCodeChoiceEditorDialog::resizeEvent(QResizeEvent* event)
{
  QDialog::resizeEvent(event);
  QtUtils::ResizeColumnsForTreeView(m_ui.optionList, {-1, 150});
}

void GameCheatCodeChoiceEditorDialog::onAddClicked()
{
  QTreeWidgetItem* item = new QTreeWidgetItem();
  item->setFlags(item->flags() | Qt::ItemIsEditable);
  item->setText(0, QStringLiteral("Option %1").arg(m_ui.optionList->topLevelItemCount()));
  item->setText(1, QStringLiteral("0"));
  m_ui.optionList->addTopLevelItem(item);
}

void GameCheatCodeChoiceEditorDialog::onRemoveClicked()
{
  const QList<QTreeWidgetItem*> items = m_ui.optionList->selectedItems();
  for (QTreeWidgetItem* item : items)
  {
    const int index = m_ui.optionList->indexOfTopLevelItem(item);
    if (index >= 0)
      delete m_ui.optionList->takeTopLevelItem(index);
  }
}

void GameCheatCodeChoiceEditorDialog::onSaveClicked()
{
  // validate the data
  const int count = m_ui.optionList->topLevelItemCount();
  if (count == 0)
  {
    QMessageBox::critical(this, tr("Error"), tr("At least one option must be defined."));
    return;
  }

  for (int i = 0; i < count; i++)
  {
    const QTreeWidgetItem* it = m_ui.optionList->topLevelItem(i);
    const QString this_name = it->text(0);
    for (int j = 0; j < count; j++)
    {
      if (i == j)
        continue;

      if (m_ui.optionList->topLevelItem(j)->text(0) == this_name)
      {
        QMessageBox::critical(this, tr("Error"), tr("The option '%1' is defined twice.").arg(this_name));
        return;
      }
    }

    // should be a parseable number
    const QString this_value = it->text(1);
    if (bool ok; this_value.toUInt(&ok), !ok)
    {
      QMessageBox::critical(this, tr("Error"),
                            tr("The option '%1' does not have a valid value. It must be a number.").arg(this_name));
      return;
    }
  }

  accept();
}

Cheats::CodeOptionList GameCheatCodeChoiceEditorDialog::getNewOptions() const
{
  Cheats::CodeOptionList ret;

  const int count = m_ui.optionList->topLevelItemCount();
  ret.reserve(static_cast<size_t>(count));

  for (int i = 0; i < count; i++)
  {
    const QTreeWidgetItem* it = m_ui.optionList->topLevelItem(i);
    ret.emplace_back(it->text(0).toStdString(), it->text(1).toUInt());
  }

  return ret;
}
