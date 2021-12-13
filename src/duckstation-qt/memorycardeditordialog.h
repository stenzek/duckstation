#pragma once
#include "core/memory_card_image.h"
#include "ui_memorycardeditordialog.h"
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>

class MemoryCardEditorDialog : public QDialog
{
  Q_OBJECT

public:
  MemoryCardEditorDialog(QWidget* parent);
  ~MemoryCardEditorDialog();

  bool setCardA(const QString& path);
  bool setCardB(const QString& path);

  static bool createMemoryCard(const QString& path);

protected:
  void resizeEvent(QResizeEvent* ev);
  void closeEvent(QCloseEvent* ev);

private Q_SLOTS:
  void onCardASelectionChanged();
  void onCardBSelectionChanged();
  void doCopyFile();
  void doDeleteFile();
  void doUndeleteFile();

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

  void doExportSaveFile();
  void importSaveFile(Card* card);

  std::tuple<Card*, const MemoryCardImage::FileInfo*> getSelectedFile();
  void updateButtonState();

  Ui::MemoryCardEditorDialog m_ui;
  QPushButton* m_deleteFile;
  QPushButton* m_undeleteFile;
  QPushButton* m_exportFile;
  QPushButton* m_moveLeft;
  QPushButton* m_moveRight;

  Card m_card_a;
  Card m_card_b;
};
