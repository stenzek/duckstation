#include "core/host.h"
#include "core/host_settings.h"
#include "common/assert.h"
#include "common/layered_settings_interface.h"

static std::mutex s_settings_mutex;
static LayeredSettingsInterface s_layered_settings_interface;

std::unique_lock<std::mutex> Host::GetSettingsLock()
{
	return std::unique_lock<std::mutex>(s_settings_mutex);
}

SettingsInterface* Host::GetSettingsInterface()
{
	return &s_layered_settings_interface;
}

SettingsInterface* Host::GetSettingsInterfaceForBindings()
{
	SettingsInterface* input_layer = s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_INPUT);
	return input_layer ? input_layer : &s_layered_settings_interface;
}

std::string Host::GetBaseStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->GetStringValue(section, key, default_value);
}

bool Host::GetBaseBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->GetBoolValue(section, key, default_value);
}

s32 Host::GetBaseIntSettingValue(const char* section, const char* key, s32 default_value /*= 0*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->GetIntValue(section, key, default_value);
}

u32 Host::GetBaseUIntSettingValue(const char* section, const char* key, u32 default_value /*= 0*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->GetUIntValue(section, key, default_value);
}

float Host::GetBaseFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->GetFloatValue(section, key, default_value);
}

double Host::GetBaseDoubleSettingValue(const char* section, const char* key, double default_value /* = 0.0f */)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->GetDoubleValue(section, key, default_value);
}

std::vector<std::string> Host::GetBaseStringListSetting(const char* section, const char* key)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE)->GetStringList(section, key);
}

std::string Host::GetStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetStringValue(section, key, default_value);
}

bool Host::GetBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetBoolValue(section, key, default_value);
}

s32 Host::GetIntSettingValue(const char* section, const char* key, s32 default_value /*= 0*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetIntValue(section, key, default_value);
}

u32 Host::GetUIntSettingValue(const char* section, const char* key, u32 default_value /*= 0*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetUIntValue(section, key, default_value);
}

float Host::GetFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetFloatValue(section, key, default_value);
}

double Host::GetDoubleSettingValue(const char* section, const char* key, double default_value /*= 0.0f*/)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetDoubleValue(section, key, default_value);
}

std::vector<std::string> Host::GetStringListSetting(const char* section, const char* key)
{
	std::unique_lock lock(s_settings_mutex);
	return s_layered_settings_interface.GetStringList(section, key);
}

SettingsInterface* Host::Internal::GetBaseSettingsLayer()
{
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE);
}

SettingsInterface* Host::Internal::GetGameSettingsLayer()
{
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_GAME);
}

SettingsInterface* Host::Internal::GetInputSettingsLayer()
{
	return s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_INPUT);
}

void Host::Internal::SetBaseSettingsLayer(SettingsInterface* sif)
{
	AssertMsg(s_layered_settings_interface.GetLayer(LayeredSettingsInterface::LAYER_BASE) == nullptr, "Base layer has not been set");
	s_layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_BASE, sif);
}

void Host::Internal::SetGameSettingsLayer(SettingsInterface* sif)
{
	std::unique_lock lock(s_settings_mutex);
	s_layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_GAME, sif);
}

void Host::Internal::SetInputSettingsLayer(SettingsInterface* sif)
{
	std::unique_lock lock(s_settings_mutex);
	s_layered_settings_interface.SetLayer(LayeredSettingsInterface::LAYER_INPUT, sif);
}
