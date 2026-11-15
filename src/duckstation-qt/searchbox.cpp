// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "searchbox.h"

#include <QtGui/QIcon>
#include <QtGui/QKeyEvent>

#include "moc_searchbox.cpp"

using namespace Qt::StringLiterals;

SearchBox::SearchBox(QWidget* parent) : QLineEdit(parent)
{
  setClearButtonEnabled(true);
  addAction(QIcon(u":/icons/monochrome/svg/search-line.svg"_s), QLineEdit::LeadingPosition);
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
