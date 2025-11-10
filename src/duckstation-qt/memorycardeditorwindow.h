// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "ui_memorycardeditorwindow.h"
#include "ui_memorycardrenamefiledialog.h"

#include "core/memory_card_image.h"

#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>

class Error;

class MemoryCardEditorWindow : public QWidget
{
  Q_OBJECT

public:
  MemoryCardEditorWindow();
  ~MemoryCardEditorWindow();

  bool setCardA(const QString& path);
  bool setCardB(const QString& path);

  static bool createMemoryCard(const QString& path, Error* error);

protected:
  bool event(QEvent* event) override;

private:
  struct Card
  {
    std::string filename;
    MemoryCardImage::DataArray data;
    std::vector<MemoryCardImage::FileInfo> files;
    u32 blocks_free = 0;
    bool dirty = false;

    QComboBox* path_cb = nullptr;
    QTableWidget* table = nullptr;
    QLabel* blocks_free_label = nullptr;
    QPushButton* save_button = nullptr;
    QPushButton* import_button = nullptr;
    QPushButton* import_file_button = nullptr;
    QPushButton* format_button = nullptr;
  };

  void createCardButtons(Card* card, QDialogButtonBox* buttonBox);
  void connectCardUi(Card* card, QDialogButtonBox* buttonBox);

  void connectUi();
  void populateComboBox(QComboBox* cb);
  void clearSelection();
  void loadCardFromComboBox(Card* card, int index);
  bool loadCard(const QString& filename, Card* card);
  void updateCardTable(Card* card);
  void updateCardBlocksFree(Card* card);
  void setCardDirty(Card* card);
  void newCard(Card* card);
  void openCard(Card* card);
  void saveCard(Card* card);
  void promptForSave(Card* card);
  void importCard(Card* card);
  void formatCard(Card* card);

  void doRenameSaveFile();
  void doExportSaveFile();
  void importSaveFile(Card* card);

  std::tuple<Card*, const MemoryCardImage::FileInfo*> getSelectedFile();
  void updateButtonState();

  void updateAnimationTimerActive();

  void onCardASelectionChanged();
  void onCardBSelectionChanged();
  void onCardContextMenuRequested(const QPoint& pos);
  void doCopyFile();
  void doDeleteFile();
  void doUndeleteFile();
  void incrementAnimationFrame();

  Ui::MemoryCardEditorDialog m_ui;
  QPushButton* m_deleteFile;
  QPushButton* m_undeleteFile;
  QPushButton* m_renameFile;
  QPushButton* m_exportFile;
  QPushButton* m_moveLeft;
  QPushButton* m_moveRight;

  Card m_card_a;
  Card m_card_b;
  u32 m_current_frame_index = 0;
  int m_file_icon_width = 0;
  int m_file_icon_height = 0;

  QTimer* m_animation_timer = nullptr;
};

class MemoryCardRenameFileDialog final : public QDialog
{
  Q_OBJECT

public:
  MemoryCardRenameFileDialog(QWidget* parent, std::string_view old_name);
  ~MemoryCardRenameFileDialog() override;

  std::string getNewName() const;

private:
  void setupAdditionalUi();

  void updateSimplifiedFieldsFromFullName();
  void updateFullNameFromSimplifiedFields();

  Ui::MemoryCardRenameFileDialog m_ui;
};
