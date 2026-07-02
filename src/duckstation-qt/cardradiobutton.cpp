// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "cardradiobutton.h"

#include <QtGui/QPainter>
#include <QtWidgets/QStyleOptionButton>

#include "moc_cardradiobutton.cpp"

CardRadioButton::CardRadioButton(QWidget* parent) : QRadioButton(parent)
{
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setIconSize(QSize(64, 64));
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

CardRadioButton::~CardRadioButton() = default;

QSize CardRadioButton::sizeHint() const
{
  return QSize(220, 160);
}

bool CardRadioButton::hitButton(const QPoint& pos) const
{
  return rect().contains(pos);
}

void CardRadioButton::paintEvent(QPaintEvent* event)
{
  Q_UNUSED(event);

  QStyleOptionButton option;
  initStyleOption(&option);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  const bool checked = isChecked();
  const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
  const QRectF background_rect = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);

  QColor background_color = checked ? option.palette.highlight().color() : option.palette.button().color();
  if (hovered)
    background_color = background_color.lighter(checked ? 110 : 105);

  QColor border_color = checked ? option.palette.highlight().color() : option.palette.mid().color();
  if (hasFocus())
    border_color = option.palette.highlight().color();

  painter.setPen(QPen(border_color, hasFocus() ? 2.0 : 1.0));
  painter.setBrush(background_color);
  painter.drawRoundedRect(background_rect, 8.0, 8.0);

  constexpr int MARGIN = 16;
  constexpr int TEXT_HEIGHT = 28;
  const QSize icon_size = iconSize();
  const int content_height = icon_size.height() + TEXT_HEIGHT + 8;
  const int icon_y = qMax(MARGIN, (height() - content_height) / 2);
  const QRect icon_rect((width() - icon_size.width()) / 2, icon_y, icon_size.width(), icon_size.height());
  const QIcon::Mode icon_mode = isEnabled() ? (checked ? QIcon::Selected : QIcon::Normal) : QIcon::Disabled;
  icon().paint(&painter, icon_rect, Qt::AlignCenter, icon_mode, checked ? QIcon::On : QIcon::Off);

  QFont title_font = font();
  title_font.setBold(true);
  painter.setFont(title_font);
  painter.setPen(checked ? option.palette.highlightedText().color() : option.palette.buttonText().color());
  painter.drawText(QRect(MARGIN, icon_rect.bottom() + 8, width() - (MARGIN * 2), TEXT_HEIGHT),
                   Qt::AlignHCenter | Qt::AlignTop, text());

  QStyleOptionButton indicator_option(option);
  indicator_option.rect = QRect(width() - 30, 10, 20, 20);
  style()->drawPrimitive(QStyle::PE_IndicatorRadioButton, &indicator_option, &painter, this);
}
