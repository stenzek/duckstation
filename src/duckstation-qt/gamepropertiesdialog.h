#pragma once
#include "frontend-common/game_settings.h"
#include "ui_gamepropertiesdialog.h"
#include <QtWidgets/QDialog>
#include <array>

struct GameListEntry;
struct GameListCompatibilityEntry;

class QtHostInterface;

class GamePropertiesDialog final : public QDialog
{
  Q_OBJECT

public:
  GamePropertiesDialog(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~GamePropertiesDialog();

  static void showForEntry(QtHostInterface* host_interface, const GameListEntry* ge, QWidget* parent);

public Q_SLOTS:
  void clear();
  void populate(const GameListEntry* ge);

protected:
  void closeEvent(QCloseEvent* ev);
  void resizeEvent(QResizeEvent* ev);

private Q_SLOTS:
  void saveCompatibilityInfo();
  void saveCompatibilityInfoIfChanged();
  void setCompatibilityInfoChanged();

  void onSetVersionTestedToCurrentClicked();
  void onComputeHashClicked();
  void onExportCompatibilityInfoClicked();
  void updateCPUClockSpeedLabel();
  void onEnableCPUClockSpeedControlChecked(int state);

private:
  void setupAdditionalUi();
  void connectUi();
  void populateCompatibilityInfo(const std::string& game_code);
  void populateTracksInfo(const std::string& image_path);
  void populateGameSettings();
  void populateBooleanUserSetting(QCheckBox* cb, const std::optional<bool>& value);
  void connectBooleanUserSetting(QCheckBox* cb, std::optional<bool>* value);
  void saveGameSettings();
  void fillEntryFromUi(GameListCompatibilityEntry* entry);
  void computeTrackHashes(std::string& redump_keyword);
  void onResize();
  void onUserAspectRatioChanged();

  Ui::GamePropertiesDialog m_ui;
  std::array<QCheckBox*, static_cast<u32>(GameSettings::Trait::Count)> m_trait_checkboxes{};

  QtHostInterface* m_host_interface;

  std::string m_path;
  std::string m_game_code;
  std::string m_game_title;
  std::string m_redump_search_keyword;

  GameSettings::Entry m_game_settings;

  bool m_compatibility_info_changed = false;
};
