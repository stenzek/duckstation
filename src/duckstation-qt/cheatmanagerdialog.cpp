#include "cheatmanagerdialog.h"
#include "cheatcodeeditordialog.h"
#include "common/assert.h"
#include "common/string_util.h"
#include "core/bus.h"
#include "core/cpu_core.h"
#include "core/system.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QFileInfo>
#include <QtGui/QColor>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QTreeWidgetItemIterator>
#include <array>
#include <utility>

static QString formatHexValue(u32 value)
{
  return QStringLiteral("0x%1").arg(static_cast<uint>(value), 8, 16, QChar('0'));
}

static QString formatValue(u32 value, bool is_signed)
{
  if (is_signed)
    return QStringLiteral("%1").arg(static_cast<int>(value));
  else
    return QStringLiteral("%1").arg(static_cast<uint>(value));
}

CheatManagerDialog::CheatManagerDialog(QWidget* parent) : QDialog(parent)
{
  m_ui.setupUi(this);

  setupAdditionalUi();
  connectUi();

  updateCheatList();
}

CheatManagerDialog::~CheatManagerDialog() = default;

void CheatManagerDialog::setupAdditionalUi()
{
  m_ui.scanStartAddress->setText(formatHexValue(m_scanner.GetStartAddress()));
  m_ui.scanEndAddress->setText(formatHexValue(m_scanner.GetEndAddress()));
}

void CheatManagerDialog::connectUi()
{
  connect(m_ui.tabWidget, &QTabWidget::currentChanged, [this](int index) {
    resizeColumns();
    setUpdateTimerEnabled(index == 1);
  });
  connect(m_ui.cheatList, &QTreeWidget::currentItemChanged, this, &CheatManagerDialog::cheatListCurrentItemChanged);
  connect(m_ui.cheatList, &QTreeWidget::itemActivated, this, &CheatManagerDialog::cheatListItemActivated);
  connect(m_ui.cheatList, &QTreeWidget::itemChanged, this, &CheatManagerDialog::cheatListItemChanged);
  connect(m_ui.cheatListNewCategory, &QPushButton::clicked, this, &CheatManagerDialog::newCategoryClicked);
  connect(m_ui.cheatListAdd, &QPushButton::clicked, this, &CheatManagerDialog::addCodeClicked);
  connect(m_ui.cheatListEdit, &QPushButton::clicked, this, &CheatManagerDialog::editCodeClicked);
  connect(m_ui.cheatListRemove, &QPushButton::clicked, this, &CheatManagerDialog::deleteCodeClicked);
  connect(m_ui.cheatListActivate, &QPushButton::clicked, this, &CheatManagerDialog::activateCodeClicked);
  connect(m_ui.cheatListImport, &QPushButton::clicked, this, &CheatManagerDialog::importClicked);
  connect(m_ui.cheatListExport, &QPushButton::clicked, this, &CheatManagerDialog::exportClicked);

  connect(m_ui.scanValue, &QLineEdit::textChanged, this, &CheatManagerDialog::updateScanValue);
  connect(m_ui.scanValueBase, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { updateScanValue(); });
  connect(m_ui.scanSize, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    m_scanner.SetSize(static_cast<MemoryAccessSize>(index));
    m_scanner.ResetSearch();
    updateResults();
  });
  connect(m_ui.scanValueSigned, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    m_scanner.SetValueSigned(index == 0);
    m_scanner.ResetSearch();
    updateResults();
  });
  connect(m_ui.scanOperator, QOverload<int>::of(&QComboBox::currentIndexChanged),
          [this](int index) { m_scanner.SetOperator(static_cast<MemoryScan::Operator>(index)); });
  connect(m_ui.scanStartAddress, &QLineEdit::textChanged, [this](const QString& value) {
    uint address;
    if (value.startsWith(QStringLiteral("0x")) && value.length() > 2)
      address = value.mid(2).toUInt(nullptr, 16);
    else
      address = value.toUInt(nullptr, 16);
    m_scanner.SetStartAddress(static_cast<PhysicalMemoryAddress>(address));
  });
  connect(m_ui.scanEndAddress, &QLineEdit::textChanged, [this](const QString& value) {
    uint address;
    if (value.startsWith(QStringLiteral("0x")) && value.length() > 2)
      address = value.mid(2).toUInt(nullptr, 16);
    else
      address = value.toUInt(nullptr, 16);
    m_scanner.SetEndAddress(static_cast<PhysicalMemoryAddress>(address));
  });
  connect(m_ui.scanPresetRange, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int index) {
    if (index == 0)
    {
      m_ui.scanStartAddress->setText(formatHexValue(0));
      m_ui.scanEndAddress->setText(formatHexValue(Bus::RAM_SIZE));
    }
    else if (index == 1)
    {
      m_ui.scanStartAddress->setText(formatHexValue(CPU::DCACHE_LOCATION));
      m_ui.scanEndAddress->setText(formatHexValue(CPU::DCACHE_LOCATION + CPU::DCACHE_SIZE));
    }
    else
    {
      m_ui.scanStartAddress->setText(formatHexValue(Bus::BIOS_BASE));
      m_ui.scanEndAddress->setText(formatHexValue(Bus::BIOS_BASE + Bus::BIOS_SIZE));
    }
  });
  connect(m_ui.scanNewSearch, &QPushButton::clicked, [this]() {
    m_scanner.Search();
    updateResults();
  });
  connect(m_ui.scanSearchAgain, &QPushButton::clicked, [this]() {
    m_scanner.SearchAgain();
    updateResults();
  });
  connect(m_ui.scanResetSearch, &QPushButton::clicked, [this]() {
    m_scanner.ResetSearch();
    updateResults();
  });
  connect(m_ui.scanAddWatch, &QPushButton::clicked, this, &CheatManagerDialog::addToWatchClicked);
  connect(m_ui.scanRemoveWatch, &QPushButton::clicked, this, &CheatManagerDialog::removeWatchClicked);
  connect(m_ui.scanTable, &QTableWidget::currentItemChanged, this, &CheatManagerDialog::scanCurrentItemChanged);
  connect(m_ui.watchTable, &QTableWidget::currentItemChanged, this, &CheatManagerDialog::watchCurrentItemChanged);
  connect(m_ui.scanTable, &QTableWidget::itemChanged, this, &CheatManagerDialog::scanItemChanged);
  connect(m_ui.watchTable, &QTableWidget::itemChanged, this, &CheatManagerDialog::watchItemChanged);
}

