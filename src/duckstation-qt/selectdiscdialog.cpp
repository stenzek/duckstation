// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "selectdiscdialog.h"
#include "qtutils.h"

#include "core/game_list.h"

#include "common/assert.h"
#include "common/path.h"

#include <QtWidgets/QTreeWidget>

SelectDiscDialog::SelectDiscDialog(const std::string& disc_set_name, QWidget* parent /* = nullptr */) : QDialog(parent)
{
  m_ui.setupUi(this);
  populateList(disc_set_name);
  updateStartEnabled();

  connect(m_ui.select, &QPushButton::clicked, this, &SelectDiscDialog::onSelectClicked);
  connect(m_ui.cancel, &QPushButton::clicked, this, &SelectDiscDialog::onCancelClicked);
  connect(m_ui.discList, &QTreeWidget::itemActivated, this, &SelectDiscDialog::onListItemActivated);
  connect(m_ui.discList, &QTreeWidget::itemSelectionChanged, this, &SelectDiscDialog::updateStartEnabled);
}

SelectDiscDialog::~SelectDiscDialog() = default;

void SelectDiscDialog::resizeEvent(QResizeEvent* ev)
{
  QDialog::resizeEvent(ev);

  QtUtils::ResizeColumnsForTreeView(m_ui.discList, {50, -1, 100});
}

void SelectDiscDialog::onListItemActivated(const QTreeWidgetItem* item)
{
  if (!item)
    return;

  m_selected_path = item->data(0, Qt::UserRole).toString().toStdString();
  done(1);
}

void SelectDiscDialog::updateStartEnabled()
{
  const QList<QTreeWidgetItem*> items = m_ui.discList->selectedItems();
  m_ui.select->setEnabled(!items.isEmpty());
  if (!items.isEmpty())
    m_selected_path = items.first()->data(0, Qt::UserRole).toString().toStdString();
  else
    m_selected_path = {};
}

void SelectDiscDialog::onSelectClicked()
{
  done(1);
}

void SelectDiscDialog::onCancelClicked()
{
  done(0);
}

void SelectDiscDialog::populateList(const std::string& disc_set_name)
{
  const auto lock = GameList::GetLock();
  const std::vector<const GameList::Entry*> entries = GameList::GetDiscSetMembers(disc_set_name);
  const GameList::Entry* last_played_entry = nullptr;

  for (const GameList::Entry* entry : entries)
  {
    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setData(0, Qt::UserRole, QString::fromStdString(entry->path));
    item->setIcon(0, QtUtils::GetIconForEntryType(GameList::EntryType::Disc));
    item->setText(0, QString::number(entry->disc_set_index + 1));
    item->setText(1, QtUtils::StringViewToQString(Path::GetFileName(entry->path)));
    item->setText(2, QtUtils::StringViewToQString(GameList::FormatTimestamp(entry->last_played_time)));
    m_ui.discList->addTopLevelItem(item);

    if (!last_played_entry ||
        (entry->last_played_time > 0 && entry->last_played_time > last_played_entry->last_played_time))
    {
      last_played_entry = entry;
      m_ui.discList->setCurrentItem(item);
    }
  }

  setWindowTitle(tr("Select Disc for %1").arg(QString::fromStdString(disc_set_name)));
}
