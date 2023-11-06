// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <QtCore/QMap>
#include <QtWidgets/QWidget>
#include <array>
#include <vector>

class QScrollArea;
class QGridLayout;
class QVBoxLayout;

class ControllerSettingsWindow;

class HotkeySettingsWidget : public QWidget
{
  Q_OBJECT

public:
  HotkeySettingsWidget(QWidget* parent, ControllerSettingsWindow* dialog);
  ~HotkeySettingsWidget();

private:
  void createUi();
  void createButtons();

  ControllerSettingsWindow* m_dialog;
  QScrollArea* m_scroll_area = nullptr;
  QWidget* m_container = nullptr;
  QVBoxLayout* m_layout = nullptr;

  QMap<QString, QGridLayout*> m_categories;
};
