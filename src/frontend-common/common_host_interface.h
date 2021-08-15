#pragma once
#include "common/bitfield.h"
#include "common/string.h"
#include "core/controller.h"
#include "core/host_interface.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

class HostDisplayTexture;

class GameList;
struct GameDatabaseEntry;

class ControllerInterface;

namespace FrontendCommon {
class SaveStateSelectorUI;

enum class ControllerNavigationButton : u32
{
  Activate,      // A on XBox Controller, Cross on PS Controller
  Cancel,        // B on XBox Controller, Circle on PS Controller
  LeftShoulder,  // LB on XBox Controller, L1 on PS Controller
  RightShoulder, // RB on XBox Controller, R1 on PS Controller
  DPadLeft,
  DPadRight,
  DPadUp,
  DPadDown,
  Count
};

} // namespace FrontendCommon

class CommonHostInterface : public HostInterface
{
public:
  friend ControllerInterface;

  enum : s32
  {
    PER_GAME_SAVE_STATE_SLOTS = 10,
    GLOBAL_SAVE_STATE_SLOTS = 10,
    NUM_CONTROLLER_AUTOFIRE_BUTTONS = 4,
    DEFAULT_AUTOFIRE_FREQUENCY = 2
  };

  using HostKeyCode = s32;
  using HostMouseButton = s32;

  using InputButtonHandler = std::function<void(bool)>;
  using InputAxisHandler = std::function<void(float)>;
  using ControllerRumbleCallback = std::function<void(const float*, u32)>;

  struct HotkeyInfo
  {
    String category;
    String name;
    String display_name;
    InputButtonHandler handler;
  };

  using HotkeyInfoList = std::vector<HotkeyInfo>;

  struct InputProfileEntry
  {
    std::string name;
    std::string path;
  };
  using InputProfileList = std::vector<InputProfileEntry>;

  struct SaveStateInfo
  {
    std::string path;
    u64 timestamp;
    s32 slot;
    bool global;
  };

  struct ExtendedSaveStateInfo
  {
    std::string path;
    std::string title;
    std::string game_code;
    std::string media_path;
    u64 timestamp;
    s32 slot;
    bool global;

    u32 screenshot_width;
    u32 screenshot_height;
    std::vector<u32> screenshot_data;
  };

  using HostInterface::SaveState;

  /// Returns the name of the frontend.
  virtual const char* GetFrontendName() const = 0;

  /// Request the frontend to exit.
  virtual void RequestExit() = 0;

  /// Runs an event next frame as part of the event loop.
  virtual void RunLater(std::function<void()> func) = 0;

  /// Thread-safe settings access.
  std::string GetStringSettingValue(const char* section, const char* key, const char* default_value = "") override;
  bool GetBoolSettingValue(const char* section, const char* key, bool default_value = false) override;
  int GetIntSettingValue(const char* section, const char* key, int default_value = 0) override;
  float GetFloatSettingValue(const char* section, const char* key, float default_value = 0.0f) override;
  std::vector<std::string> GetSettingStringList(const char* section, const char* key) override;

  /// Loads new settings and applies them.
  virtual void ApplySettings(bool display_osd_messages);

  /// Reloads the input map from config. Callable from controller interface.
  void UpdateInputMap();

  virtual bool IsFullscreen() const;
  virtual bool SetFullscreen(bool enabled);

  virtual bool Initialize() override;

  virtual void Shutdown() override;

  virtual bool BootSystem(std::shared_ptr<SystemBootParameters> parameters) override;
  virtual void ResetSystem() override;
  virtual void DestroySystem() override;

  /// Returns the settings interface.
  ALWAYS_INLINE SettingsInterface* GetSettingsInterface() const { return m_settings_interface.get(); }
  ALWAYS_INLINE std::lock_guard<std::recursive_mutex> GetSettingsLock()
  {
    return std::lock_guard<std::recursive_mutex>(m_settings_mutex);
  }

  /// Returns the game list.
  ALWAYS_INLINE GameList* GetGameList() const { return m_game_list.get(); }

  /// Returns a list of all available hotkeys.
  ALWAYS_INLINE const HotkeyInfoList& GetHotkeyInfoList() const { return m_hotkeys; }

