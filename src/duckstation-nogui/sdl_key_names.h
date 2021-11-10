#pragma once
#include "common/string.h"
#include "common/types.h"
#include <SDL.h>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <string_view>

namespace SDLKeyNames {

enum : u32
{
  MODIFIER_SHIFT = 16,
  KEY_MASK = ((1 << MODIFIER_SHIFT) - 1),
  MODIFIER_MASK = ~KEY_MASK,
};

static const std::map<int, const char*> s_sdl_key_names = {{SDLK_RETURN, "Return"},
                                                           {SDLK_ESCAPE, "Escape"},
                                                           {SDLK_BACKSPACE, "Backspace"},
                                                           {SDLK_TAB, "Tab"},
                                                           {SDLK_SPACE, "Space"},
                                                           {SDLK_EXCLAIM, "Exclam"},
                                                           {SDLK_QUOTEDBL, "QuoteDbl"},
                                                           {SDLK_HASH, "Hash"},
                                                           {SDLK_PERCENT, "Percent"},
                                                           {SDLK_DOLLAR, "Dollar"},
                                                           {SDLK_AMPERSAND, "Ampersand"},
                                                           {SDLK_QUOTE, "Apostrophe"},
                                                           {SDLK_LEFTPAREN, "ParenLeft"},
                                                           {SDLK_RIGHTPAREN, "ParenRight"},
                                                           {SDLK_ASTERISK, "Asterisk"},
                                                           {SDLK_PLUS, "PLus"},
                                                           {SDLK_COMMA, "Comma"},
                                                           {SDLK_MINUS, "Minus"},
                                                           {SDLK_PERIOD, "Period"},
                                                           {SDLK_SLASH, "Slash"},
                                                           {SDLK_0, "0"},
                                                           {SDLK_1, "1"},
                                                           {SDLK_2, "2"},
                                                           {SDLK_3, "3"},
                                                           {SDLK_4, "4"},
                                                           {SDLK_5, "5"},
                                                           {SDLK_6, "6"},
                                                           {SDLK_7, "7"},
                                                           {SDLK_8, "8"},
                                                           {SDLK_9, "9"},
                                                           {SDLK_COLON, "Colon"},
                                                           {SDLK_SEMICOLON, "Semcolon"},
                                                           {SDLK_LESS, "Less"},
                                                           {SDLK_EQUALS, "Equal"},
                                                           {SDLK_GREATER, "Greater"},
                                                           {SDLK_QUESTION, "Question"},
                                                           {SDLK_AT, "AT"},
                                                           {SDLK_LEFTBRACKET, "BracketLeft"},
                                                           {SDLK_BACKSLASH, "Backslash"},
                                                           {SDLK_RIGHTBRACKET, "BracketRight"},
                                                           {SDLK_CARET, "Caret"},
                                                           {SDLK_UNDERSCORE, "Underscore"},
                                                           {SDLK_BACKQUOTE, "QuoteLeft"},
                                                           {SDLK_a, "A"},
                                                           {SDLK_b, "B"},
                                                           {SDLK_c, "C"},
                                                           {SDLK_d, "D"},
                                                           {SDLK_e, "E"},
                                                           {SDLK_f, "F"},
                                                           {SDLK_g, "G"},
                                                           {SDLK_h, "H"},
                                                           {SDLK_i, "I"},
                                                           {SDLK_j, "J"},
                                                           {SDLK_k, "K"},
                                                           {SDLK_l, "L"},
                                                           {SDLK_m, "M"},
                                                           {SDLK_n, "N"},
                                                           {SDLK_o, "O"},
                                                           {SDLK_p, "P"},
                                                           {SDLK_q, "Q"},
                                                           {SDLK_r, "R"},
                                                           {SDLK_s, "S"},
                                                           {SDLK_t, "T"},
                                                           {SDLK_u, "U"},
                                                           {SDLK_v, "V"},
                                                           {SDLK_w, "W"},
                                                           {SDLK_x, "X"},
                                                           {SDLK_y, "Y"},
                                                           {SDLK_z, "Z"},
                                                           {SDLK_CAPSLOCK, "CapsLock"},
                                                           {SDLK_F1, "F1"},
                                                           {SDLK_F2, "F2"},
                                                           {SDLK_F3, "F3"},
                                                           {SDLK_F4, "F4"},
                                                           {SDLK_F5, "F5"},
                                                           {SDLK_F6, "F6"},
                                                           {SDLK_F7, "F7"},
                                                           {SDLK_F8, "F8"},
                                                           {SDLK_F9, "F9"},
                                                           {SDLK_F10, "F10"},
                                                           {SDLK_F11, "F11"},
                                                           {SDLK_F12, "F12"},
                                                           {SDLK_PRINTSCREEN, "Print"},
                                                           {SDLK_SCROLLLOCK, "ScrollLock"},
                                                           {SDLK_PAUSE, "Pause"},
                                                           {SDLK_INSERT, "Insert"},
                                                           {SDLK_HOME, "Home"},
                                                           {SDLK_PAGEUP, "PageUp"},
                                                           {SDLK_DELETE, "Delete"},
                                                           {SDLK_END, "End"},
                                                           {SDLK_PAGEDOWN, "PageDown"},
                                                           {SDLK_RIGHT, "Right"},
                                                           {SDLK_LEFT, "Left"},
                                                           {SDLK_DOWN, "Down"},
                                                           {SDLK_UP, "Up"},
                                                           {SDLK_NUMLOCKCLEAR, "NumLock"},
                                                           {SDLK_KP_DIVIDE, "Keypad+Divide"},
                                                           {SDLK_KP_MULTIPLY, "Keypad+Multiply"},
                                                           {SDLK_KP_MINUS, "Keypad+Minus"},
                                                           {SDLK_KP_PLUS, "Keypad+Plus"},
                                                           {SDLK_KP_ENTER, "Keypad+Return"},
                                                           {SDLK_KP_1, "Keypad+1"},
                                                           {SDLK_KP_2, "Keypad+2"},
                                                           {SDLK_KP_3, "Keypad+3"},
                                                           {SDLK_KP_4, "Keypad+4"},
                                                           {SDLK_KP_5, "Keypad+5"},
                                                           {SDLK_KP_6, "Keypad+6"},
                                                           {SDLK_KP_7, "Keypad+7"},
                                                           {SDLK_KP_8, "Keypad+8"},
                                                           {SDLK_KP_9, "Keypad+9"},
                                                           {SDLK_KP_0, "Keypad+0"},
                                                           {SDLK_KP_PERIOD, "Keypad+Period"},
                                                           {SDLK_APPLICATION, "Application"},
                                                           {SDLK_POWER, "Power"},
                                                           {SDLK_KP_EQUALS, "Keypad+Equal"},
                                                           {SDLK_F13, "F13"},
                                                           {SDLK_F14, "F14"},
                                                           {SDLK_F15, "F15"},
                                                           {SDLK_F16, "F16"},
                                                           {SDLK_F17, "F17"},
                                                           {SDLK_F18, "F18"},
                                                           {SDLK_F19, "F19"},
                                                           {SDLK_F20, "F20"},
                                                           {SDLK_F21, "F21"},
                                                           {SDLK_F22, "F22"},
                                                           {SDLK_F23, "F23"},
                                                           {SDLK_F24, "F24"},
                                                           {SDLK_EXECUTE, "Execute"},
                                                           {SDLK_HELP, "Help"},
                                                           {SDLK_MENU, "Menu"},
                                                           {SDLK_SELECT, "Select"},
                                                           {SDLK_STOP, "Stop"},
                                                           {SDLK_AGAIN, "Again"},
                                                           {SDLK_UNDO, "Undo"},
                                                           {SDLK_CUT, "Cut"},
                                                           {SDLK_COPY, "Copy"},
                                                           {SDLK_PASTE, "Paste"},
                                                           {SDLK_FIND, "Find"},
                                                           {SDLK_MUTE, "Mute"},
                                                           {SDLK_VOLUMEUP, "VolumeUp"},
                                                           {SDLK_VOLUMEDOWN, "VolumeDown"},
                                                           {SDLK_KP_COMMA, "Keypad+Comma"},
                                                           {SDLK_KP_EQUALSAS400, "Keypad+EqualAS400"},
                                                           {SDLK_ALTERASE, "AltErase"},
                                                           {SDLK_SYSREQ, "SysReq"},
                                                           {SDLK_CANCEL, "Cancel"},
                                                           {SDLK_CLEAR, "Clear"},
                                                           {SDLK_PRIOR, "Prior"},
                                                           {SDLK_RETURN2, "Return2"},
                                                           {SDLK_SEPARATOR, "Separator"},
                                                           {SDLK_OUT, "Out"},
                                                           {SDLK_OPER, "Oper"},
                                                           {SDLK_CLEARAGAIN, "ClearAgain"},
                                                           {SDLK_CRSEL, "CrSel"},
                                                           {SDLK_EXSEL, "ExSel"},
                                                           {SDLK_KP_00, "Keypad+00"},
                                                           {SDLK_KP_000, "Keypad+000"},
                                                           {SDLK_THOUSANDSSEPARATOR, "ThousandsSeparator"},
                                                           {SDLK_DECIMALSEPARATOR, "DecimalSeparator"},
                                                           {SDLK_CURRENCYUNIT, "CurrencyUnit"},
                                                           {SDLK_CURRENCYSUBUNIT, "CurrencySubunit"},
                                                           {SDLK_KP_LEFTPAREN, "Keypad+ParenLeft"},
                                                           {SDLK_KP_RIGHTPAREN, "Keypad+ParenRight"},
                                                           {SDLK_KP_LEFTBRACE, "Keypad+LeftBrace"},
                                                           {SDLK_KP_RIGHTBRACE, "Keypad+RightBrace"},
                                                           {SDLK_KP_TAB, "Keypad+Tab"},
                                                           {SDLK_KP_BACKSPACE, "Keypad+Backspace"},
                                                           {SDLK_KP_A, "Keypad+A"},
                                                           {SDLK_KP_B, "Keypad+B"},
                                                           {SDLK_KP_C, "Keypad+C"},
                                                           {SDLK_KP_D, "Keypad+D"},
                                                           {SDLK_KP_E, "Keypad+E"},
                                                           {SDLK_KP_F, "Keypad+F"},
                                                           {SDLK_KP_XOR, "Keypad+XOR"},
                                                           {SDLK_KP_POWER, "Keypad+Power"},
                                                           {SDLK_KP_PERCENT, "Keypad+Percent"},
                                                           {SDLK_KP_LESS, "Keypad+Less"},
                                                           {SDLK_KP_GREATER, "Keypad+Greater"},
                                                           {SDLK_KP_AMPERSAND, "Keypad+Ampersand"},
                                                           {SDLK_KP_DBLAMPERSAND, "Keypad+AmpersandDbl"},
                                                           {SDLK_KP_VERTICALBAR, "Keypad+Bar"},
                                                           {SDLK_KP_DBLVERTICALBAR, "Keypad+BarDbl"},
                                                           {SDLK_KP_COLON, "Keypad+Colon"},
                                                           {SDLK_KP_HASH, "Keypad+Hash"},
                                                           {SDLK_KP_SPACE, "Keypad+Space"},
                                                           {SDLK_KP_AT, "Keypad+At"},
                                                           {SDLK_KP_EXCLAM, "Keypad+Exclam"},
                                                           {SDLK_KP_MEMSTORE, "Keypad+MemStore"},
                                                           {SDLK_KP_MEMRECALL, "Keypad+MemRecall"},
                                                           {SDLK_KP_MEMCLEAR, "Keypad+MemClear"},
                                                           {SDLK_KP_MEMADD, "Keypad+MemAdd"},
                                                           {SDLK_KP_MEMSUBTRACT, "Keypad+MemSubtract"},
                                                           {SDLK_KP_MEMMULTIPLY, "Keypad+MemMultiply"},
                                                           {SDLK_KP_MEMDIVIDE, "Keypad+MemDivide"},
                                                           {SDLK_KP_PLUSMINUS, "Keypad+PlusMinus"},
                                                           {SDLK_KP_CLEAR, "Keypad+Clear"},
                                                           {SDLK_KP_CLEARENTRY, "Keypad+ClearEntry"},
                                                           {SDLK_KP_BINARY, "Keypad+Binary"},
                                                           {SDLK_KP_OCTAL, "Keypad+Octal"},
                                                           {SDLK_KP_DECIMAL, "Keypad+Decimal"},
                                                           {SDLK_KP_HEXADECIMAL, "Keypad+Hexadecimal"},
                                                           {SDLK_LCTRL, "LeftControl"},
                                                           {SDLK_LSHIFT, "LeftShift"},
                                                           {SDLK_LALT, "LeftAlt"},
                                                           {SDLK_LGUI, "Super_L"},
                                                           {SDLK_RCTRL, "RightCtrl"},
                                                           {SDLK_RSHIFT, "RightShift"},
                                                           {SDLK_RALT, "RightAlt"},
                                                           {SDLK_RGUI, "RightSuper"},
                                                           {SDLK_MODE, "Mode"},
                                                           {SDLK_AUDIONEXT, "MediaNext"},
                                                           {SDLK_AUDIOPREV, "MediaPrevious"},
                                                           {SDLK_AUDIOSTOP, "MediaStop"},
                                                           {SDLK_AUDIOPLAY, "MediaPlay"},
                                                           {SDLK_AUDIOMUTE, "VolumeMute"},
                                                           {SDLK_MEDIASELECT, "MediaSelect"},
                                                           {SDLK_WWW, "WWW"},
                                                           {SDLK_MAIL, "Mail"},
                                                           {SDLK_CALCULATOR, "Calculator"},
                                                           {SDLK_COMPUTER, "Computer"},
                                                           {SDLK_AC_SEARCH, "Search"},
                                                           {SDLK_AC_HOME, "Home"},
                                                           {SDLK_AC_BACK, "Back"},
                                                           {SDLK_AC_FORWARD, "Forward"},
                                                           {SDLK_AC_STOP, "Stop"},
                                                           {SDLK_AC_REFRESH, "Refresh"},
                                                           {SDLK_AC_BOOKMARKS, "Bookmarks"},
                                                           {SDLK_BRIGHTNESSDOWN, "BrightnessDown"},
                                                           {SDLK_BRIGHTNESSUP, "BrightnessUp"},
                                                           {SDLK_DISPLAYSWITCH, "DisplaySwitch"},
                                                           {SDLK_KBDILLUMTOGGLE, "IllumToggle"},
                                                           {SDLK_KBDILLUMDOWN, "IllumDown"},
                                                           {SDLK_KBDILLUMUP, "IllumUp"},
                                                           {SDLK_EJECT, "Eject"},
                                                           {SDLK_SLEEP, "Sleep"},
                                                           {SDLK_APP1, "App1"},
                                                           {SDLK_APP2, "App2"},
                                                           {SDLK_AUDIOREWIND, "MediaRewind"},
                                                           {SDLK_AUDIOFASTFORWARD, "MediaFastForward"}};

struct SDLKeyModifierEntry
{
  SDL_Keymod mod;
  SDL_Keymod mod_mask;
  SDL_Keycode key_left;
  SDL_Keycode key_right;
  const char* name;
};

static const std::array<SDLKeyModifierEntry, 4> s_sdl_key_modifiers = {
  {{KMOD_LSHIFT, static_cast<SDL_Keymod>(KMOD_LSHIFT | KMOD_RSHIFT), SDLK_LSHIFT, SDLK_RSHIFT, "Shift"},
   {KMOD_LCTRL, static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_RCTRL), SDLK_LCTRL, SDLK_RCTRL, "Control"},
   {KMOD_LALT, static_cast<SDL_Keymod>(KMOD_LALT | KMOD_RALT), SDLK_LALT, SDLK_RALT, "Alt"},
   {KMOD_LGUI, static_cast<SDL_Keymod>(KMOD_LGUI | KMOD_RGUI), SDLK_LGUI, SDLK_RGUI, "Meta"}}};

