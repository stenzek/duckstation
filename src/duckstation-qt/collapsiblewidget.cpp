// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "collapsiblewidget.h"

#include <QtCore/QPropertyAnimation>
#include <QtGui/QPaintEvent>
#include <QtGui/QRegion>
#include <QtGui/QShowEvent>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionGroupBox>
#include <QtWidgets/QStylePainter>

#include <algorithm>

#include "moc_collapsiblewidget.cpp"

CollapsibleWidget::CollapsibleWidget(QWidget* parent) : QGroupBox(parent)
{
  init();
}

CollapsibleWidget::CollapsibleWidget(const QString& title, QWidget* parent) : QGroupBox(title, parent)
{
  init();
}

CollapsibleWidget::~CollapsibleWidget() = default;

void CollapsibleWidget::init()
{
  setCheckable(true);
  setChecked(false);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

  m_animation = new QPropertyAnimation(this, "maximumHeight", this);
  m_animation->setDuration(300);
  m_animation->setEasingCurve(QEasingCurve::InOutCubic);

  connect(this, &QGroupBox::toggled, this, &CollapsibleWidget::toggleCollapsed);
  connect(m_animation, &QPropertyAnimation::finished, this, [this]() {
    if (isChecked())
      setMaximumHeight(QWIDGETSIZE_MAX);
  });

  setMaximumHeight(collapsedHeight());
}

int CollapsibleWidget::collapsedHeight() const
{
  QStyleOptionGroupBox option;
  initStyleOption(&option);

  const QRect label_rect = style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, this);
  const QRect check_box_rect = style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxCheckBox, this);
  const int header_bottom = std::max(label_rect.bottom(), check_box_rect.bottom()) + 1;
  return std::max(header_bottom + contentsMargins().bottom(), fontMetrics().height());
}

void CollapsibleWidget::updateExpandedHeight()
{
  const int old_maximum_height = maximumHeight();
  setMaximumHeight(QWIDGETSIZE_MAX);
  m_expanded_height = sizeHint().height();
  setMaximumHeight(old_maximum_height);
}

void CollapsibleWidget::toggleCollapsed(bool expanded)
{
  m_animation->stop();

  if (expanded)
    updateExpandedHeight();

  m_animation->setStartValue(height());
  m_animation->setEndValue(expanded ? m_expanded_height : collapsedHeight());
  m_animation->start();
}

void CollapsibleWidget::setCollapsed(bool collapsed)
{
  setChecked(!collapsed);
}

void CollapsibleWidget::changeEvent(QEvent* event)
{
  QGroupBox::changeEvent(event);

  if (!isChecked() && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange))
    setMaximumHeight(collapsedHeight());
}

void CollapsibleWidget::paintEvent(QPaintEvent* event)
{
  Q_UNUSED(event);

  QStylePainter painter(this);
  QStyleOptionGroupBox option;
  initStyleOption(&option);

  const QRect indicator_rect = style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxCheckBox, this);
  if (!isChecked())
    option.subControls &= ~QStyle::SC_GroupBoxFrame;

  const QRegion group_box_region = QRegion(rect()).subtracted(indicator_rect.adjusted(-1, -1, 1, 1));
  painter.setClipRegion(group_box_region);
  painter.drawComplexControl(QStyle::CC_GroupBox, option);
  painter.setClipping(false);

  QStyleOption arrow_option;
  arrow_option.initFrom(this);
  arrow_option.rect = indicator_rect;
  painter.drawPrimitive(isChecked() ? QStyle::PE_IndicatorArrowDown : QStyle::PE_IndicatorArrowRight, arrow_option);
}

void CollapsibleWidget::showEvent(QShowEvent* event)
{
  QGroupBox::showEvent(event);

  if (!isChecked())
    setMaximumHeight(collapsedHeight());
}