  /// Access to current controller interface.
  ALWAYS_INLINE ControllerInterface* GetControllerInterface() const { return m_controller_interface.get(); }

  /// Returns true if running in batch mode, i.e. exit after emulation.
  ALWAYS_INLINE bool InBatchMode() const { return m_flags.batch_mode; }

  /// Returns true if the fullscreen UI is enabled.
  ALWAYS_INLINE bool IsFullscreenUIEnabled() const { return m_fullscreen_ui_enabled; }

  /// Returns true if an undo load state exists.
  ALWAYS_INLINE bool CanUndoLoadState() const { return static_cast<bool>(m_undo_load_state); }

  /// Parses command line parameters for all frontends.
  bool ParseCommandLineParameters(int argc, char* argv[], std::unique_ptr<SystemBootParameters>* out_boot_params);

  /// Returns a path where an input profile with the specified name would be saved.
  std::string GetSavePathForInputProfile(const char* name) const;

  /// Returns a list of all input profiles. first - name, second - path
  InputProfileList GetInputProfileList() const;

  /// Returns the path for an input profile.
  std::string GetInputProfilePath(const char* name) const;

  /// Applies the specified input profile.
  bool ApplyInputProfile(const char* profile_path);

  /// Saves the current input configuration to the specified profile name.
  bool SaveInputProfile(const char* profile_path);

  /// Powers off the system, optionally saving the resume state.
  void PowerOffSystem(bool save_resume_state);

  /// Undoes a load state, i.e. restores the state prior to the load.
  bool UndoLoadState();

  /// Loads state from the specified filename.
  bool LoadState(const char* filename);

  /// Loads the current emulation state from file. Specifying a slot of -1 loads the "resume" game state.
  bool LoadState(bool global, s32 slot);

  /// Saves the current emulation state to a file. Specifying a slot of -1 saves the "resume" save state.
  bool SaveState(bool global, s32 slot);

  /// Returns true if the specified file/disc image is resumable.
  bool CanResumeSystemFromFile(const char* filename);

  /// Loads the resume save state for the given game. Optionally boots the game anyway if loading fails.
  bool ResumeSystemFromState(const char* filename, bool boot_on_failure);

  /// Loads the most recent resume save state. This may be global or per-game.
  bool ResumeSystemFromMostRecentState();

  /// Saves the resume save state, call when shutting down.
  bool SaveResumeSaveState();

  /// Returns a list of save states for the specified game code.
  std::vector<SaveStateInfo> GetAvailableSaveStates(const char* game_code) const;

  /// Returns save state info if present. If game_code is null or empty, assumes global state.
  std::optional<SaveStateInfo> GetSaveStateInfo(const char* game_code, s32 slot);

  /// Returns save state info from opened save state stream.
  std::optional<ExtendedSaveStateInfo> GetExtendedSaveStateInfo(ByteStream* stream);

  /// Returns save state info if present. If game_code is null or empty, assumes global state.
  std::optional<ExtendedSaveStateInfo> GetExtendedSaveStateInfo(const char* game_code, s32 slot);

  /// Returns save state info for the undo slot, if present.
  std::optional<ExtendedSaveStateInfo> GetUndoSaveStateInfo();

  /// Deletes save states for the specified game code. If resume is set, the resume state is deleted too.
  void DeleteSaveStates(const char* game_code, bool resume);

  /// Adds OSD messages, duration is in seconds.
  void AddOSDMessage(std::string message, float duration = 2.0f) override;
  void ClearOSDMessages();

  /// async message queue bookeeping for. Should be called on UI thread.
  void AcquirePendingOSDMessages();

  /// Displays a loading screen with the logo, rendered with ImGui. Use when executing possibly-time-consuming tasks
  /// such as compiling shaders when starting up.
  void DisplayLoadingScreen(const char* message, int progress_min = -1, int progress_max = -1,
                            int progress_value = -1) override;

  /// Retrieves information about specified game from game list.
  void GetGameInfo(const char* path, CDImage* image, std::string* code, std::string* title) override;

  /// Returns true if currently dumping audio.
  bool IsDumpingAudio() const;

  /// Starts dumping audio to a file. If no file name is provided, one will be generated automatically.
  bool StartDumpingAudio(const char* filename = nullptr);