static const char* GetKeyName(SDL_Keycode key)
{
  const auto it = s_sdl_key_names.find(key);
  return it == s_sdl_key_names.end() ? nullptr : it->second;
}

static std::optional<SDL_Keycode> GetKeyCodeForName(const std::string_view key_name)
{
  for (const auto& it : s_sdl_key_names)
  {
    if (key_name == it.second)
      return it.first;
  }

  return std::nullopt;
}

static u32 KeyEventToInt(const SDL_Event* event)
{
  u32 code = static_cast<u32>(event->key.keysym.sym);

  const SDL_Keymod mods = static_cast<SDL_Keymod>(event->key.keysym.mod);
  if (mods & (KMOD_LSHIFT | KMOD_RSHIFT))
    code |= static_cast<u32>(KMOD_LSHIFT) << MODIFIER_SHIFT;
  if (mods & (KMOD_LCTRL | KMOD_RCTRL))
    code |= static_cast<u32>(KMOD_LCTRL) << MODIFIER_SHIFT;
  if (mods & (KMOD_LALT | KMOD_RALT))
    code |= static_cast<u32>(KMOD_LALT) << MODIFIER_SHIFT;
  if (mods & (KMOD_LGUI | KMOD_RGUI))
    code |= static_cast<u32>(KMOD_LGUI) << MODIFIER_SHIFT;

  return code;
}

