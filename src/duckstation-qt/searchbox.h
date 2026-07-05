// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QLineEdit>

class SearchBox final : public QLineEdit
{
  Q_OBJECT

public:
  explicit SearchBox(QWidget* parent = nullptr);
  ~SearchBox() override;

protected:
  void keyPressEvent(QKeyEvent* event) override;
};