void CheatManagerDialog::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  resizeColumns();
}

void CheatManagerDialog::resizeEvent(QResizeEvent* event)
{
  QDialog::resizeEvent(event);
  resizeColumns();
}

void CheatManagerDialog::resizeColumns()
{
  QtUtils::ResizeColumnsForTableView(m_ui.scanTable, {-1, 100, 100});
  QtUtils::ResizeColumnsForTableView(m_ui.watchTable, {50, -1, 100, 150, 100});
  QtUtils::ResizeColumnsForTreeView(m_ui.cheatList, {-1, 100, 150, 100});
}

void CheatManagerDialog::setUpdateTimerEnabled(bool enabled)
{
  if ((!m_update_timer && !enabled) && m_update_timer->isActive() == enabled)
    return;

  if (!m_update_timer)
  {
    m_update_timer = new QTimer(this);
    connect(m_update_timer, &QTimer::timeout, this, &CheatManagerDialog::updateScanUi);
  }

  if (enabled)
    m_update_timer->start(100);
  else
    m_update_timer->stop();
}

int CheatManagerDialog::getSelectedResultIndex() const
{
  QList<QTableWidgetSelectionRange> sel = m_ui.scanTable->selectedRanges();
  if (sel.isEmpty())
    return -1;

  return sel.front().topRow();
}

int CheatManagerDialog::getSelectedWatchIndex() const
{
  QList<QTableWidgetSelectionRange> sel = m_ui.watchTable->selectedRanges();
  if (sel.isEmpty())
    return -1;

  return sel.front().topRow();
}