  /// Stops dumping audio to file if it has been started.
  void StopDumpingAudio();

  /// Saves a screenshot to the specified file. IF no file name is provided, one will be generated automatically.
  bool SaveScreenshot(const char* filename = nullptr, bool full_resolution = true, bool apply_aspect_ratio = true,
                      bool compress_on_thread = true);

  /// Loads the cheat list from the specified file.
  bool LoadCheatList(const char* filename);

  /// Loads the cheat list for the current game title from the user directory.
  bool LoadCheatListFromGameTitle();

  /// Loads the cheat list for the current game code from the built-in code database.
  bool LoadCheatListFromDatabase();

  /// Saves the current cheat list to the game title's file.
  bool SaveCheatList();

  /// Saves the current cheat list to the specified file.
  bool SaveCheatList(const char* filename);

  /// Deletes the cheat list, if present.
  bool DeleteCheatList();

  /// Removes all cheats from the cheat list.
  void ClearCheatList(bool save_to_file);

  /// Enables/disabled the specified cheat code.
  void SetCheatCodeState(u32 index, bool enabled, bool save_to_file);

  /// Immediately applies the specified cheat code.
  void ApplyCheatCode(u32 index);

  /// Temporarily toggles post-processing on/off.
  void TogglePostProcessing();

  /// Reloads post processing shaders with the current configuration.
  void ReloadPostProcessingShaders();

  /// Toggle Widescreen Hack and Aspect Ratio
  void ToggleWidescreen();

  /// Swaps memory cards in slot 1/2.
  void SwapMemoryCards();

  /// Parses a fullscreen mode into its components (width * height @ refresh hz)
  static bool ParseFullscreenMode(const std::string_view& mode, u32* width, u32* height, float* refresh_rate);

  /// Converts a fullscreen mode to a string.
  static std::string GetFullscreenModeString(u32 width, u32 height, float refresh_rate);

  /// Returns true if the state should be saved on shutdown.
  bool ShouldSaveResumeState() const;

  /// Returns true if fast forwarding or slow motion is currently active.
  bool IsRunningAtNonStandardSpeed() const;

  /// Requests the specified size for the render window. Not guaranteed to succeed (e.g. if in fullscreen).
  virtual bool RequestRenderWindowSize(s32 new_window_width, s32 new_window_height);

  /// Requests a resize to a multiple of the render window size.
  bool RequestRenderWindowScale(float scale);

  /// Returns a pointer to the top-level window, needed by some controller interfaces.
  virtual void* GetTopLevelWindowHandle() const;

  /// Called when achievements data is loaded.
  virtual void OnAchievementsRefreshed();

  /// Opens a file in the DuckStation "package".
  /// This is the APK for Android builds, or the program directory for standalone builds.
  virtual std::unique_ptr<ByteStream> OpenPackageFile(const char* path, u32 flags) override;

  /// Returns true if the fullscreen UI is intercepting controller input.
  bool IsControllerNavigationActive() const;

  /// Controller navigation, used by fullscreen mode.
  void SetControllerNavigationButtonState(FrontendCommon::ControllerNavigationButton button, bool pressed);

  /// Alters autofire state for controllers (activates/deactivates).
  void SetControllerAutoFireSlotState(u32 controller_index, u32 slot_index, bool active);

  /// Toggles fast forward state.
  bool IsFastForwardEnabled() const { return m_fast_forward_enabled; }
  void SetFastForwardEnabled(bool enabled);

  /// Toggles turbo state.
  bool IsTurboEnabled() const { return m_turbo_enabled; }
  void SetTurboEnabled(bool enabled);

  /// Toggles rewind state.
  void SetRewindState(bool enabled);

  /// ImGui window drawing.
  void DrawStatsOverlay();
  void DrawEnhancementsOverlay();
  void DrawOSDMessages();
  void DrawDebugWindows();

  /// Returns true if features such as save states should be disabled.
  bool IsCheevosChallengeModeActive() const;

protected:
  enum : u32
  {
    SETTINGS_VERSION = 3
  };

  struct OSDMessage
  {
    std::string text;
    Common::Timer time;
    float duration;
  };

