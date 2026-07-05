// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "searchbox.h"

#include <QtGui/QKeyEvent>

#include "moc_searchbox.cpp"

SearchBox::SearchBox(QWidget* parent) : QLineEdit(parent)
{
  setClearButtonEnabled(true);
  setPlaceholderText(tr("Search..."));
}

SearchBox::~SearchBox() = default;

void SearchBox::keyPressEvent(QKeyEvent* event)
{
  if (event->key() == Qt::Key_Escape && !text().isEmpty())
    clear();
  else
    QLineEdit::keyPressEvent(event);
}