QTreeWidgetItem* CheatManagerDialog::getItemForCheatIndex(u32 index) const
{
  QTreeWidgetItemIterator iter(m_ui.cheatList);
  while (*iter)
  {
    QTreeWidgetItem* item = *iter;
    const QVariant item_data(item->data(0, Qt::UserRole));
    if (item_data.isValid() && item_data.toUInt() == index)
      return item;

    ++iter;
  }

  return nullptr;
}

QTreeWidgetItem* CheatManagerDialog::getItemForCheatGroup(const QString& group_name) const
{
  const int count = m_ui.cheatList->topLevelItemCount();
  for (int i = 0; i < count; i++)
  {
    QTreeWidgetItem* item = m_ui.cheatList->topLevelItem(i);
    if (item->text(0) == group_name)
      return item;
  }

  return nullptr;
}

QTreeWidgetItem* CheatManagerDialog::createItemForCheatGroup(const QString& group_name) const
{
  QTreeWidgetItem* group = new QTreeWidgetItem();
  group->setFlags(group->flags() | Qt::ItemIsUserCheckable);
  group->setText(0, group_name);
  m_ui.cheatList->addTopLevelItem(group);
  return group;
}

QStringList CheatManagerDialog::getCheatGroupNames() const
{
  QStringList group_names;

  const int count = m_ui.cheatList->topLevelItemCount();
  for (int i = 0; i < count; i++)
  {
    QTreeWidgetItem* item = m_ui.cheatList->topLevelItem(i);
    group_names.push_back(item->text(0));
  }

  return group_names;
}

static int getCheatIndexFromItem(QTreeWidgetItem* item)
{
  QVariant item_data(item->data(0, Qt::UserRole));
  if (!item_data.isValid())
    return -1;

  return static_cast<int>(item_data.toUInt());
}

int CheatManagerDialog::getSelectedCheatIndex() const
{
  QList<QTreeWidgetItem*> sel = m_ui.cheatList->selectedItems();
  if (sel.isEmpty())
    return -1;

  return static_cast<int>(getCheatIndexFromItem(sel.first()));
}

CheatList* CheatManagerDialog::getCheatList() const
{
  Assert(System::IsValid());

  CheatList* list = System::GetCheatList();
  if (!list)
    QtHostInterface::GetInstance()->LoadCheatListFromGameTitle();
  if (!list)
  {
    QtHostInterface::GetInstance()->executeOnEmulationThread(
      []() { System::SetCheatList(std::make_unique<CheatList>()); }, true);
    list = System::GetCheatList();
  }

  return list;
}

void CheatManagerDialog::updateCheatList()
{
  QSignalBlocker sb(m_ui.cheatList);

  CheatList* list = getCheatList();
  while (m_ui.cheatList->topLevelItemCount() > 0)
    delete m_ui.cheatList->takeTopLevelItem(0);

  const std::vector<std::string> groups = list->GetCodeGroups();
  for (const std::string& group_name : groups)
  {
    QTreeWidgetItem* group = createItemForCheatGroup(QString::fromStdString(group_name));

    const u32 count = list->GetCodeCount();
    bool all_enabled = true;
    for (u32 i = 0; i < count; i++)
    {
      const CheatCode& code = list->GetCode(i);
      if (code.group != group_name)
        continue;

      QTreeWidgetItem* item = new QTreeWidgetItem(group);
      fillItemForCheatCode(item, i, code);

      all_enabled &= code.enabled;
    }

    group->setCheckState(0, all_enabled ? Qt::Checked : Qt::Unchecked);
    group->setExpanded(true);
  }

  m_ui.cheatListEdit->setEnabled(false);
  m_ui.cheatListRemove->setEnabled(false);
  m_ui.cheatListActivate->setText(tr("Activate"));
  m_ui.cheatListActivate->setEnabled(false);
  m_ui.cheatListExport->setEnabled(list->GetCodeCount() > 0);
}