static bool KeyEventToString(const SDL_Event* event, String& out_string)
{
  const SDL_Keycode key = event->key.keysym.sym;
  const SDL_Keymod mods = static_cast<SDL_Keymod>(event->key.keysym.mod);
  const char* key_name = GetKeyName(event->key.keysym.sym);
  if (!key_name)
    return false;

  out_string.Clear();

  for (const SDLKeyModifierEntry& mod : s_sdl_key_modifiers)
  {
    if (mods & mod.mod_mask && key != mod.key_left && key != mod.key_right)
    {
      out_string.AppendString(mod.name);
      out_string.AppendCharacter('+');
    }
  }

  out_string.AppendString(key_name);
  return true;
}

static std::optional<u32> ParseKeyString(const std::string_view key_str)
{
  u32 modifiers = 0;
  std::string_view::size_type pos = 0;
  for (;;)
  {
    std::string_view::size_type plus_pos = key_str.find('+', pos);
    if (plus_pos == std::string_view::npos)
      break;

    const std::string_view mod_part = key_str.substr(pos, plus_pos - pos);
    
    // Keypad in SDL is not a mod and should always be the last + in the string
    bool known_mod = false;
    for (const SDLKeyModifierEntry& mod : s_sdl_key_modifiers)
    {
      if (mod_part == mod.name)
      {
        modifiers |= static_cast<int>(mod.mod);
        known_mod = true;
        break;
      }
    }

    if (!known_mod)
      break;

    pos = plus_pos + 1;
  }

  std::optional<SDL_Keycode> key_code = GetKeyCodeForName(key_str.substr(pos));
  if (!key_code)
    return std::nullopt;

  return static_cast<u32>(key_code.value()) | (modifiers << MODIFIER_SHIFT);
}
} // namespace SDLKeyNames
