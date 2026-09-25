// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include <SDL3/SDL.h>

class Error;

#define DYN_SDL_FUNCTIONS(X)                                                                                           \
  X(SDL_GetError)                                                                                                      \
  X(SDL_SetLogOutputFunction)                                                                                          \
  X(SDL_SetLogPriorities)                                                                                              \
  X(SDL_SetHint)                                                                                                       \
  X(SDL_InitSubSystem)                                                                                                 \
  X(SDL_QuitSubSystem)                                                                                                 \
  X(SDL_PollEvent)                                                                                                     \
  X(SDL_free)                                                                                                          \
  X(SDL_OpenAudioDeviceStream)                                                                                         \
  X(SDL_DestroyAudioStream)                                                                                            \
  X(SDL_ResumeAudioStreamDevice)                                                                                       \
  X(SDL_PauseAudioStreamDevice)                                                                                        \
  X(SDL_PutAudioStreamData)                                                                                            \
  X(SDL_GetBooleanProperty)                                                                                            \
  X(SDL_GUIDToString)                                                                                                  \
  X(SDL_OpenGamepad)                                                                                                   \
  X(SDL_CloseGamepad)                                                                                                  \
  X(SDL_GetGamepadType)                                                                                                \
  X(SDL_GetGamepadButtonLabelForType)                                                                                  \
  X(SDL_GetGamepadVendor)                                                                                              \
  X(SDL_GetGamepadProduct)                                                                                             \
  X(SDL_SetGamepadLED)                                                                                                 \
  X(SDL_GetGamepadMappings)                                                                                            \
  X(SDL_GetGamepadJoystick)                                                                                            \
  X(SDL_GetGamepadName)                                                                                                \
  X(SDL_GetGamepadBindings)                                                                                            \
  X(SDL_SetGamepadSensorEnabled)                                                                                       \
  X(SDL_GetGamepadPlayerIndex)                                                                                         \
  X(SDL_GetGamepadProperties)                                                                                          \
  X(SDL_GetGamepadAxis)                                                                                                \
  X(SDL_GetGamepadButton)                                                                                              \
  X(SDL_SendGamepadEffect)                                                                                             \
  X(SDL_GamepadHasSensor)                                                                                              \
  X(SDL_RumbleGamepad)                                                                                                 \
  X(SDL_IsGamepad)                                                                                                     \
  X(SDL_OpenJoystick)                                                                                                  \
  X(SDL_CloseJoystick)                                                                                                 \
  X(SDL_GetJoystickName)                                                                                               \
  X(SDL_GetJoystickPlayerIndex)                                                                                        \
  X(SDL_GetJoystickGUID)                                                                                               \
  X(SDL_GetJoystickSerial)                                                                                             \
  X(SDL_GetJoystickPath)                                                                                               \
  X(SDL_GetJoystickProperties)                                                                                         \
  X(SDL_GetJoystickID)                                                                                                 \
  X(SDL_GetJoystickAxis)                                                                                               \
  X(SDL_GetJoystickButton)                                                                                             \
  X(SDL_GetJoystickHat)                                                                                                \
  X(SDL_GetNumJoystickAxes)                                                                                            \
  X(SDL_GetNumJoystickButtons)                                                                                         \
  X(SDL_GetNumJoystickHats)                                                                                            \
  X(SDL_OpenHapticFromJoystick)                                                                                        \
  X(SDL_CloseHaptic)                                                                                                   \
  X(SDL_HapticRumbleSupported)                                                                                         \
  X(SDL_InitHapticRumble)                                                                                              \
  X(SDL_PlayHapticRumble)                                                                                              \
  X(SDL_StopHapticRumble)                                                                                              \
  X(SDL_GetHapticFeatures)                                                                                             \
  X(SDL_CreateHapticEffect)                                                                                            \
  X(SDL_UpdateHapticEffect)                                                                                            \
  X(SDL_DestroyHapticEffect)                                                                                           \
  X(SDL_RunHapticEffect)                                                                                               \
  X(SDL_StopHapticEffect)

struct DynSDL
{
#define ADD_FUNC(F) decltype(&::F) F;
  DYN_SDL_FUNCTIONS(ADD_FUNC)
#undef ADD_FUNC

  bool Open(Error* const error);
  bool InitSubSystem(SDL_InitFlags flags, Error* const error);
  void QuitSubSystem(SDL_InitFlags flags);
};

extern DynSDL g_dyn_sdl;
