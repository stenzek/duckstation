// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once

#include "postprocessing.h"

#include "gpu_texture.h"

#include "common/rectangle.h"
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

  std::vector<ShaderOption> TakeOptions();
  void LoadOptions(const SettingsInterface& si, const char* section);

  const ShaderOption* GetOptionByName(const std::string_view& name) const;
  ShaderOption* GetOptionByName(const std::string_view& name);

  virtual bool ResizeOutput(GPUTexture::Format format, u32 width, u32 height) = 0;

  virtual bool CompilePipeline(GPUTexture::Format format, u32 width, u32 height) = 0;

  virtual bool Apply(GPUTexture* input, GPUTexture* final_target, s32 final_left, s32 final_top, s32 final_width,
                     s32 final_height, s32 orig_width, s32 orig_height, u32 target_width, u32 target_height) = 0;

protected:
  static void ParseKeyValue(const std::string_view& line, std::string_view* key, std::string_view* value);

  virtual void OnOptionChanged(const ShaderOption& option);

  std::string m_name;
  std::vector<ShaderOption> m_options;
};

} // namespace PostProcessing