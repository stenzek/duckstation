// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QRadioButton>

class CardRadioButton final : public QRadioButton
{
  Q_OBJECT

public:
  explicit CardRadioButton(QWidget* parent = nullptr);
  ~CardRadioButton() override;

  QSize sizeHint() const override;

protected:
  bool hitButton(const QPoint& pos) const override;
  void paintEvent(QPaintEvent* event) override;
};
