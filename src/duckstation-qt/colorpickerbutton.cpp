// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "colorpickerbutton.h"

#include <QtGui/QPainter>
#include <QtWidgets/QColorDialog>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionButton>

#include "moc_colorpickerbutton.cpp"

ColorPickerButton::ColorPickerButton(QWidget* parent) : QPushButton(parent)
{
  connect(this, &QPushButton::clicked, this, &ColorPickerButton::onClicked);
}

u32 ColorPickerButton::color()
{
  return m_color;
}

void ColorPickerButton::setColor(u32 rgb)
{
  if (m_color == rgb)
    return;

  m_color = rgb;
  update();
}

void ColorPickerButton::paintEvent(QPaintEvent* event)
{
  Q_UNUSED(event);

  QPainter painter(this);
  QStyleOptionButton option;
  option.initFrom(this);

  if (isDown())
    option.state |= QStyle::State_Sunken;
  if (isDefault())
    option.features |= QStyleOptionButton::DefaultButton;

  // Get the content rect (area inside the border)
  const QRect contentRect = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);

  // Draw the button frame first (this includes the border but should not fill the interior)
  style()->drawPrimitive(QStyle::PE_PanelButtonBevel, &option, &painter, this);

  // Fill the content area with our custom color
  painter.fillRect(contentRect, QColor::fromRgb(m_color));

  // Draw the focus rectangle if needed
  if (option.state & QStyle::State_HasFocus)
  {
    QStyleOptionFocusRect focusOption;
    focusOption.initFrom(this);
    focusOption.rect = style()->subElementRect(QStyle::SE_PushButtonFocusRect, &option, this);
    style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focusOption, &painter, this);
  }

  // Draw the button label (text/icon) on top
  style()->drawControl(QStyle::CE_PushButtonLabel, &option, &painter, this);
}

void ColorPickerButton::onClicked()
{
  const u32 red = (m_color >> 16) & 0xff;
  const u32 green = (m_color >> 8) & 0xff;
  const u32 blue = m_color & 0xff;

  const QColor initial(QColor::fromRgb(red, green, blue));
  const QColor selected(QColorDialog::getColor(initial, this, tr("Select LED Color")));

  // QColorDialog returns Invalid on cancel, and apparently initial == Invalid is true...
  if (!selected.isValid() || initial == selected)
    return;

  const u32 new_rgb = (static_cast<u32>(selected.red()) << 16) | (static_cast<u32>(selected.green()) << 8) |
                      static_cast<u32>(selected.blue());
  m_color = new_rgb;
  update();
  emit colorChanged(new_rgb);
}
