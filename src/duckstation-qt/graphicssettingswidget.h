// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <QtWidgets/QWidget>

#include "ui_graphicssettingswidget.h"

#include "util/gpu_device.h"

enum class GPURenderer : u8;

class SettingsWindow;

class GraphicsSettingsWidget : public QWidget
{
  Q_OBJECT

public:
  GraphicsSettingsWidget(SettingsWindow* dialog, QWidget* parent);
  ~GraphicsSettingsWidget();

public Q_SLOTS:
  void onShowDebugSettingsChanged(bool enabled);

private Q_SLOTS:
  void updateRendererDependentOptions();
  void updatePGXPSettingsEnabled();

  void onAspectRatioChanged();
  void updateResolutionDependentOptions();
  void onTrueColorChanged();
  void onDownsampleModeChanged();

  void onMediaCaptureBackendChanged();
  void onMediaCaptureContainerChanged();
  void onMediaCaptureVideoEnabledChanged();
  void onMediaCaptureVideoAutoResolutionChanged();
  void onMediaCaptureAudioEnabledChanged();

  void onEnableTextureCacheChanged();
  void onEnableAnyTextureReplacementsChanged();
  void onTextureReplacementOptionsClicked();

private:
  static constexpr int TAB_INDEX_RENDERING = 0;
  static constexpr int TAB_INDEX_ADVANCED = 1;
  static constexpr int TAB_INDEX_PGXP = 2;
  static constexpr int TAB_INDEX_OSD = 3;
  static constexpr int TAB_INDEX_CAPTURE = 4;
  static constexpr int TAB_INDEX_TEXTURE_REPLACEMENTS = 5;
  static constexpr int TAB_INDEX_DEBUGGING = 6;

  void setupAdditionalUi();
  void removePlatformSpecificUi();

  GPURenderer getEffectiveRenderer() const;
  bool effectiveRendererIsHardware() const;

  void populateGPUAdaptersAndResolutions(RenderAPI render_api);

  Ui::GraphicsSettingsWidget m_ui;

  SettingsWindow* m_dialog;

  GPUDevice::AdapterInfoList m_adapters;
  RenderAPI m_adapters_render_api = RenderAPI::None;
};
