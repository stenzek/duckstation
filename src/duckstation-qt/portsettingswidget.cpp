#include "portsettingswidget.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QMessageBox>

PortSettingsWidget::PortSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  createUi();
}

PortSettingsWidget::~PortSettingsWidget() = default;

void PortSettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  m_tab_widget = new QTabWidget(this);
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
    createPortSettingsUi(i, &m_port_ui[i]);

  layout->addWidget(m_tab_widget, 0, 0, 1, 1);

  setLayout(layout);
}

void PortSettingsWidget::createPortSettingsUi(int index, PortSettingsUI* ui)
{
  ui->widget = new QWidget(m_tab_widget);
  ui->layout = new QVBoxLayout(ui->widget);

  QHBoxLayout* memory_card_layout = new QHBoxLayout();
  ui->memory_card_path = new QLineEdit(ui->widget);
  SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, ui->memory_card_path,
                                                 QStringLiteral("MemoryCards/Card%1Path").arg(index + 1));
  memory_card_layout->addWidget(ui->memory_card_path);

  ui->memory_card_path_browse = new QPushButton(tr("Browse..."), ui->widget);
  memory_card_layout->addWidget(ui->memory_card_path_browse);
  ui->layout->addWidget(new QLabel(tr("Memory Card Path:"), ui->widget));
  ui->layout->addLayout(memory_card_layout);

  ui->layout->addWidget(new QLabel(tr("Controller Type:"), ui->widget));

  ui->controller_type = new QComboBox(ui->widget);
  for (int i = 0; i < static_cast<int>(ControllerType::Count); i++)
  {
    ui->controller_type->addItem(
      QString::fromLocal8Bit(Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(i))));
  }
  ControllerType ctype = Settings::ParseControllerTypeName(
                           m_host_interface->getSettingValue(QStringLiteral("Controller%1/Type").arg(index + 1))
                             .toString()
                             .toStdString()
                             .c_str())
                           .value_or(ControllerType::None);
  ui->controller_type->setCurrentIndex(static_cast<int>(ctype));
  connect(ui->controller_type, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
          [this, index]() { onControllerTypeChanged(index); });

  ui->layout->addWidget(ui->controller_type);

  createPortBindingSettingsUi(index, ui, ctype);

  ui->layout->addStretch(1);

  ui->widget->setLayout(ui->layout);

  m_tab_widget->addTab(ui->widget, tr("Port %1").arg(index + 1));
}

void PortSettingsWidget::createPortBindingSettingsUi(int index, PortSettingsUI* ui, ControllerType ctype)
{
  QWidget* container = new QWidget(ui->widget);
  QGridLayout* layout = new QGridLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  const auto buttons = Controller::GetButtonNames(ctype);

  InputBindingWidget* first_button = nullptr;
  InputBindingWidget* last_button = nullptr;

  int start_row = 0;
  if (!buttons.empty())
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(container), start_row++, 0, 1, 4);
    layout->addWidget(new QLabel(tr("Button Bindings:"), container), start_row++, 0, 1, 4);

    const int num_rows = (static_cast<int>(buttons.size()) + 1) / 2;
    int current_row = 0;
    int current_column = 0;
    for (const auto& [button_name, button_code] : buttons)
    {
      if (current_row == num_rows)
      {
        current_row = 0;
        current_column += 2;
      }

      const QString button_name_q = QString::fromStdString(button_name);
      const QString setting_name = QStringLiteral("Controller%1/Button%2").arg(index + 1).arg(button_name_q);
      QLabel* label = new QLabel(button_name_q, container);
      InputButtonBindingWidget* button = new InputButtonBindingWidget(m_host_interface, setting_name, container);
      layout->addWidget(label, start_row + current_row, current_column);
      layout->addWidget(button, start_row + current_row, current_column + 1);

      if (!first_button)
        first_button = button;
      if (last_button)
        last_button->setNextWidget(button);
      last_button = button;

      current_row++;
    }

    start_row += num_rows;
  }

  const auto axises = Controller::GetAxisNames(ctype);
  if (!axises.empty())
  {
    QFrame* line = new QFrame(container);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line, start_row++, 0, 1, 4);
    layout->addWidget(new QLabel(tr("Axis Bindings:"), container), start_row++, 0, 1, 4);

    const int num_rows = (static_cast<int>(axises.size()) + 1) / 2;
    int current_row = 0;
    int current_column = 0;
    for (const auto& [axis_name, axis_code] : axises)
    {
      if (current_row == num_rows)
      {
        current_row = 0;
        current_column += 2;
      }

      const QString axis_name_q = QString::fromStdString(axis_name);
      const QString setting_name = QStringLiteral("Controller%1/Axis%2").arg(index + 1).arg(axis_name_q);
      QLabel* label = new QLabel(axis_name_q, container);
      InputAxisBindingWidget* button = new InputAxisBindingWidget(m_host_interface, setting_name, container);
      layout->addWidget(label, start_row + current_row, current_column);
      layout->addWidget(button, start_row + current_row, current_column + 1);

      if (!first_button)
        first_button = button;
      if (last_button)
        last_button->setNextWidget(button);
      last_button = button;

      current_row++;
    }

    start_row += num_rows;
  }

  layout->addWidget(QtUtils::CreateHorizontalLine(ui->widget), start_row++, 0, 1, 4);

  if (first_button)
  {
    QHBoxLayout* hbox = new QHBoxLayout();

    QPushButton* clear_all_button = new QPushButton(tr("Clear All"), ui->widget);
    clear_all_button->connect(clear_all_button, &QPushButton::pressed, [this, first_button]() {
      if (QMessageBox::question(this, tr("Clear Bindings"),
                                tr("Are you sure you want to clear all bound controls? This cannot be reversed.")) !=
          QMessageBox::Yes)
      {
        return;
      }

      InputBindingWidget* widget = first_button;
      while (widget)
      {
        widget->clearBinding();
        widget = widget->getNextWidget();
      }
    });

    QPushButton* rebind_all_button = new QPushButton(tr("Rebind All"), ui->widget);
    rebind_all_button->connect(rebind_all_button, &QPushButton::pressed, [this, first_button]() {
      if (QMessageBox::question(this, tr("Clear Bindings"), tr("Do you want to clear all currently-bound controls?")) ==
          QMessageBox::Yes)
      {
        InputBindingWidget* widget = first_button;
        while (widget)
        {
          widget->clearBinding();
          widget = widget->getNextWidget();
        }
      }

      first_button->beginRebindAll();
    });

    hbox->addWidget(clear_all_button);
    hbox->addWidget(rebind_all_button);
    layout->addLayout(hbox, start_row++, 0, 1, 4, Qt::AlignRight);
  }

  if (ui->button_binding_container)
  {
    QLayoutItem* old_item = ui->layout->replaceWidget(ui->button_binding_container, container);
    Q_ASSERT(old_item != nullptr);

    delete old_item;
    delete ui->button_binding_container;
  }
  else
  {
    ui->layout->addWidget(container);
  }
  ui->button_binding_container = container;
}

void PortSettingsWidget::onControllerTypeChanged(int index)
{
  const int type_index = m_port_ui[index].controller_type->currentIndex();
  if (type_index < 0 || type_index >= static_cast<int>(ControllerType::Count))
    return;

  m_host_interface->putSettingValue(
    QStringLiteral("Controller%1/Type").arg(index + 1),
    QString::fromStdString(Settings::GetControllerTypeName(static_cast<ControllerType>(type_index))));
  m_host_interface->applySettings();
  createPortBindingSettingsUi(index, &m_port_ui[index], static_cast<ControllerType>(type_index));
}
