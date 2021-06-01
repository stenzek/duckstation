// https://stackoverflow.com/questions/32476006/how-to-make-an-expandable-collapsable-section-widget-in-qt

#pragma once

#include <QtCore/QParallelAnimationGroup>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

class CollapsibleWidget : public QWidget
{
  Q_OBJECT
private:
  QVBoxLayout mainLayout;
  QToolButton toggleButton;
  QParallelAnimationGroup toggleAnimation;
  QScrollArea contentArea;
  int animationDuration{300};

public:
  explicit CollapsibleWidget(const QString& title = "", const int animationDuration = 300, QWidget* parent = 0);

  QScrollArea* getScrollArea() { return &contentArea; }

  void setContentLayout(QLayout* contentLayout);
};
