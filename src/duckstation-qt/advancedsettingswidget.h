// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include "ui_advancedsettingswidget.h"

class SettingsWindow;

class AdvancedSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AdvancedSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~AdvancedSettingsWidget();

Q_SIGNALS:
  void onShowDebugOptionsChanged(bool enabled);

private Q_SLOTS:
  void onShowDebugOptionsStateChanged();

private:
  struct TweakOption
  {
    enum class Type
    {
      Boolean,
      IntRange
    };

    Type type;
    QString description;
    std::string key;
    std::string section;

    union
    {
      struct
      {
        bool default_value;
      } boolean;

      struct
      {
        int min_value;
        int max_value;
        int default_value;
      } int_range;
    };
  };

  SettingsWindow* m_dialog;

  Ui::AdvancedSettingsWidget m_ui;

  QVector<TweakOption> m_tweak_options;

  void addTweakOptions();
  void onResetToDefaultClicked();
};
