// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtCore/QPropertyAnimation>
#include <QtWidgets/QAbstractButton>

class ToggleButton : public QAbstractButton
{
  Q_OBJECT
  Q_PROPERTY(int offset READ offset WRITE setOffset)

public:
  explicit ToggleButton(QWidget* parent = nullptr);
  ~ToggleButton() override;

  Qt::CheckState checkState() const;

  QSize sizeHint() const override;

Q_SIGNALS:
  void checkStateChanged(Qt::CheckState state);

protected:
  void checkStateSet() override;
  void nextCheckState() override;

  void paintEvent(QPaintEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  void animateToggle(bool checked);
  void updateTogglePosition();

  int offset() const;
  void setOffset(int value);

  int m_offset = 0;
  bool m_hovered = false;

  QPropertyAnimation m_offset_animation;
  QPropertyAnimation m_background_animation;
};
