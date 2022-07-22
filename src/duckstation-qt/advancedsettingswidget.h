#pragma once
#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include "ui_advancedsettingswidget.h"

class SettingsDialog;

class AdvancedSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  explicit AdvancedSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~AdvancedSettingsWidget();

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

  SettingsDialog* m_dialog;

  Ui::AdvancedSettingsWidget m_ui;

  void onResetToDefaultClicked();

  QVector<TweakOption> m_tweak_options;
};
