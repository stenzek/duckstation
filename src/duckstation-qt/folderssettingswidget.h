// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QWidget>

class SettingsWindow;

class FoldersSettingsWidget final : public QWidget
{
  Q_OBJECT

public:
  FoldersSettingsWidget(SettingsWindow* dialog, QWidget* parent);
};
