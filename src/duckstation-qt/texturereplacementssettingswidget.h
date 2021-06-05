#pragma once
#include "ui_texturereplacementssettingswidget.h"
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <array>

class SettingsDialog;

class TextureReplacementSettingsWidget final : public QWidget
{
  Q_OBJECT

public:
  TextureReplacementSettingsWidget(SettingsDialog* dialog, QWidget* parent);
  ~TextureReplacementSettingsWidget();

private Q_SLOTS:
  void setDefaults();
  void updateOptionsEnabled();
  void openDumpDirectory();
  void updateVRAMUsage();

private:
  void connectUi();

  SettingsDialog* m_dialog;

  Ui::TextureReplacementSettingsWidget m_ui;
};
