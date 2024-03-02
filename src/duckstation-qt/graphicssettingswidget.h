// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include <QtWidgets/QWidget>

#include "ui_graphicssettingswidget.h"

enum class RenderAPI : u32;
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

  void onAdapterChanged();
  void onAspectRatioChanged();
  void onMSAAModeChanged();
  void onTrueColorChanged();
  void onDownsampleModeChanged();
  void onFullscreenModeChanged();
  void onEnableAnyTextureReplacementsChanged();
  void onEnableVRAMWriteDumpingChanged();

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
};
