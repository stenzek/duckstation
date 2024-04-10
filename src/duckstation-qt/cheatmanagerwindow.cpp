// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "cheatmanagerwindow.h"
#include "cheatcodeeditordialog.h"
#include "mainwindow.h"
#include "qthost.h"
#include "qtutils.h"

#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/host.h"
#include "core/system.h"

#include "common/assert.h"
#include "common/string_util.h"

#include <QtCore/QFileInfo>
#include <QtGui/QColor>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTreeWidgetItemIterator>
#include <array>
#include <utility>

CheatManagerWindow::CheatManagerWindow() : QWidget()
{
  m_ui.setupUi(this);

  connectUi();

  updateCheatList();
}

CheatManagerWindow::~CheatManagerWindow() = default;

void CheatManagerWindow::connectUi()
{
  connect(m_ui.cheatList, &QTreeWidget::currentItemChanged, this, &CheatManagerWindow::cheatListCurrentItemChanged);
  connect(m_ui.cheatList, &QTreeWidget::itemActivated, this, &CheatManagerWindow::cheatListItemActivated);
  connect(m_ui.cheatList, &QTreeWidget::itemChanged, this, &CheatManagerWindow::cheatListItemChanged);
  connect(m_ui.cheatListNewCategory, &QPushButton::clicked, this, &CheatManagerWindow::newCategoryClicked);
  connect(m_ui.cheatListAdd, &QPushButton::clicked, this, &CheatManagerWindow::addCodeClicked);
  connect(m_ui.cheatListEdit, &QPushButton::clicked, this, &CheatManagerWindow::editCodeClicked);
  connect(m_ui.cheatListRemove, &QPushButton::clicked, this, &CheatManagerWindow::deleteCodeClicked);
  connect(m_ui.cheatListActivate, &QPushButton::clicked, this, &CheatManagerWindow::activateCodeClicked);
  connect(m_ui.cheatListImport, &QPushButton::clicked, this, &CheatManagerWindow::importClicked);
  connect(m_ui.cheatListExport, &QPushButton::clicked, this, &CheatManagerWindow::exportClicked);
  connect(m_ui.cheatListClear, &QPushButton::clicked, this, &CheatManagerWindow::clearClicked);
  connect(m_ui.cheatListReset, &QPushButton::clicked, this, &CheatManagerWindow::resetClicked);

  connect(g_emu_thread, &EmuThread::cheatEnabled, this, &CheatManagerWindow::setCheatCheckState);
  connect(g_emu_thread, &EmuThread::runningGameChanged, this, &CheatManagerWindow::updateCheatList);
}

void CheatManagerWindow::showEvent(QShowEvent* event)
{
  QWidget::showEvent(event);
  resizeColumns();
}

void CheatManagerWindow::closeEvent(QCloseEvent* event)
{
  QWidget::closeEvent(event);
  emit closed();
}

void CheatManagerWindow::resizeEvent(QResizeEvent* event)
{
  QWidget::resizeEvent(event);
  resizeColumns();
}

void CheatManagerWindow::resizeColumns()
{
  QtUtils::ResizeColumnsForTreeView(m_ui.cheatList, {-1, 100, 150, 100});
}

QTreeWidgetItem* CheatManagerWindow::getItemForCheatIndex(u32 index) const
{
  QTreeWidgetItemIterator iter(m_ui.cheatList);
  while (*iter)
  {
    QTreeWidgetItem* item = *iter;
    const QVariant item_data(item->data(0, Qt::UserRole));
    if (item_data.isValid() && item_data.toUInt() == index)
      return item;

    ++iter;
  }

  return nullptr;
}

QTreeWidgetItem* CheatManagerWindow::getItemForCheatGroup(const QString& group_name) const
{
  const int count = m_ui.cheatList->topLevelItemCount();
  for (int i = 0; i < count; i++)
  {
    QTreeWidgetItem* item = m_ui.cheatList->topLevelItem(i);
    if (item->text(0) == group_name)
      return item;
  }

  return nullptr;
}

QTreeWidgetItem* CheatManagerWindow::createItemForCheatGroup(const QString& group_name) const
{
  QTreeWidgetItem* group = new QTreeWidgetItem();
  group->setFlags(group->flags() | Qt::ItemIsUserCheckable);
  group->setText(0, group_name);
  m_ui.cheatList->addTopLevelItem(group);
  return group;
}

QStringList CheatManagerWindow::getCheatGroupNames() const
{
  QStringList group_names;

  const int count = m_ui.cheatList->topLevelItemCount();
  for (int i = 0; i < count; i++)
  {
    QTreeWidgetItem* item = m_ui.cheatList->topLevelItem(i);
    group_names.push_back(item->text(0));
  }

  return group_names;
}

