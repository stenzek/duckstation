// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtCore/QVariantAnimation>
#include <QtWidgets/QStackedWidget>

class SlideTransitionOverlay;

class AnimatedStackedWidget final : public QStackedWidget
{
  Q_OBJECT

public:
  explicit AnimatedStackedWidget(QWidget* parent = nullptr);
  ~AnimatedStackedWidget() override;

public Q_SLOTS:
  void setCurrentIndex(int index);
  void setCurrentWidget(QWidget* widget);

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  void finishAnimation();

  QVariantAnimation m_animation;
  SlideTransitionOverlay* m_overlay = nullptr;
};