  CommonHostInterface();
  ~CommonHostInterface();

  /// Registers frontend-specific hotkeys.
  virtual void RegisterHotkeys();

  /// Executes per-frame tasks such as controller polling.
  virtual void PollAndUpdate();

  /// Saves the undo load state, so it can be restored.
  bool SaveUndoLoadState();

  virtual std::unique_ptr<AudioStream> CreateAudioStream(AudioBackend backend) override;
  virtual s32 GetAudioOutputVolume() const override;
  virtual void UpdateControllerInterface();

  virtual void OnSystemCreated() override;
  virtual void OnSystemPaused(bool paused) override;
  virtual void OnSystemDestroyed() override;
  virtual void OnRunningGameChanged(const std::string& path, CDImage* image, const std::string& game_code,
                                    const std::string& game_title) override;
  virtual void OnControllerTypeChanged(u32 slot) override;

  virtual std::optional<HostKeyCode> GetHostKeyCode(const std::string_view key_code) const;

  virtual bool AddButtonToInputMap(const std::string& binding, const std::string_view& device,
                                   const std::string_view& button, InputButtonHandler handler);
  virtual bool AddAxisToInputMap(const std::string& binding, const std::string_view& device,
                                 const std::string_view& axis, Controller::AxisType axis_type,
                                 InputAxisHandler handler);
  virtual bool AddRumbleToInputMap(const std::string& binding, u32 controller_index, u32 num_motors);

  void RegisterHotkey(String category, String name, String display_name, InputButtonHandler handler);
  bool HandleHostKeyEvent(HostKeyCode code, HostKeyCode modifiers, bool pressed);
  bool HandleHostMouseEvent(HostMouseButton button, bool pressed);
  virtual void UpdateInputMap(SettingsInterface& si);
  void ClearInputMap();

  /// Updates controller metastate, including turbo and rumble.
  void UpdateControllerMetaState();

  void AddControllerRumble(u32 controller_index, u32 num_motors, ControllerRumbleCallback callback);
  void UpdateControllerRumble();
  void StopControllerRumble();

  void SetControllerAutoFireState(u32 controller_index, s32 button_code, bool active);
  void StopControllerAutoFire();
  void UpdateControllerAutoFire();

  /// Returns the path to a save state file. Specifying an index of -1 is the "resume" save state.
  std::string GetGameSaveStateFileName(const char* game_code, s32 slot) const;

  /// Returns the path to a save state file. Specifying an index of -1 is the "resume" save state.
  std::string GetGlobalSaveStateFileName(s32 slot) const;

  /// Moves the current save state file to a backup name, if it exists.
  void RenameCurrentSaveStateToBackup(const char* filename);

  /// Sets the base path for the user directory. Can be overridden by platform/frontend/command line.
  virtual void SetUserDirectory();

  /// Updates logging settings.
  virtual void UpdateLogSettings(LOGLEVEL level, const char* filter, bool log_to_console, bool log_to_debug,
                                 bool log_to_window, bool log_to_file);

  /// Returns the path of the settings file.
  std::string GetSettingsFileName() const;

  /// Returns the most recent resume save state.
  std::string GetMostRecentResumeSaveStatePath() const;

  /// Returns the path to the cheat file for the specified game title.
  std::string GetCheatFileName() const;

  /// Restores all settings to defaults.
  virtual void SetDefaultSettings(SettingsInterface& si) override;

  /// Resets known settings to default.
  virtual void SetDefaultSettings();

  /// Loads settings to m_settings and any frontend-specific parameters.
  virtual void LoadSettings(SettingsInterface& si) override;

  /// Saves current settings variables to ini.
  virtual void SaveSettings(SettingsInterface& si) override;

  /// Checks and fixes up any incompatible settings.
  virtual void FixIncompatibleSettings(bool display_osd_messages) override;

  /// Checks for settings changes, std::move() the old settings away for comparing beforehand.
  virtual void CheckForSettingsChanges(const Settings& old_settings) override;

  /// Increases timer resolution when supported by the host OS.
  void SetTimerResolutionIncreased(bool enabled);

  void UpdateSpeedLimiterState();

  void RecreateSystem() override;
  void OnHostDisplayResized() override;