static int getCheatIndexFromItem(QTreeWidgetItem* item)
{
  QVariant item_data(item->data(0, Qt::UserRole));
  if (!item_data.isValid())
    return -1;

  return static_cast<int>(item_data.toUInt());
}

int CheatManagerWindow::getSelectedCheatIndex() const
{
  QList<QTreeWidgetItem*> sel = m_ui.cheatList->selectedItems();
  if (sel.isEmpty())
    return -1;

  return static_cast<int>(getCheatIndexFromItem(sel.first()));
}

CheatList* CheatManagerWindow::getCheatList() const
{
  return System::IsValid() ? System::GetCheatList() : nullptr;
}

void CheatManagerWindow::updateCheatList()
{
  QSignalBlocker sb(m_ui.cheatList);
  while (m_ui.cheatList->topLevelItemCount() > 0)
    delete m_ui.cheatList->takeTopLevelItem(0);

  m_ui.cheatList->setEnabled(false);
  m_ui.cheatListAdd->setEnabled(false);
  m_ui.cheatListNewCategory->setEnabled(false);
  m_ui.cheatListEdit->setEnabled(false);
  m_ui.cheatListRemove->setEnabled(false);
  m_ui.cheatListActivate->setText(tr("Activate"));
  m_ui.cheatListActivate->setEnabled(false);
  m_ui.cheatListImport->setEnabled(false);
  m_ui.cheatListExport->setEnabled(false);
  m_ui.cheatListClear->setEnabled(false);
  m_ui.cheatListReset->setEnabled(false);

  Host::RunOnCPUThread([]() {
    if (!System::IsValid())
      return;

    CheatList* list = System::GetCheatList();
    if (!list)
    {
      System::LoadCheatList();
      list = System::GetCheatList();
    }
    if (!list)
    {
      System::LoadCheatListFromDatabase();
      list = System::GetCheatList();
    }
    if (!list)
    {
      System::SetCheatList(std::make_unique<CheatList>());
      list = System::GetCheatList();
    }

    // still racey...
    QtHost::RunOnUIThread([list]() {
      if (!QtHost::IsSystemValid())
        return;

      CheatManagerWindow* cm = g_main_window->getCheatManagerWindow();
      if (!cm)
        return;

      QSignalBlocker sb(cm->m_ui.cheatList);

      const std::vector<std::string> groups = list->GetCodeGroups();
      for (const std::string& group_name : groups)
      {
        QTreeWidgetItem* group = cm->createItemForCheatGroup(QString::fromStdString(group_name));

        const u32 count = list->GetCodeCount();
        bool all_enabled = true;
        for (u32 i = 0; i < count; i++)
        {
          const CheatCode& code = list->GetCode(i);
          if (code.group != group_name)
            continue;

          QTreeWidgetItem* item = new QTreeWidgetItem(group);
          cm->fillItemForCheatCode(item, i, code);

          all_enabled &= code.enabled;
        }

        group->setCheckState(0, all_enabled ? Qt::Checked : Qt::Unchecked);
        group->setExpanded(true);
      }

      cm->m_ui.cheatList->setEnabled(true);
      cm->m_ui.cheatListAdd->setEnabled(true);
      cm->m_ui.cheatListNewCategory->setEnabled(true);
      cm->m_ui.cheatListImport->setEnabled(true);
      cm->m_ui.cheatListClear->setEnabled(true);
      cm->m_ui.cheatListReset->setEnabled(true);
      cm->m_ui.cheatListExport->setEnabled(cm->m_ui.cheatList->topLevelItemCount() > 0);
    });
  });
}

void CheatManagerWindow::fillItemForCheatCode(QTreeWidgetItem* item, u32 index, const CheatCode& code)
{
  item->setData(0, Qt::UserRole, QVariant(static_cast<uint>(index)));
  if (code.IsManuallyActivated())
  {
    item->setFlags(item->flags() & ~(Qt::ItemIsUserCheckable));
  }
  else
  {
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, code.enabled ? Qt::Checked : Qt::Unchecked);
  }
  item->setText(0, QString::fromStdString(code.description));
  item->setText(1, qApp->translate("Cheats", CheatCode::GetTypeDisplayName(code.type)));
  item->setText(2, qApp->translate("Cheats", CheatCode::GetActivationDisplayName(code.activation)));
  item->setText(3, QString::number(static_cast<uint>(code.instructions.size())));
}

void CheatManagerWindow::saveCheatList()
{
  Host::RunOnCPUThread([]() { System::SaveCheatList(); });
}

