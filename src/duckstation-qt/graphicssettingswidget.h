// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "core/types.h"

#include "util/gpu_device.h"

#include <QtWidgets/QDialog>
#include <QtWidgets/QWidget>

#include "ui_graphicssettingswidget.h"
#include "ui_texturereplacementsettingsdialog.h"

enum class GPURenderer : u8;

class SettingsInterface;

class SettingsWindow;

class GraphicsSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  static constexpr int DEFAULT_MAX_RESOLUTION_SCALE = 16;

  GraphicsSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GraphicsSettingsWidget();

  static void populateUpscalingModes(QComboBox* const cb, int max_scale = DEFAULT_MAX_RESOLUTION_SCALE);

  static QVariant packAspectRatio(DisplayAspectRatio ar);
  static DisplayAspectRatio unpackAspectRatio(const QVariant& var);
  static void createAspectRatioSetting(QComboBox* const cb, QSpinBox* const numerator, QLabel* const separator,
                                       QSpinBox* const denominator, SettingsInterface* const sif);

  void onShowDebugSettingsChanged(bool enabled);

private:
  static constexpr int TAB_INDEX_RENDERING = 0;
  static constexpr int TAB_INDEX_ADVANCED = 1;
  static constexpr int TAB_INDEX_PGXP = 2;
  static constexpr int TAB_INDEX_OSD = 3;
  static constexpr int TAB_INDEX_CAPTURE = 4;
  static constexpr int TAB_INDEX_TEXTURE_REPLACEMENTS = 5;
  static constexpr int TAB_INDEX_DEBUGGING = 6;

  void updateRendererDependentOptions();
  void updatePGXPSettingsEnabled();

  void updateResolutionDependentOptions();
  void onDownsampleModeChanged();
  void onFineCropModeChanged();
  void onFineCropResetClicked();

  void onEnableTextureCacheChanged();
  void onEnableAnyTextureDumpingChanged();
  void onEnableAnyTextureReplacementsChanged();
  void onTextureReplacementOptionsClicked();

  void onGPUThreadChanged();

  void removePlatformSpecificUi();

  GPURenderer getEffectiveRenderer() const;
  bool effectiveRendererIsHardware() const;

  void populateGPUAdaptersAndResolutions(RenderAPI render_api);

  void populateAndConnectUpscalingModes(int max_scale = DEFAULT_MAX_RESOLUTION_SCALE);

  Ui::GraphicsSettingsWidget m_ui;

  SettingsWindow* m_dialog;

  GPUDevice::AdapterInfoList m_adapters;
  RenderAPI m_adapters_render_api = RenderAPI::None;
};

class TextureReplacementSettingsDialog final : public QDialog
{
  Q_OBJECT

public:
  TextureReplacementSettingsDialog(SettingsWindow* settings_window, QWidget* parent);

private:
  void onExportClicked();

  Ui::TextureReplacementSettingsDialog m_ui;
};
