#include "controllersettingswidget.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QCursor>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>

static constexpr char INPUT_PROFILE_FILTER[] = "Input Profiles (*.ini)";

ControllerSettingsWidget::ControllerSettingsWidget(QtHostInterface* host_interface, QWidget* parent /* = nullptr */)
  : QWidget(parent), m_host_interface(host_interface)
{
  createUi();

  connect(host_interface, &QtHostInterface::inputProfileLoaded, this, &ControllerSettingsWidget::onProfileLoaded);
}

ControllerSettingsWidget::~ControllerSettingsWidget() = default;

void ControllerSettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  m_tab_widget = new QTabWidget(this);
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
    createPortSettingsUi(i, &m_port_ui[i]);

  layout->addWidget(m_tab_widget, 0, 0, 1, 1);

  setLayout(layout);
}

void ControllerSettingsWidget::onProfileLoaded()
{
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
  {
    ControllerType ctype = Settings::ParseControllerTypeName(
                             m_host_interface->getSettingValue(QStringLiteral("Controller%1/Type").arg(i + 1))
                               .toString()
                               .toStdString()
                               .c_str())
                             .value_or(ControllerType::None);

    {
      QSignalBlocker blocker(m_port_ui[i].controller_type);
      m_port_ui[i].controller_type->setCurrentIndex(static_cast<int>(ctype));
    }
    createPortBindingSettingsUi(i, &m_port_ui[i], ctype);
  }
}

void ControllerSettingsWidget::reloadBindingButtons()
{
  for (PortSettingsUI& ui : m_port_ui)
  {
    InputBindingWidget* widget = ui.first_button;
    while (widget)
    {
      widget->reloadBinding();
      widget = widget->getNextWidget();
    }
  }
}

void ControllerSettingsWidget::createPortSettingsUi(int index, PortSettingsUI* ui)
{
  ui->widget = new QWidget(m_tab_widget);
  ui->layout = new QVBoxLayout(ui->widget);

  QHBoxLayout* hbox = new QHBoxLayout();
  hbox->addWidget(new QLabel(tr("Controller Type:"), ui->widget));
  hbox->addSpacing(8);

  ui->controller_type = new QComboBox(ui->widget);
  for (int i = 0; i < static_cast<int>(ControllerType::Count); i++)
  {
    ui->controller_type->addItem(
      QString::fromUtf8(Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(i))));
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

  hbox->addWidget(ui->controller_type, 1);
  ui->layout->addLayout(hbox);

  ui->bindings_scroll_area = new QScrollArea(ui->widget);
  ui->bindings_scroll_area->setWidgetResizable(true);
  ui->bindings_scroll_area->setFrameShape(QFrame::StyledPanel);
  ui->bindings_scroll_area->setFrameShadow(QFrame::Plain);

  createPortBindingSettingsUi(index, ui, ctype);

  ui->bindings_scroll_area->setWidget(ui->bindings_container);
  ui->layout->addWidget(ui->bindings_scroll_area, 1);

  hbox = new QHBoxLayout();
  QPushButton* load_profile_button = new QPushButton(tr("Load Profile"), ui->widget);
  connect(load_profile_button, &QPushButton::clicked, this, &ControllerSettingsWidget::onLoadProfileClicked);
  hbox->addWidget(load_profile_button);

  QPushButton* save_profile_button = new QPushButton(tr("Save Profile"), ui->widget);
  connect(save_profile_button, &QPushButton::clicked, this, &ControllerSettingsWidget::onSaveProfileClicked);
  hbox->addWidget(save_profile_button);

  hbox->addStretch(1);

  QPushButton* clear_all_button = new QPushButton(tr("Clear All"), ui->widget);
  clear_all_button->connect(clear_all_button, &QPushButton::clicked, [this, index]() {
    if (QMessageBox::question(this, tr("Clear Bindings"),
                              tr("Are you sure you want to clear all bound controls? This cannot be reversed.")) !=
        QMessageBox::Yes)
    {
      return;
    }

    InputBindingWidget* widget = m_port_ui[index].first_button;
    while (widget)
    {
      widget->clearBinding();
      widget = widget->getNextWidget();
    }
  });

  QPushButton* rebind_all_button = new QPushButton(tr("Rebind All"), ui->widget);
  rebind_all_button->connect(rebind_all_button, &QPushButton::clicked, [this, index]() {
    if (QMessageBox::question(this, tr("Clear Bindings"), tr("Do you want to clear all currently-bound controls?")) ==
        QMessageBox::Yes)
    {
      InputBindingWidget* widget = m_port_ui[index].first_button;
      while (widget)
      {
        widget->clearBinding();
        widget = widget->getNextWidget();
      }
    }

    if (m_port_ui[index].first_button)
      m_port_ui[index].first_button->beginRebindAll();
  });

  hbox->addWidget(clear_all_button);
  hbox->addWidget(rebind_all_button);

  ui->layout->addLayout(hbox);

  ui->widget->setLayout(ui->layout);

  m_tab_widget->addTab(ui->widget, tr("Port %1").arg(index + 1));
}