void CheatManagerWindow::cheatListCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous)
{
  const int cheat_index = current ? getCheatIndexFromItem(current) : -1;
  const bool has_current = (cheat_index >= 0);
  m_ui.cheatListEdit->setEnabled(has_current);
  m_ui.cheatListRemove->setEnabled(has_current);
  m_ui.cheatListActivate->setEnabled(has_current);

  if (!has_current)
  {
    m_ui.cheatListActivate->setText(tr("Activate"));
  }
  else
  {
    const bool manual_activation = getCheatList()->GetCode(static_cast<u32>(cheat_index)).IsManuallyActivated();
    m_ui.cheatListActivate->setText(manual_activation ? tr("Activate") : tr("Toggle"));
  }
}

void CheatManagerWindow::cheatListItemActivated(QTreeWidgetItem* item)
{
  if (!item)
    return;

  const int index = getCheatIndexFromItem(item);
  if (index >= 0)
    activateCheat(static_cast<u32>(index));
}

void CheatManagerWindow::cheatListItemChanged(QTreeWidgetItem* item, int column)
{
  if (!item || column != 0)
    return;

  CheatList* list = getCheatList();

  const int index = getCheatIndexFromItem(item);
  if (index < 0)
  {
    // we're probably a parent/group node
    const int child_count = item->childCount();
    const Qt::CheckState cs = item->checkState(0);
    for (int i = 0; i < child_count; i++)
      item->child(i)->setCheckState(0, cs);

    return;
  }

  if (static_cast<u32>(index) >= list->GetCodeCount())
    return;

  CheatCode& cc = list->GetCode(static_cast<u32>(index));
  if (cc.IsManuallyActivated())
    return;

  const bool new_enabled = (item->checkState(0) == Qt::Checked);
  if (cc.enabled == new_enabled)
    return;

  Host::RunOnCPUThread([index, new_enabled]() {
    System::GetCheatList()->SetCodeEnabled(static_cast<u32>(index), new_enabled);
    System::SaveCheatList();
  });
}

void CheatManagerWindow::activateCheat(u32 index)
{
  CheatList* list = getCheatList();
  if (index >= list->GetCodeCount())
    return;

  CheatCode& cc = list->GetCode(index);
  if (cc.IsManuallyActivated())
  {
    g_emu_thread->applyCheat(index);
    return;
  }

  const bool new_enabled = !cc.enabled;
  setCheatCheckState(index, new_enabled);

  Host::RunOnCPUThread([index, new_enabled]() {
    System::GetCheatList()->SetCodeEnabled(index, new_enabled);
    System::SaveCheatList();
  });
}

void CheatManagerWindow::setCheatCheckState(u32 index, bool checked)
{
  QTreeWidgetItem* item = getItemForCheatIndex(index);
  if (item)
  {
    QSignalBlocker sb(m_ui.cheatList);
    item->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
  }
}

void CheatManagerWindow::newCategoryClicked()
{
  QString group_name = QInputDialog::getText(this, tr("Add Group"), tr("Group Name:"));
  if (group_name.isEmpty())
    return;

  if (getItemForCheatGroup(group_name) != nullptr)
  {
    QMessageBox::critical(this, tr("Error"), tr("This group name already exists."));
    return;
  }

  createItemForCheatGroup(group_name);
}

void CheatManagerWindow::addCodeClicked()
{
  CheatList* list = getCheatList();

  CheatCode new_code;
  new_code.group = "Ungrouped";

  CheatCodeEditorDialog editor(getCheatGroupNames(), &new_code, this);
  if (editor.exec() > 0)
  {
    const QString group_name_qstr(QString::fromStdString(new_code.group));
    QTreeWidgetItem* group_item = getItemForCheatGroup(group_name_qstr);
    if (!group_item)
      group_item = createItemForCheatGroup(group_name_qstr);

    QTreeWidgetItem* item = new QTreeWidgetItem(group_item);
    fillItemForCheatCode(item, list->GetCodeCount(), new_code);
    group_item->setExpanded(true);

    Host::RunOnCPUThread(
      [&new_code]() {
        System::GetCheatList()->AddCode(std::move(new_code));
        System::SaveCheatList();
      },
      true);
  }
}