void CheatManagerDialog::fillItemForCheatCode(QTreeWidgetItem* item, u32 index, const CheatCode& code)
{
  item->setData(0, Qt::UserRole, QVariant(static_cast<uint>(index)));
  if (code.IsManuallyActivated())
  {
    item->setFlags(item->flags() & ~(Qt::ItemIsUserCheckable));
  }
  else
  {
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(0, code.enabled ? Qt::Checked : Qt::Unchecked);
  }
  item->setText(0, QString::fromStdString(code.description));
  item->setText(1, qApp->translate("Cheats", CheatCode::GetTypeDisplayName(code.type)));
  item->setText(2, qApp->translate("Cheats", CheatCode::GetActivationDisplayName(code.activation)));
  item->setText(3, QString::number(static_cast<uint>(code.instructions.size())));
}

void CheatManagerDialog::saveCheatList()
{
  QtHostInterface::GetInstance()->executeOnEmulationThread([]() { QtHostInterface::GetInstance()->SaveCheatList(); });
}

void CheatManagerDialog::cheatListCurrentItemChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous)
{
  const int cheat_index = current ? getCheatIndexFromItem(current) : -1;
  const bool has_current = (cheat_index >= 0);
  m_ui.cheatListEdit->setEnabled(has_current);
  m_ui.cheatListRemove->setEnabled(has_current);
  m_ui.cheatListActivate->setEnabled(has_current);

  if (!has_current)
  {
    m_ui.cheatListActivate->setText(tr("Activate"));
  }
  else
  {
    const bool manual_activation = getCheatList()->GetCode(static_cast<u32>(cheat_index)).IsManuallyActivated();
    m_ui.cheatListActivate->setText(manual_activation ? tr("Activate") : tr("Toggle"));
  }
}

void CheatManagerDialog::cheatListItemActivated(QTreeWidgetItem* item)
{
  if (!item)
    return;

  const int index = getCheatIndexFromItem(item);
  if (index >= 0)
    activateCheat(static_cast<u32>(index));
}

void CheatManagerDialog::cheatListItemChanged(QTreeWidgetItem* item, int column)
{
  if (!item || column != 0)
    return;

  const int index = getCheatIndexFromItem(item);
  if (index < 0)
    return;

  CheatList* list = getCheatList();
  if (static_cast<u32>(index) >= list->GetCodeCount())
    return;

  CheatCode& cc = list->GetCode(static_cast<u32>(index));
  if (cc.IsManuallyActivated())
    return;

  const bool new_enabled = (item->checkState(0) == Qt::Checked);
  if (cc.enabled == new_enabled)
    return;

  QtHostInterface::GetInstance()->executeOnEmulationThread([index, new_enabled]() {
    System::GetCheatList()->SetCodeEnabled(static_cast<u32>(index), new_enabled);
    QtHostInterface::GetInstance()->SaveCheatList();
  });
}

void CheatManagerDialog::activateCheat(u32 index)
{
  CheatList* list = getCheatList();
  if (index >= list->GetCodeCount())
    return;

  CheatCode& cc = list->GetCode(index);
  if (cc.IsManuallyActivated())
  {
    QtHostInterface::GetInstance()->applyCheat(index);
    return;
  }

  const bool new_enabled = !cc.enabled;
  QTreeWidgetItem* item = getItemForCheatIndex(index);
  if (item)
  {
    QSignalBlocker sb(m_ui.cheatList);
    item->setCheckState(0, new_enabled ? Qt::Checked : Qt::Unchecked);
  }

  QtHostInterface::GetInstance()->executeOnEmulationThread([index, new_enabled]() {
    System::GetCheatList()->SetCodeEnabled(index, new_enabled);
    QtHostInterface::GetInstance()->SaveCheatList();
  });
}

void CheatManagerDialog::newCategoryClicked()
{
  QString group_name = QInputDialog::getText(this, tr("Add Group"), tr("Group Name:"));
  if (group_name.isEmpty())
    return;

  if (getItemForCheatGroup(group_name) != nullptr)
  {
    QMessageBox::critical(this, tr("Error"), tr("This group name already exists."));
    return;
  }

  createItemForCheatGroup(group_name);
}

