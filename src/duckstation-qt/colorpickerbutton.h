// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/types.h"
#include <QtWidgets/QPushButton>

class ColorPickerButton : public QPushButton
{
  Q_OBJECT

public:
  ColorPickerButton(QWidget* parent);

  quint32 color();
  void setColor(quint32 rgb);

Q_SIGNALS:
  void colorChanged(quint32 new_color);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void onClicked();

  u32 m_color = 0;
};
