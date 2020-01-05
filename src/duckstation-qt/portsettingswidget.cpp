#include "portsettingswidget.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtCore/QTimer>
#include <QtGui/QKeyEvent>

PortSettingsWidget::PortSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  createUi();
}

PortSettingsWidget::~PortSettingsWidget() = default;

void PortSettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);

  m_tab_widget = new QTabWidget(this);
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
    createPortSettingsUi(i, &m_port_ui[i]);

  layout->addWidget(m_tab_widget, 0, 0, 1, 1);

  setLayout(layout);
}

void PortSettingsWidget::createPortSettingsUi(int index, PortSettingsUI* ui)
{
  const Settings& settings = m_host_interface->GetCoreSettings();

  ui->widget = new QWidget(m_tab_widget);
  ui->layout = new QVBoxLayout(ui->widget);

  QHBoxLayout* memory_card_layout = new QHBoxLayout();
  ui->memory_card_path = new QLineEdit(QString::fromStdString(settings.memory_card_paths[index]), ui->widget);
  memory_card_layout->addWidget(ui->memory_card_path);
  ui->memory_card_path_browse = new QPushButton(tr("Browse..."), ui->widget);
  memory_card_layout->addWidget(ui->memory_card_path_browse);
  ui->layout->addWidget(new QLabel(tr("Memory Card Path:"), ui->widget));
  ui->layout->addLayout(memory_card_layout);

  ui->controller_type = new QComboBox(ui->widget);
  for (int i = 0; i < static_cast<int>(ControllerType::Count); i++)
  {
    ui->controller_type->addItem(
      QString::fromLocal8Bit(Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(i))));
  }
  ui->controller_type->setCurrentIndex(static_cast<int>(settings.controller_types[index]));
  connect(ui->controller_type, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
          [this, index]() { onControllerTypeChanged(index); });
  ui->layout->addWidget(new QLabel(tr("Controller Type:"), ui->widget));
  ui->layout->addWidget(ui->controller_type);

  createPortBindingSettingsUi(index, ui);

  ui->layout->addStretch(1);

  ui->widget->setLayout(ui->layout);

  m_tab_widget->addTab(ui->widget, tr("Port %1").arg(index + 1));
}

void PortSettingsWidget::createPortBindingSettingsUi(int index, PortSettingsUI* ui)
{
  QWidget* container = new QWidget(ui->widget);
  QGridLayout* layout = new QGridLayout(container);
  layout->setContentsMargins(0, 0, 0, 0);
  const ControllerType ctype = m_host_interface->GetCoreSettings().controller_types[index];
  const auto buttons = Controller::GetButtonNames(ctype);

  if (!buttons.empty())
  {
    QFrame* line = new QFrame(container);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line, 0, 0, 1, 4);
    layout->addWidget(new QLabel(tr("Button Bindings:"), container), 1, 0, 1, 4);

    const int start_row = 2;
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

      current_row++;
    }
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

  m_host_interface->GetCoreSettings().controller_types[index] = static_cast<ControllerType>(type_index);
  m_host_interface->getQSettings().setValue(
    QStringLiteral("Controller%1/Type").arg(index + 1),
    QString::fromStdString(Settings::GetControllerTypeName(static_cast<ControllerType>(type_index))));
  createPortBindingSettingsUi(index, &m_port_ui[index]);
}
