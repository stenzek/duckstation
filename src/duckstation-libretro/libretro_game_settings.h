#include "frontend-common/game_settings.h"
#include <memory>

std::unique_ptr<GameSettings::Entry> GetSettingsForGame(const std::string& game_code);