  void ApplyGameSettings(bool display_osd_messages);
  void ApplyRendererFromGameSettings(const std::string& boot_filename);
  void ApplyControllerCompatibilitySettings(u64 controller_mask, bool display_osd_messages);

  bool CreateHostDisplayResources();
  void ReleaseHostDisplayResources();

  virtual void DrawImGuiWindows();

  void DoFrameStep();
  void DoToggleCheats();

  std::unique_ptr<SettingsInterface> m_settings_interface;
  std::recursive_mutex m_settings_mutex;

  std::unique_ptr<GameList> m_game_list;

  std::unique_ptr<ControllerInterface> m_controller_interface;

  std::unique_ptr<HostDisplayTexture> m_logo_texture;

  std::deque<OSDMessage> m_osd_active_messages; // accessed only by GUI/OSD thread (no lock reqs)
  std::deque<OSDMessage> m_osd_posted_messages; // written to by multiple threads.
  std::mutex m_osd_messages_lock;

  bool m_fullscreen_ui_enabled = false;
  bool m_frame_step_request = false;
  bool m_fast_forward_enabled = false;
  bool m_turbo_enabled = false;
  bool m_throttler_enabled = true;
  bool m_display_all_frames = true;

  union
  {
    u8 bits;

    // running in batch mode? i.e. exit after stopping emulation
    BitField<u8, bool, 0, 1> batch_mode;

    // disable controller interface (buggy devices with SDL)
    BitField<u8, bool, 1, 1> disable_controller_interface;

    // starting fullscreen (outside of boot options)
    BitField<u8, bool, 2, 1> start_fullscreen;

    // force fullscreen UI enabled (nogui)
    BitField<u8, bool, 3, 1> force_fullscreen_ui;
  } m_flags = {};

private:
  void LoadSettings();
  void InitializeUserDirectory();
  void RegisterGeneralHotkeys();
  void RegisterSystemHotkeys();
  void RegisterGraphicsHotkeys();
  void RegisterSaveStateHotkeys();
  void RegisterAudioHotkeys();
  void FindInputProfiles(const std::string& base_path, InputProfileList* out_list) const;
  void UpdateControllerInputMap(SettingsInterface& si);
  bool UpdateControllerInputMapFromGameSettings();
  void UpdateHotkeyInputMap(SettingsInterface& si);
  void ClearAllControllerBindings();
  void CreateImGuiContext();

#ifdef WITH_DISCORD_PRESENCE
  void SetDiscordPresenceEnabled(bool enabled);
  void InitializeDiscordPresence();
  void ShutdownDiscordPresence();
  void UpdateDiscordPresence(bool rich_presence_only);
  void PollDiscordPresence();
#endif

#ifdef WITH_CHEEVOS
  void UpdateCheevosActive();
#endif

  HotkeyInfoList m_hotkeys;

  std::unique_ptr<FrontendCommon::SaveStateSelectorUI> m_save_state_selector_ui;

  // input key maps
  std::map<HostKeyCode, InputButtonHandler> m_keyboard_input_handlers;
  std::map<HostMouseButton, InputButtonHandler> m_mouse_input_handlers;

  // controller vibration motors/rumble
  struct ControllerRumbleState
  {
    enum : u32
    {
      MAX_MOTORS = 2
    };

    u32 controller_index;
    u32 num_motors;
    std::array<float, MAX_MOTORS> last_strength;
    u64 last_update_time;
    ControllerRumbleCallback update_callback;
  };
  std::vector<ControllerRumbleState> m_controller_vibration_motors;

  // controller turbo buttons
  struct ControllerAutoFireState
  {
    u32 controller_index;
    u32 slot_index;
    s32 button_code;
    u8 frequency;
    u8 countdown;
    bool active;
    bool state;
  };
  std::vector<ControllerAutoFireState> m_controller_autofires;

  // temporary save state, created when loading, used to undo load state
  std::unique_ptr<ByteStream> m_undo_load_state;

#ifdef WITH_DISCORD_PRESENCE
  // discord rich presence
  bool m_discord_presence_enabled = false;
  bool m_discord_presence_active = false;
#ifdef WITH_CHEEVOS
  std::string m_discord_presence_cheevos_string;
#endif
#endif
};
