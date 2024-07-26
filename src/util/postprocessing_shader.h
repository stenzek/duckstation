// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "postprocessing.h"

#include "gpu_texture.h"

#include "common/gsvector.h"
#include "common/settings_interface.h"
#include "common/timer.h"
#include "common/types.h"
#include "gpu_device.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

class GPUPipeline;
class GPUTexture;
class ProgressCallback;

namespace PostProcessing {

class Shader
{
public:
  Shader();
  Shader(std::string name);
  virtual ~Shader();

  ALWAYS_INLINE const std::string& GetName() const { return m_name; }
  ALWAYS_INLINE const std::vector<ShaderOption>& GetOptions() const { return m_options; }
  ALWAYS_INLINE std::vector<ShaderOption>& GetOptions() { return m_options; }
  ALWAYS_INLINE bool HasOptions() const { return !m_options.empty(); }

  virtual bool IsValid() const = 0;
  virtual bool WantsDepthBuffer() const = 0;

  std::vector<ShaderOption> TakeOptions();
  void LoadOptions(const SettingsInterface& si, const char* section);

  const ShaderOption* GetOptionByName(std::string_view name) const;
  ShaderOption* GetOptionByName(std::string_view name);

  virtual bool ResizeOutput(GPUTexture::Format format, u32 width, u32 height) = 0;

  virtual bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height, ProgressCallback* progress) = 0;

  virtual bool Apply(GPUTexture* input_color, GPUTexture* input_depth, GPUTexture* final_target, GSVector4i final_rect,
                     s32 orig_width, s32 orig_height, s32 native_width, s32 native_height, u32 target_width,
                     u32 target_height) = 0;

protected:
  using OptionList = std::vector<ShaderOption>;

  static void ParseKeyValue(std::string_view line, std::string_view* key, std::string_view* value);

  virtual void OnOptionChanged(const ShaderOption& option);

  std::string m_name;
  OptionList m_options;
};

} // namespace PostProcessing