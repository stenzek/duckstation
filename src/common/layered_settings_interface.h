// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "settings_interface.h"
#include <array>

class LayeredSettingsInterface final : public SettingsInterface
{
public:
  enum Layer : u32
  {
    LAYER_GAME,
    LAYER_INPUT,
    LAYER_BASE,
    NUM_LAYERS
  };

  LayeredSettingsInterface();
  ~LayeredSettingsInterface() override;

  SettingsInterface* GetLayer(Layer layer) const { return m_layers[layer]; }
  void SetLayer(Layer layer, SettingsInterface* sif) { m_layers[layer] = sif; }

  bool IsEmpty() override;

  bool LookupValue(const char* section, const char* key, std::string_view* value) const override;
  void StoreValue(const char* section, const char* key, std::string_view value) override;
  bool ContainsValue(const char* section, const char* key) const override;
  void DeleteValue(const char* section, const char* key) override;
  void ClearSection(const char* section) override;
  void RemoveSection(const char* section) override;
  void RemoveEmptySections() override;

  std::vector<std::string> GetStringList(const char* section, const char* key) const override;
  void SetStringList(const char* section, const char* key, const std::vector<std::string>& items) override;
  bool RemoveFromStringList(const char* section, const char* key, const char* item) override;
  bool AddToStringList(const char* section, const char* key, const char* item) override;

  std::vector<std::pair<std::string, std::string>> GetKeyValueList(const char* section) const override;
  void SetKeyValueList(const char* section, const std::vector<std::pair<std::string, std::string>>& items) override;

private:
  static constexpr Layer FIRST_LAYER = LAYER_GAME;
  static constexpr Layer LAST_LAYER = LAYER_BASE;

  std::array<SettingsInterface*, NUM_LAYERS> m_layers{};
};