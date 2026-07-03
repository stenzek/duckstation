// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "animatedstackedwidget.h"

#include <QtGui/QPainter>
#include <QtWidgets/QStyle>
#include <utility>

#include "moc_animatedstackedwidget.cpp"

class SlideTransitionOverlay final : public QWidget
{
public:
  explicit SlideTransitionOverlay(QWidget* parent) : QWidget(parent) { setAttribute(Qt::WA_TransparentForMouseEvents); }

  void start(QPixmap outgoing, QPixmap incoming, bool forward)
  {
    m_outgoing = std::move(outgoing);
    m_incoming = std::move(incoming);
    m_direction = forward ? 1 : -1;
    m_progress = 0.0;
    show();
    raise();
  }

  void setProgress(qreal progress)
  {
    m_progress = progress;
    update();
  }

  void finish()
  {
    hide();
    m_outgoing = {};
    m_incoming = {};
  }

protected:
  void paintEvent(QPaintEvent* event) override
  {
    Q_UNUSED(event);

    QPainter painter(this);
    const int offset = qRound(static_cast<qreal>(width()) * m_progress);
    painter.drawPixmap(QPoint(-m_direction * offset, 0), m_outgoing);
    painter.drawPixmap(QPoint(m_direction * (width() - offset), 0), m_incoming);
  }

private:
  QPixmap m_outgoing;
  QPixmap m_incoming;
  qreal m_progress = 0.0;
  int m_direction = 1;
};

static QPixmap GrabPage(QWidget* page)
{
  const qreal dpr = page->devicePixelRatioF();
  QPixmap pixmap((QSizeF(page->size()) * dpr).toSize());
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(page->palette().window().color());
  page->render(&pixmap);
  return pixmap;
}

AnimatedStackedWidget::AnimatedStackedWidget(QWidget* parent) : QStackedWidget(parent), m_animation(this)
{
  m_overlay = new SlideTransitionOverlay(this);
  m_overlay->hide();

  m_animation.setDuration(style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, this));
  m_animation.setEasingCurve(QEasingCurve::OutCubic);
  connect(&m_animation, &QVariantAnimation::valueChanged, this,
          [this](const QVariant& value) { m_overlay->setProgress(value.toReal()); });
  connect(&m_animation, &QVariantAnimation::finished, this, &AnimatedStackedWidget::finishAnimation);
}

AnimatedStackedWidget::~AnimatedStackedWidget() = default;

void AnimatedStackedWidget::setCurrentIndex(int index)
{
  const int previous_index = currentIndex();
  if (index == previous_index || index < 0 || index >= count())
    return;

  finishAnimation();

  if (!isVisible() || style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, this) <= 0)
  {
    QStackedWidget::setCurrentIndex(index);
    return;
  }

  const QPixmap outgoing = GrabPage(currentWidget());
  QStackedWidget::setCurrentIndex(index);
  const QPixmap incoming = GrabPage(currentWidget());

  m_overlay->setGeometry(contentsRect());
  m_overlay->start(outgoing, incoming, index > previous_index);
  m_animation.setStartValue(0.0);
  m_animation.setEndValue(1.0);
  m_animation.start();
}

void AnimatedStackedWidget::setCurrentWidget(QWidget* widget)
{
  setCurrentIndex(indexOf(widget));
}

void AnimatedStackedWidget::resizeEvent(QResizeEvent* event)
{
  finishAnimation();
  QStackedWidget::resizeEvent(event);
}

void AnimatedStackedWidget::finishAnimation()
{
  m_animation.stop();
  m_overlay->finish();
}
