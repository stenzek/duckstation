#pragma once
#include "ui_gamepropertiesdialog.h"
#include <QtCore/QMap>
#include <QtWidgets/QDialog>

struct GameListEntry;
struct GameListCompatibilityEntry;

class QtHostInterface;

class GamePropertiesDialog final : public QDialog
{
  Q_OBJECT

public:
  GamePropertiesDialog(QtHostInterface* host_interface, QWidget* parent = nullptr);
  ~GamePropertiesDialog();

  static void showForEntry(QtHostInterface* host_interface, const GameListEntry* ge);

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
  void onVerifyDumpClicked();
  void onExportCompatibilityInfoClicked();

private:
  void setupAdditionalUi();
  void connectUi();
  void populateCompatibilityInfo(const std::string& game_code);
  void populateTracksInfo(const char* image_path);
  void fillEntryFromUi(GameListCompatibilityEntry* entry);
  void onResize();

  Ui::GamePropertiesDialog m_ui;

  QtHostInterface* m_host_interface;

  bool m_compatibility_info_changed = false;
};