void CheatManagerDialog::addCodeClicked()
{
  CheatList* list = getCheatList();

  CheatCode new_code;
  new_code.group = "Ungrouped";

  CheatCodeEditorDialog editor(getCheatGroupNames(), &new_code, this);
  if (editor.exec() > 0)
  {
    const QString group_name_qstr(QString::fromStdString(new_code.group));
    QTreeWidgetItem* group_item = getItemForCheatGroup(group_name_qstr);
    if (!group_item)
      group_item = createItemForCheatGroup(group_name_qstr);

    QTreeWidgetItem* item = new QTreeWidgetItem(group_item);
    fillItemForCheatCode(item, list->GetCodeCount(), new_code);
    group_item->setExpanded(true);

    QtHostInterface::GetInstance()->executeOnEmulationThread(
      [this, &new_code]() {
        System::GetCheatList()->AddCode(std::move(new_code));
        QtHostInterface::GetInstance()->SaveCheatList();
      },
      true);
  }
}

void CheatManagerDialog::editCodeClicked()
{
  int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  CheatList* list = getCheatList();
  if (static_cast<u32>(index) >= list->GetCodeCount())
    return;

  CheatCode new_code = list->GetCode(static_cast<u32>(index));
  CheatCodeEditorDialog editor(getCheatGroupNames(), &new_code, this);
  if (editor.exec() > 0)
  {
    QTreeWidgetItem* item = getItemForCheatIndex(static_cast<u32>(index));
    if (item)
    {
      if (new_code.group != list->GetCode(static_cast<u32>(index)).group)
      {
        item = item->parent()->takeChild(item->parent()->indexOfChild(item));

        const QString group_name_qstr(QString::fromStdString(new_code.group));
        QTreeWidgetItem* group_item = getItemForCheatGroup(group_name_qstr);
        if (!group_item)
          group_item = createItemForCheatGroup(group_name_qstr);
        group_item->addChild(item);
        group_item->setExpanded(true);
      }

      fillItemForCheatCode(item, static_cast<u32>(index), new_code);
    }
    else
    {
      // shouldn't happen...
      updateCheatList();
    }

    QtHostInterface::GetInstance()->executeOnEmulationThread(
      [index, &new_code]() {
        System::GetCheatList()->SetCode(static_cast<u32>(index), std::move(new_code));
        QtHostInterface::GetInstance()->SaveCheatList();
      },
      true);
  }
}

