#include "controllersettingswidget.h"
#include "collapsiblewidget.h"
#include "common/string_util.h"
#include "core/controller.h"
#include "core/settings.h"
#include "inputbindingwidgets.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include "settingwidgetbinder.h"
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
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

MultitapMode ControllerSettingsWidget::getMultitapMode()
{
  return Settings::ParseMultitapModeName(
           QtHostInterface::GetInstance()
             ->GetStringSettingValue("ControllerPorts", "MultitapMode",
                                     Settings::GetMultitapModeName(Settings::DEFAULT_MULTITAP_MODE))
             .c_str())
    .value_or(Settings::DEFAULT_MULTITAP_MODE);
}

QString ControllerSettingsWidget::getTabTitleForPort(u32 index, MultitapMode mode) const
{
  constexpr u32 NUM_PORTS_PER_MULTITAP = 4;

  u32 port_number, subport_number;

  switch (mode)
  {
    case MultitapMode::Port1Only:
    {
      if (index == NUM_PORTS_PER_MULTITAP)
        return tr("Port %1").arg((index / NUM_PORTS_PER_MULTITAP) + 1);
      else if (index > NUM_PORTS_PER_MULTITAP)
        return QString();

      port_number = 0;
      subport_number = index;
    }
    break;

    case MultitapMode::Port2Only:
    {
      if (index == 0)
        return tr("Port %1").arg(index + 1);
      else if (index > NUM_PORTS_PER_MULTITAP)
        return QString();

      port_number = 1;
      subport_number = (index - 1);
    }
    break;

    case MultitapMode::BothPorts:
    {
      port_number = index / NUM_PORTS_PER_MULTITAP;
      subport_number = (index % NUM_PORTS_PER_MULTITAP);
    }
    break;

    case MultitapMode::Disabled:
    default:
    {
      if (index >= (NUM_CONTROLLER_AND_CARD_PORTS / NUM_PORTS_PER_MULTITAP))
        return QString();

      return tr("Port %1").arg(index + 1);
    }
  }

  return tr("Port %1%2").arg(port_number + 1).arg(QChar::fromLatin1('A' + subport_number));
}

void ControllerSettingsWidget::createUi()
{
  QGridLayout* layout = new QGridLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  const MultitapMode multitap_mode = getMultitapMode();
  m_tab_widget = new QTabWidget(this);
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
    createPortSettingsUi(i, &m_port_ui[i], multitap_mode);

  layout->addWidget(m_tab_widget, 0, 0, 1, 1);

  setLayout(layout);
}

void ControllerSettingsWidget::updateMultitapControllerTitles()
{
  m_tab_widget->clear();

  const MultitapMode multitap_mode = getMultitapMode();
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
    createPortSettingsUi(i, &m_port_ui[i], multitap_mode);
}

void ControllerSettingsWidget::onProfileLoaded()
{
  for (int i = 0; i < static_cast<int>(m_port_ui.size()); i++)
  {
    if (!m_port_ui[i].widget)
      continue;

    ControllerType ctype = Settings::ParseControllerTypeName(
                             m_host_interface
                               ->GetStringSettingValue(QStringLiteral("Controller%1").arg(i + 1).toStdString().c_str(),
                                                       QStringLiteral("Type").toStdString().c_str())
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

void ControllerSettingsWidget::createPortSettingsUi(int index, PortSettingsUI* ui, MultitapMode multitap_mode)
{
  if (ui->widget)
  {
    delete ui->widget;
    *ui = {};
  }

  const QString tab_title(getTabTitleForPort(index, multitap_mode));
  if (tab_title.isEmpty())
    return;

  ui->widget = new QWidget(m_tab_widget);
  ui->layout = new QVBoxLayout(ui->widget);

  QHBoxLayout* hbox = new QHBoxLayout();
  hbox->addWidget(new QLabel(tr("Controller Type:"), ui->widget));
  hbox->addSpacing(8);

  ui->controller_type = new QComboBox(ui->widget);
  for (int i = 0; i < static_cast<int>(ControllerType::Count); i++)
  {
    ui->controller_type->addItem(
      qApp->translate("ControllerType", Settings::GetControllerTypeDisplayName(static_cast<ControllerType>(i))));
  }
  ControllerType ctype =
    Settings::ParseControllerTypeName(
      m_host_interface->GetStringSettingValue(TinyString::FromFormat("Controller%d", index + 1), "Type").c_str())
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
                              tr("Are you sure you want to clear all bound controls? This can not be reversed.")) !=
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
    if (QMessageBox::question(this, tr("Rebind All"),
                              tr("Are you sure you want to rebind all controls? All currently-bound controls will be "
                                 "irreversibly cleared. Rebinding will begin after confirmation.")) != QMessageBox::Yes)
    {
      return;
    }

    InputBindingWidget* widget = m_port_ui[index].first_button;
    while (widget)
    {
      widget->clearBinding();
      widget = widget->getNextWidget();
    }

    if (m_port_ui[index].first_button)
      m_port_ui[index].first_button->beginRebindAll();
  });

  hbox->addWidget(clear_all_button);
  hbox->addWidget(rebind_all_button);

  ui->layout->addLayout(hbox);

  ui->widget->setLayout(ui->layout);

  m_tab_widget->addTab(ui->widget, tab_title);
}