void CheatManagerWindow::editCodeClicked()
{
  int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  CheatList* list = getCheatList();
  if (static_cast<u32>(index) >= list->GetCodeCount())
    return;

  CheatCode new_code = list->GetCode(static_cast<u32>(index));
  CheatCodeEditorDialog editor(getCheatGroupNames(), &new_code, this);
  if (editor.exec() > 0)
  {
    QTreeWidgetItem* item = getItemForCheatIndex(static_cast<u32>(index));
    if (item)
    {
      if (new_code.group != list->GetCode(static_cast<u32>(index)).group)
      {
        item = item->parent()->takeChild(item->parent()->indexOfChild(item));

        const QString group_name_qstr(QString::fromStdString(new_code.group));
        QTreeWidgetItem* group_item = getItemForCheatGroup(group_name_qstr);
        if (!group_item)
          group_item = createItemForCheatGroup(group_name_qstr);
        group_item->addChild(item);
        group_item->setExpanded(true);
      }

      fillItemForCheatCode(item, static_cast<u32>(index), new_code);
    }
    else
    {
      // shouldn't happen...
      updateCheatList();
    }

    Host::RunOnCPUThread(
      [index, &new_code]() {
        System::GetCheatList()->SetCode(static_cast<u32>(index), std::move(new_code));
        System::SaveCheatList();
      },
      true);
  }
}

void CheatManagerWindow::deleteCodeClicked()
{
  int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  CheatList* list = getCheatList();
  if (static_cast<u32>(index) >= list->GetCodeCount())
    return;

  if (QMessageBox::question(this, tr("Delete Code"),
                            tr("Are you sure you wish to delete the selected code? This action is not reversible."),
                            QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
  {
    return;
  }

  Host::RunOnCPUThread(
    [index]() {
      System::GetCheatList()->RemoveCode(static_cast<u32>(index));
      System::SaveCheatList();
    },
    true);
  updateCheatList();
}

void CheatManagerWindow::activateCodeClicked()
{
  int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  activateCheat(static_cast<u32>(index));
}

void CheatManagerWindow::importClicked()
{
  QMenu menu(this);
  connect(menu.addAction(tr("From File...")), &QAction::triggered, this, &CheatManagerWindow::importFromFileTriggered);
  connect(menu.addAction(tr("From Text...")), &QAction::triggered, this, &CheatManagerWindow::importFromTextTriggered);
  menu.exec(QCursor::pos());
}

void CheatManagerWindow::importFromFileTriggered()
{
  const QString filter(tr("PCSXR/Libretro Cheat Files (*.cht *.txt);;All Files (*.*)"));
  const QString filename =
    QDir::toNativeSeparators(QFileDialog::getOpenFileName(this, tr("Import Cheats"), QString(), filter));
  if (filename.isEmpty())
    return;

  CheatList new_cheats;
  if (!new_cheats.LoadFromFile(filename.toUtf8().constData(), CheatList::Format::Autodetect))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to parse cheat file. The log may contain more information."));
    return;
  }

  Host::RunOnCPUThread(
    [&new_cheats]() {
      DebugAssert(System::HasCheatList());
      System::GetCheatList()->MergeList(new_cheats);
      System::SaveCheatList();
    },
    true);
  updateCheatList();
}

void CheatManagerWindow::importFromTextTriggered()
{
  const QString text = QInputDialog::getMultiLineText(this, tr("Import Cheats"), tr("Cheat File Text:"));
  if (text.isEmpty())
    return;

  CheatList new_cheats;
  if (!new_cheats.LoadFromString(text.toStdString(), CheatList::Format::Autodetect))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to parse cheat file. The log may contain more information."));
    return;
  }

  Host::RunOnCPUThread(
    [&new_cheats]() {
      DebugAssert(System::HasCheatList());
      System::GetCheatList()->MergeList(new_cheats);
      System::SaveCheatList();
    },
    true);
  updateCheatList();
}

void CheatManagerWindow::exportClicked()
{
  const QString filter(tr("PCSXR Cheat Files (*.cht);;All Files (*.*)"));
  const QString filename =
    QDir::toNativeSeparators(QFileDialog::getSaveFileName(this, tr("Export Cheats"), QString(), filter));
  if (filename.isEmpty())
    return;

  if (!getCheatList()->SaveToPCSXRFile(filename.toUtf8().constData()))
    QMessageBox::critical(this, tr("Error"), tr("Failed to save cheat file. The log may contain more information."));
}

void CheatManagerWindow::clearClicked()
{
  if (QMessageBox::question(this, tr("Confirm Clear"),
                            tr("Are you sure you want to remove all cheats? This is not reversible.")) !=
      QMessageBox::Yes)
  {
    return;
  }

  Host::RunOnCPUThread([] { System::ClearCheatList(true); }, true);
  updateCheatList();
}

void CheatManagerWindow::resetClicked()
{
  if (QMessageBox::question(
        this, tr("Confirm Reset"),
        tr(
          "Are you sure you want to reset the cheat list? Any cheats not in the DuckStation database WILL BE LOST.")) !=
      QMessageBox::Yes)
  {
    return;
  }

  Host::RunOnCPUThread([] { System::DeleteCheatList(); }, true);
  updateCheatList();
}