void CheatManagerDialog::deleteCodeClicked()
{
  int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  CheatList* list = getCheatList();
  if (static_cast<u32>(index) >= list->GetCodeCount())
    return;

  if (QMessageBox::question(this, tr("Delete Code"),
                            tr("Are you sure you wish to delete the selected code? This action is not reversible."),
                            QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
  {
    return;
  }

  QtHostInterface::GetInstance()->executeOnEmulationThread(
    [index]() {
      System::GetCheatList()->RemoveCode(static_cast<u32>(index));
      QtHostInterface::GetInstance()->SaveCheatList();
    },
    true);
  updateCheatList();
}

void CheatManagerDialog::activateCodeClicked()
{
  int index = getSelectedCheatIndex();
  if (index < 0)
    return;

  activateCheat(static_cast<u32>(index));
}

void CheatManagerDialog::importClicked()
{
  const QString filter(tr("PCSXR/Libretro Cheat Files (*.cht *.txt);;All Files (*.*)"));
  const QString filename(QFileDialog::getOpenFileName(this, tr("Import Cheats"), QString(), filter));
  if (filename.isEmpty())
    return;

  CheatList new_cheats;
  if (!new_cheats.LoadFromFile(filename.toUtf8().constData(), CheatList::Format::Autodetect))
  {
    QMessageBox::critical(this, tr("Error"), tr("Failed to parse cheat file. The log may contain more information."));
    return;
  }

  QtHostInterface::GetInstance()->executeOnEmulationThread(
    [&new_cheats]() {
      DebugAssert(System::HasCheatList());
      System::GetCheatList()->MergeList(new_cheats);
      QtHostInterface::GetInstance()->SaveCheatList();
    },
    true);
  updateCheatList();
}

void CheatManagerDialog::exportClicked()
{
  const QString filter(tr("PCSXR Cheat Files (*.cht);;All Files (*.*)"));
  const QString filename(QFileDialog::getSaveFileName(this, tr("Export Cheats"), QString(), filter));
  if (filename.isEmpty())
    return;

  if (!getCheatList()->SaveToPCSXRFile(filename.toUtf8().constData()))
    QMessageBox::critical(this, tr("Error"), tr("Failed to save cheat file. The log may contain more information."));
}

void CheatManagerDialog::addToWatchClicked()
{
  const int index = getSelectedResultIndex();
  if (index < 0)
    return;

  const MemoryScan::Result& res = m_scanner.GetResults()[static_cast<u32>(index)];
  m_watch.AddEntry(StringUtil::StdStringFromFormat("0x%08x", res.address), res.address, m_scanner.GetSize(),
                   m_scanner.GetValueSigned(), false);
  updateWatch();
}

void CheatManagerDialog::removeWatchClicked()
{
  const int index = getSelectedWatchIndex();
  if (index < 0)
    return;

  m_watch.RemoveEntry(static_cast<u32>(index));
  updateWatch();
}

void CheatManagerDialog::scanCurrentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous)
{
  m_ui.scanAddWatch->setEnabled((current != nullptr));
}

void CheatManagerDialog::watchCurrentItemChanged(QTableWidgetItem* current, QTableWidgetItem* previous)
{
  m_ui.scanRemoveWatch->setEnabled((current != nullptr));
}

void CheatManagerDialog::scanItemChanged(QTableWidgetItem* item)
{
  const u32 index = static_cast<u32>(item->row());
  switch (item->column())
  {
    case 1:
    {
      bool value_ok = false;
      if (m_scanner.GetValueSigned())
      {
        int value = item->text().toInt(&value_ok);
        if (value_ok)
          m_scanner.SetResultValue(index, static_cast<u32>(value));
      }
      else
      {
        uint value = item->text().toUInt(&value_ok);
        if (value_ok)
          m_scanner.SetResultValue(index, static_cast<u32>(value));
      }
    }
    break;

    default:
      break;
  }
}

void CheatManagerDialog::watchItemChanged(QTableWidgetItem* item)
{
  const u32 index = static_cast<u32>(item->row());
  if (index >= m_watch.GetEntryCount())
    return;

  switch (item->column())
  {
    case 0:
    {
      m_watch.SetEntryFreeze(index, (item->checkState() == Qt::Checked));
    }
    break;

    case 1:
    {
      m_watch.SetEntryDescription(index, item->text().toStdString());
    }
    break;

    case 4:
    {
      const MemoryWatchList::Entry& entry = m_watch.GetEntry(index);
      bool value_ok = false;
      if (entry.is_signed)
      {
        int value = item->text().toInt(&value_ok);
        if (value_ok)
          m_watch.SetEntryValue(index, static_cast<u32>(value));
      }
      else
      {
        uint value = item->text().toUInt(&value_ok);
        if (value_ok)
          m_watch.SetEntryValue(index, static_cast<u32>(value));
      }
    }
    break;

    default:
      break;
  }
}

void CheatManagerDialog::updateScanValue()
{
  QString value = m_ui.scanValue->text();
  if (value.startsWith(QStringLiteral("0x")))
    value.remove(0, 2);

  bool ok = false;
  uint uint_value = value.toUInt(&ok, (m_ui.scanValueBase->currentIndex() > 0) ? 16 : 10);
  if (ok)
    m_scanner.SetValue(uint_value);
}

void CheatManagerDialog::updateResults()
{
  QSignalBlocker sb(m_ui.scanTable);
  m_ui.scanTable->setRowCount(0);

  const MemoryScan::ResultVector& results = m_scanner.GetResults();
  if (!results.empty())
  {
    int row = 0;
    for (const MemoryScan::Result& res : m_scanner.GetResults())
    {
      if (row == MAX_DISPLAYED_SCAN_RESULTS)
      {
        QMessageBox::information(this, tr("Memory Scan"),
                                 tr("Memory scan found %1 addresses, but only the first %2 are displayed.")
                                   .arg(m_scanner.GetResultCount())
                                   .arg(MAX_DISPLAYED_SCAN_RESULTS));
        break;
      }

      m_ui.scanTable->insertRow(row);

      QTableWidgetItem* address_item = new QTableWidgetItem(formatHexValue(res.address));
      address_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      m_ui.scanTable->setItem(row, 0, address_item);

      QTableWidgetItem* value_item = new QTableWidgetItem(formatValue(res.value, m_scanner.GetValueSigned()));
      m_ui.scanTable->setItem(row, 1, value_item);

      QTableWidgetItem* previous_item = new QTableWidgetItem(formatValue(res.last_value, m_scanner.GetValueSigned()));
      previous_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      m_ui.scanTable->setItem(row, 2, previous_item);
      row++;
    }
  }

  m_ui.scanResetSearch->setEnabled(!results.empty());
  m_ui.scanSearchAgain->setEnabled(!results.empty());
  m_ui.scanAddWatch->setEnabled(false);
}

void CheatManagerDialog::updateResultsValues()
{
  QSignalBlocker sb(m_ui.scanTable);

  int row = 0;
  for (const MemoryScan::Result& res : m_scanner.GetResults())
  {
    if (res.value_changed)
    {
      QTableWidgetItem* item = m_ui.scanTable->item(row, 1);
      item->setText(formatValue(res.value, m_scanner.GetValueSigned()));
      item->setForeground(Qt::red);
    }

    row++;
    if (row == MAX_DISPLAYED_SCAN_RESULTS)
      break;
  }
}

void CheatManagerDialog::updateWatch()
{
  static constexpr std::array<const char*, 6> size_strings = {
    {QT_TR_NOOP("Byte"), QT_TR_NOOP("Halfword"), QT_TR_NOOP("Word"), QT_TR_NOOP("Signed Byte"),
     QT_TR_NOOP("Signed Halfword"), QT_TR_NOOP("Signed Word")}};

  m_watch.UpdateValues();

  QSignalBlocker sb(m_ui.watchTable);
  m_ui.watchTable->setRowCount(0);

  const MemoryWatchList::EntryVector& entries = m_watch.GetEntries();
  if (!entries.empty())
  {
    int row = 0;
    for (const MemoryWatchList::Entry& res : entries)
    {
      m_ui.watchTable->insertRow(row);

      QTableWidgetItem* freeze_item = new QTableWidgetItem();
      freeze_item->setFlags(freeze_item->flags() | (Qt::ItemIsEditable | Qt::ItemIsUserCheckable));
      freeze_item->setCheckState(res.freeze ? Qt::Checked : Qt::Unchecked);
      m_ui.watchTable->setItem(row, 0, freeze_item);

      QTableWidgetItem* description_item = new QTableWidgetItem(QString::fromStdString(res.description));
      m_ui.watchTable->setItem(row, 1, description_item);

      QTableWidgetItem* address_item = new QTableWidgetItem(formatHexValue(res.address));
      address_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      m_ui.watchTable->setItem(row, 2, address_item);

      QTableWidgetItem* size_item =
        new QTableWidgetItem(tr(size_strings[static_cast<u32>(res.size) + (res.is_signed ? 3 : 0)]));
      size_item->setFlags(address_item->flags() & ~(Qt::ItemIsEditable));
      m_ui.watchTable->setItem(row, 3, size_item);

      QTableWidgetItem* value_item = new QTableWidgetItem(formatValue(res.value, res.is_signed));
      m_ui.watchTable->setItem(row, 4, value_item);

      row++;
    }
  }

  m_ui.scanSaveWatch->setEnabled(!entries.empty());
  m_ui.scanRemoveWatch->setEnabled(false);
}

void CheatManagerDialog::updateWatchValues()
{
  QSignalBlocker sb(m_ui.watchTable);
  int row = 0;
  for (const MemoryWatchList::Entry& res : m_watch.GetEntries())
  {
    if (res.changed)
      m_ui.watchTable->item(row, 4)->setText(formatValue(res.value, res.is_signed));

    row++;
  }
}

void CheatManagerDialog::updateScanUi()
{
  m_scanner.UpdateResultsValues();
  m_watch.UpdateValues();

  updateResultsValues();
  updateWatchValues();
}
