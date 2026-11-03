// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "collapsiblewidget.h"
#include "qthost.h"

#include <QtCore/QPropertyAnimation>
#include <QtGui/QPaintEvent>
#include <QtGui/QRegion>
#include <QtGui/QShowEvent>
#include <QtWidgets/QApplication>
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

void CollapsibleWidget::initHeaderStyleOption(QStyleOptionGroupBox* option) const
{
  initStyleOption(option);

#ifdef __APPLE__
  // QMacStyle forces State_Mini on non-flat group boxes when drawing, and positions the label based on the mini
  // checkbox size. Match that here, otherwise the full-size checkbox rect overlaps the start of the label.
  if (!(option->features & QStyleOptionFrame::Flat))
    option->state |= QStyle::State_Mini;
#endif
}

int CollapsibleWidget::collapsedHeight() const
{
  QStyleOptionGroupBox option;
  initHeaderStyleOption(&option);

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
  initHeaderStyleOption(&option);

  const QRect indicator_rect = style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxCheckBox, this);
  if (!isChecked())
    option.subControls &= ~QStyle::SC_GroupBoxFrame;

#ifdef __APPLE__
  if (QtHost::IsNativeApplicationTheme())
  {
    // The native checkbox is drawn larger than its reported subcontrol rect, so it can't be clipped out. Instead,
    // draw the frame and label ourselves, mirroring QMacStyle, using the style's layout for positioning.
    const QRect label_rect = style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxLabel, this);
    if (option.subControls & QStyle::SC_GroupBoxFrame)
    {
      QStyleOptionFrame frame;
      frame.QStyleOption::operator=(option);
      frame.features = option.features;
      frame.lineWidth = option.lineWidth;
      frame.midLineWidth = option.midLineWidth;
      frame.rect = style()->subControlRect(QStyle::CC_GroupBox, &option, QStyle::SC_GroupBoxFrame, this);
      painter.drawPrimitive(QStyle::PE_FrameGroupBox, frame);
    }

    // QMacStyle uses the small system font for non-flat group box titles, unless a font was explicitly set.
    if (!(option.features & QStyleOptionFrame::Flat) && !testAttribute(Qt::WA_SetFont))
      painter.setFont(QApplication::font("QSmallFont"));

    // QMacStyle draws the arrow centered 2px below the indicator rect's center, so vertically center the text on that
    // rather than top-aligning it in the label rect, which sits higher.
    QRect text_rect = label_rect;
    text_rect.setTop(indicator_rect.top() + 2);
    text_rect.setBottom(indicator_rect.bottom() + 2);
    painter.drawItemText(text_rect, Qt::TextHideMnemonic | Qt::AlignLeft | Qt::AlignVCenter, option.palette,
                         isEnabled(), option.text, QPalette::WindowText);
    painter.setFont(font());
  }
  else
#endif
  {
    const QRegion group_box_region = QRegion(rect()).subtracted(indicator_rect.adjusted(-1, -1, 1, 1));
    painter.setClipRegion(group_box_region);
    painter.drawComplexControl(QStyle::CC_GroupBox, option);
    painter.setClipping(false);
  }

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
