// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "togglebutton.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QStyleOption>

#include "moc_togglebutton.cpp"

ToggleButton::ToggleButton(QWidget* parent) : QAbstractButton(parent), m_offset_animation(this, "offset")
{
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);

  m_offset_animation.setDuration(150);
  m_offset_animation.setEasingCurve(QEasingCurve::OutCubic);
}

ToggleButton::~ToggleButton() = default;

QSize ToggleButton::sizeHint() const
{
  return QSize(50, 25);
}

void ToggleButton::showEvent(QShowEvent* event)
{
  QAbstractButton::showEvent(event);

  // Make sure the toggle position matches the current state when first shown
  updateTogglePosition();
}

void ToggleButton::resizeEvent(QResizeEvent* event)
{
  QAbstractButton::resizeEvent(event);

  // Update position when resized since it depends on widget dimensions
  updateTogglePosition();
}

void ToggleButton::updateTogglePosition()
{
  // Immediately set the toggle to the correct position without animation
  if (width() > 0)
  {
    m_offset_animation.stop();
    m_offset = isChecked() ? width() - height() : 0;
    update();
  }
}

void ToggleButton::paintEvent(QPaintEvent* event)
{
  Q_UNUSED(event);

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  QStyleOption opt;
  opt.initFrom(this);

  // Get colors from the current style
  QColor background_color = isChecked() ? opt.palette.highlight().color() : opt.palette.dark().color();
  QColor thumb_color = opt.palette.light().color();

  if (m_hovered || hasFocus())
  {
    background_color = background_color.lighter(120);
  }

  if (!isEnabled())
  {
    background_color = opt.palette.mid().color();
    thumb_color = opt.palette.midlight().color();
  }

  // Draw background
  const int track_width = width() - 2;
  const int track_height = height() - 2;
  const int corner_radius = track_height / 2;

  QPainterPath path;
  path.addRoundedRect(1, 1, track_width, track_height, corner_radius, corner_radius);

  painter.fillPath(path, background_color);

  // Draw thumb
  const int thumb_size = track_height - 4;
  const int thumb_x = m_offset + 2;
  const int thumb_y = 2;

  QPainterPath thumbPath;
  thumbPath.addEllipse(thumb_x, thumb_y, thumb_size, thumb_size);

  painter.fillPath(thumbPath, thumb_color);
}

void ToggleButton::enterEvent(QEnterEvent* event)
{
  Q_UNUSED(event);
  m_hovered = true;
  update();
}

void ToggleButton::leaveEvent(QEvent* event)
{
  Q_UNUSED(event);
  m_hovered = false;
  update();
}

void ToggleButton::checkStateSet()
{
  QAbstractButton::checkStateSet();
  animateToggle(isChecked());
}

void ToggleButton::animateToggle(bool checked)
{
  m_offset_animation.stop();
  m_offset_animation.setStartValue(m_offset);
  m_offset_animation.setEndValue(checked ? width() - height() : 0);
  m_offset_animation.start();
}

void ToggleButton::nextCheckState()
{
  QAbstractButton::nextCheckState();
  animateToggle(isChecked());
  update();
}

void ToggleButton::keyPressEvent(QKeyEvent* event)
{
  if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return)
  {
    setChecked(!isChecked());
    event->accept();
  }
  else
  {
    QAbstractButton::keyPressEvent(event);
  }
}

int ToggleButton::offset() const
{
  return m_offset;
}

void ToggleButton::setOffset(int value)
{
  m_offset = value;
  update();
}