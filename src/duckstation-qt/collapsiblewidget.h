// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QGroupBox>

class QPropertyAnimation;
class QPaintEvent;
class QShowEvent;

class CollapsibleWidget final : public QGroupBox
{
  Q_OBJECT

public:
  explicit CollapsibleWidget(QWidget* parent = nullptr);
  explicit CollapsibleWidget(const QString& title, QWidget* parent = nullptr);
  ~CollapsibleWidget() override;

  bool isCollapsed() const { return !isChecked(); }
  void setCollapsed(bool collapsed);

protected:
  void changeEvent(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  void init();
  int collapsedHeight() const;
  void updateExpandedHeight();
  void toggleCollapsed(bool expanded);

  QPropertyAnimation* m_animation = nullptr;
  int m_expanded_height = 0;
};