void ControllerSettingsWidget::createPortBindingSettingsUi(int index, PortSettingsUI* ui, ControllerType ctype)
{
  ui->bindings_container = new QWidget(ui->widget);

  QGridLayout* layout = new QGridLayout(ui->bindings_container);
  const auto buttons = Controller::GetButtonNames(ctype);
  const char* cname = Settings::GetControllerTypeName(ctype);

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

      std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
      std::string key_name = StringUtil::StdStringFromFormat("Button%s", button_name.c_str());
      QLabel* label = new QLabel(qApp->translate(cname, button_name.c_str()), ui->bindings_container);
      InputButtonBindingWidget* button = new InputButtonBindingWidget(m_host_interface, std::move(section_name),
                                                                      std::move(key_name), ui->bindings_container);
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
    for (const auto& [axis_name, axis_code, axis_type] : axises)
    {
      if (current_row == num_rows)
      {
        current_row = 0;
        current_column += 2;
      }

      std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
      std::string key_name = StringUtil::StdStringFromFormat("Axis%s", axis_name.c_str());
      QLabel* label = new QLabel(qApp->translate(cname, axis_name.c_str()), ui->bindings_container);
      InputAxisBindingWidget* button = new InputAxisBindingWidget(
        m_host_interface, std::move(section_name), std::move(key_name), axis_type, ui->bindings_container);
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

    std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
    QLabel* label = new QLabel(tr("Rumble"), ui->bindings_container);
    InputRumbleBindingWidget* button =
      new InputRumbleBindingWidget(m_host_interface, std::move(section_name), "Rumble", ui->bindings_container);

    layout->addWidget(label, start_row, 0);
    layout->addWidget(button, start_row, 1);

    if (!first_button)
      first_button = button;
    if (last_button)
      last_button->setNextWidget(button);
    last_button = button;

    start_row++;
  }

  const Controller::SettingList settings = Controller::GetSettings(ctype);
  if (!settings.empty())
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->widget), start_row++, 0, 1, 4);

    for (const SettingInfo& si : settings)
    {
      std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
      std::string key_name = si.key;
      const QString setting_tooltip = si.description ? qApp->translate(cname, si.description) : QString();

      switch (si.type)
      {
        case SettingInfo::Type::Boolean:
        {
          QCheckBox* cb = new QCheckBox(qApp->translate(cname, si.visible_name), ui->bindings_container);
          cb->setToolTip(setting_tooltip);
          SettingWidgetBinder::BindWidgetToBoolSetting(m_host_interface, cb, std::move(section_name),
                                                       std::move(key_name), si.BooleanDefaultValue());
          layout->addWidget(cb, start_row, 0, 1, 4);
          start_row++;
        }
        break;

        case SettingInfo::Type::Integer:
        {
          QSpinBox* sb = new QSpinBox(ui->bindings_container);
          sb->setToolTip(setting_tooltip);
          sb->setMinimum(si.IntegerMinValue());
          sb->setMaximum(si.IntegerMaxValue());
          sb->setSingleStep(si.IntegerStepValue());
          SettingWidgetBinder::BindWidgetToIntSetting(m_host_interface, sb, std::move(section_name),
                                                      std::move(key_name), si.IntegerDefaultValue());
          layout->addWidget(new QLabel(qApp->translate(cname, si.visible_name), ui->bindings_container), start_row, 0);
          layout->addWidget(sb, start_row, 1, 1, 3);
          start_row++;
        }
        break;

        case SettingInfo::Type::Float:
        {
          QDoubleSpinBox* sb = new QDoubleSpinBox(ui->bindings_container);
          sb->setToolTip(setting_tooltip);
          sb->setMinimum(si.FloatMinValue());
          sb->setMaximum(si.FloatMaxValue());
          sb->setSingleStep(si.FloatStepValue());
          SettingWidgetBinder::BindWidgetToFloatSetting(m_host_interface, sb, std::move(section_name),
                                                        std::move(key_name), si.FloatDefaultValue());
          layout->addWidget(new QLabel(qApp->translate(cname, si.visible_name), ui->bindings_container), start_row, 0);
          layout->addWidget(sb, start_row, 1, 1, 3);
          start_row++;
        }
        break;

        case SettingInfo::Type::String:
        {
          QLineEdit* le = new QLineEdit(ui->bindings_container);
          le->setToolTip(setting_tooltip);
          SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, le, std::move(section_name),
                                                         std::move(key_name), si.StringDefaultValue());
          layout->addWidget(new QLabel(qApp->translate(cname, si.visible_name), ui->bindings_container), start_row, 0);
          layout->addWidget(le, start_row, 1, 1, 3);
          start_row++;
        }
        break;

        case SettingInfo::Type::Path:
        {
          QLineEdit* le = new QLineEdit(ui->bindings_container);
          le->setToolTip(setting_tooltip);
          QPushButton* browse_button = new QPushButton(tr("Browse..."), ui->bindings_container);
          SettingWidgetBinder::BindWidgetToStringSetting(m_host_interface, le, std::move(section_name),
                                                         std::move(key_name), si.StringDefaultValue());
          connect(browse_button, &QPushButton::clicked, [this, le]() {
            QString path = QFileDialog::getOpenFileName(this, tr("Select File"));
            if (!path.isEmpty())
              le->setText(path);
          });

          QHBoxLayout* hbox = new QHBoxLayout();
          hbox->addWidget(le, 1);
          hbox->addWidget(browse_button);

          layout->addWidget(new QLabel(qApp->translate(cname, si.visible_name), ui->bindings_container), start_row, 0);
          layout->addLayout(hbox, start_row, 1, 1, 3);
          start_row++;
        }
        break;
      }
    }
  }

  // turbo/autofire
  if (ctype != ControllerType::None)
  {
    layout->addWidget(QtUtils::CreateHorizontalLine(ui->widget), start_row++, 0, 1, 4);

    CollapsibleWidget* collapsible = new CollapsibleWidget(tr("Auto Fire Buttons"), 100, ui->bindings_container);
    QGridLayout* autofire_layout = new QGridLayout();
    autofire_layout->setContentsMargins(0, 0, 0, 0);

    QVector<QPair<QString, QVariant>> option_list;
    option_list.push_back({});
    for (const auto& [button_name, button_code] : buttons)
      option_list.push_back({qApp->translate(cname, button_name.c_str()), QString::fromStdString(button_name)});

    for (u32 autofire_index = 0; autofire_index < QtHostInterface::NUM_CONTROLLER_AUTOFIRE_BUTTONS; autofire_index++)
    {
      std::string section_name = StringUtil::StdStringFromFormat("Controller%d", index + 1);
      autofire_layout->addWidget(new QLabel(tr("Auto Fire %1").arg(autofire_index + 1), collapsible), autofire_index,
                                 0);
      QComboBox* button_cb = new QComboBox(collapsible);
      for (const auto& it : option_list)
        button_cb->addItem(it.first, it.second);
      autofire_layout->addWidget(button_cb, autofire_index, 1);
      SettingWidgetBinder::BindWidgetToStringSetting(
        m_host_interface, button_cb, section_name,
        StringUtil::StdStringFromFormat("AutoFire%uButton", autofire_index + 1));

      InputButtonBindingWidget* binding_button = new InputButtonBindingWidget(
        m_host_interface, section_name, StringUtil::StdStringFromFormat("AutoFire%u", autofire_index + 1), collapsible);
      autofire_layout->addWidget(binding_button, autofire_index, 2);

      QSpinBox* frequency = new QSpinBox(collapsible);
      frequency->setMinimum(1);
      frequency->setMaximum(255);
      frequency->setSuffix(tr(" Frames"));
      autofire_layout->addWidget(frequency, autofire_index, 3);
      SettingWidgetBinder::BindWidgetToIntSetting(
        m_host_interface, frequency, std::move(section_name),
        StringUtil::StdStringFromFormat("AutoFire%uFrequency", autofire_index + 1),
        QtHostInterface::DEFAULT_AUTOFIRE_FREQUENCY);
    }

    collapsible->getScrollArea()->setFrameStyle(QFrame::NoFrame);
    collapsible->setContentLayout(autofire_layout);
    layout->addWidget(collapsible, start_row, 0, 1, 4);

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

  m_host_interface->SetStringSettingValue(TinyString::FromFormat("Controller%d", index + 1), "Type",
                                          Settings::GetControllerTypeName(static_cast<ControllerType>(type_index)));

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

    m_host_interface->saveInputProfile(m_host_interface->getSavePathForInputProfile(name));
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