void ControllerSettingsWidget::createPortBindingSettingsUi(int index, PortSettingsUI* ui, ControllerType ctype)
{
  ui->bindings_container = new QWidget(ui->widget);

  QGridLayout* layout = new QGridLayout(ui->bindings_container);
  const auto buttons = Controller::GetButtonNames(ctype);

  InputBindingWidget* first_button = nullptr;
  InputBindingWidget* last_button = nullptr;

  int start_row = 0;
  if (!buttons.empty())
  {
    layout->addWidget(new QLabel(tr("Button Bindings:"), ui->bindings_container), start_row++, 0, 1, 4);

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
      QLabel* label = new QLabel(button_name_q, ui->bindings_container);
      InputButtonBindingWidget* button =
        new InputButtonBindingWidget(m_host_interface, setting_name, ui->bindings_container);
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
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->bindings_container), start_row++, 0, 1, 4);
    layout->addWidget(new QLabel(tr("Axis Bindings:"), ui->bindings_container), start_row++, 0, 1, 4);

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
      QLabel* label = new QLabel(axis_name_q, ui->bindings_container);
      InputAxisBindingWidget* button =
        new InputAxisBindingWidget(m_host_interface, setting_name, ui->bindings_container);
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

  const u32 num_motors = Controller::GetVibrationMotorCount(ctype);
  if (num_motors > 0)
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->widget), start_row++, 0, 1, 4);

    const QString setting_name = QStringLiteral("Controller%1/Rumble").arg(index + 1);
    QLabel* label = new QLabel(tr("Rumble"), ui->bindings_container);
    InputRumbleBindingWidget* button =
      new InputRumbleBindingWidget(m_host_interface, setting_name, ui->bindings_container);

    layout->addWidget(label, start_row, 0);
    layout->addWidget(button, start_row, 1);

    if (!first_button)
      first_button = button;
    if (last_button)
      last_button->setNextWidget(button);
    last_button = button;

    start_row++;
  }

  // dummy row to fill remaining space
  layout->addWidget(new QWidget(ui->bindings_container), start_row, 0, 1, 4);
  layout->setRowStretch(start_row, 1);

  ui->bindings_scroll_area->setWidget(ui->bindings_container);
  ui->first_button = first_button;
}

void ControllerSettingsWidget::onControllerTypeChanged(int index)
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

void ControllerSettingsWidget::onLoadProfileClicked()
{
  const auto profile_names = m_host_interface->getInputProfileList();

  QMenu menu;

  QAction* browse = menu.addAction(tr("Browse..."));
  connect(browse, &QAction::triggered, [this]() {
    QString path =
      QFileDialog::getOpenFileName(this, tr("Select path to input profile ini"), QString(), tr(INPUT_PROFILE_FILTER));
    if (!path.isEmpty())
      m_host_interface->applyInputProfile(path);
  });

  if (!profile_names.empty())
    menu.addSeparator();

  for (const auto& [name, path] : profile_names)
  {
    QAction* action = menu.addAction(QString::fromStdString(name));
    QString path_qstr = QString::fromStdString(path);
    connect(action, &QAction::triggered, [this, path_qstr]() { m_host_interface->applyInputProfile(path_qstr); });
  }

  menu.exec(QCursor::pos());
}

void ControllerSettingsWidget::onSaveProfileClicked()
{
  const auto profile_names = m_host_interface->getInputProfileList();

  QMenu menu;

  QAction* new_action = menu.addAction(tr("New..."));
  connect(new_action, &QAction::triggered, [this]() {
    QString name = QInputDialog::getText(QtUtils::GetRootWidget(this), tr("Enter Input Profile Name"),
                                         tr("Enter Input Profile Name"));
    if (name.isEmpty())
    {
      QMessageBox::critical(QtUtils::GetRootWidget(this), tr("Error"),
                            tr("No name entered, input profile was not saved."));
      return;
    }

    m_host_interface->saveInputProfile(m_host_interface->getPathForInputProfile(name));
  });

  QAction* browse = menu.addAction(tr("Browse..."));
  connect(browse, &QAction::triggered, [this]() {
    QString path = QFileDialog::getSaveFileName(QtUtils::GetRootWidget(this), tr("Select path to input profile ini"),
                                                QString(), tr(INPUT_PROFILE_FILTER));
    if (path.isEmpty())
    {
      QMessageBox::critical(QtUtils::GetRootWidget(this), tr("Error"),
                            tr("No path selected, input profile was not saved."));
      return;
    }

    m_host_interface->saveInputProfile(path);
  });

  if (!profile_names.empty())
    menu.addSeparator();

  for (const auto& [name, path] : profile_names)
  {
    QAction* action = menu.addAction(QString::fromStdString(name));
    QString path_qstr = QString::fromStdString(path);
    connect(action, &QAction::triggered, [this, path_qstr]() { m_host_interface->saveInputProfile(path_qstr); });
  }

  menu.exec(QCursor::pos());
}
