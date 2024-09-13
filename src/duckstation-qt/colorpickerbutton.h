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

Q_SIGNALS:
  void colorChanged(quint32 new_color);

public Q_SLOTS:
  quint32 color();
  void setColor(quint32 rgb);

private Q_SLOTS:
  void onClicked();

private:
  void updateBackgroundColor();

  u32 m_color = 0;
};
