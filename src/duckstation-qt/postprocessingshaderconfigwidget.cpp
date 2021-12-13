#include "postprocessingshaderconfigwidget.h"
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSlider>

using FrontendCommon::PostProcessingShader;

PostProcessingShaderConfigWidget::PostProcessingShaderConfigWidget(QWidget* parent,
                                                                   FrontendCommon::PostProcessingShader* shader)
  : QWidget(parent), m_shader(shader)
{
  createUi();
}

PostProcessingShaderConfigWidget::~PostProcessingShaderConfigWidget() = default;

void PostProcessingShaderConfigWidget::createUi()
{
  m_layout = new QGridLayout(this);
  m_layout->setContentsMargins(0, 0, 0, 0);
  u32 row = 0;

  for (PostProcessingShader::Option& option : m_shader->GetOptions())
  {
    if (option.type == PostProcessingShader::Option::Type::Bool)
    {
      QCheckBox* checkbox = new QCheckBox(QString::fromStdString(option.ui_name), this);
      checkbox->setChecked(option.value[0].int_value != 0);
      connect(checkbox, &QCheckBox::stateChanged, [this, &option](int state) {
        option.value[0].int_value = (state == Qt::Checked) ? 1 : 0;
        configChanged();
      });
      connect(this, &PostProcessingShaderConfigWidget::resettingtoDefaults, [&option, checkbox]() {
        QSignalBlocker sb(checkbox);
        checkbox->setChecked(option.default_value[0].int_value != 0);
        option.value = option.default_value;
      });
      m_layout->addWidget(checkbox, row, 0, 1, 3, Qt::AlignLeft);
      row++;
    }
    else
    {
      for (u32 i = 0; i < option.vector_size; i++)
      {
        QString label;
        if (option.vector_size <= 1)
        {
          label = QString::fromStdString(option.ui_name);
        }
        else
        {
          static constexpr std::array<const char*, PostProcessingShader::Option::MAX_VECTOR_COMPONENTS + 1> suffixes = {
            {QT_TR_NOOP("Red"), QT_TR_NOOP("Green"), QT_TR_NOOP("Blue"), QT_TR_NOOP("Alpha")}};
          label = tr("%1 (%2)").arg(QString::fromStdString(option.ui_name)).arg(tr(suffixes[i]));
        }

        m_layout->addWidget(new QLabel(label, this), row, 0, 1, 1, Qt::AlignLeft);

        QSlider* slider = new QSlider(Qt::Horizontal, this);
        m_layout->addWidget(slider, row, 1, 1, 1, Qt::AlignLeft);

        QLabel* slider_label = new QLabel(this);
        m_layout->addWidget(slider_label, row, 2, 1, 1, Qt::AlignLeft);

        if (option.type == PostProcessingShader::Option::Type::Int)
        {
          slider_label->setText(QString::number(option.value[i].int_value));

          const int range = std::max(option.max_value[i].int_value - option.min_value[i].int_value, 1);
          const int step_value =
            (option.step_value[i].int_value != 0) ? option.step_value[i].int_value : ((range + 99) / 100);
          const int num_steps = range / step_value;
          slider->setMinimum(0);
          slider->setMaximum(num_steps);
          slider->setSingleStep(1);
          slider->setTickInterval(step_value);
          slider->setValue((option.value[i].int_value - option.min_value[i].int_value) / step_value);
          connect(slider, &QSlider::valueChanged, [this, &option, i, slider_label, step_value](int value) {
            const int new_value = std::clamp(option.min_value[i].int_value + (value * option.step_value[i].int_value),
                                             option.min_value[i].int_value, option.max_value[i].int_value);
            option.value[i].int_value = new_value;
            slider_label->setText(QString::number(new_value));
            configChanged();
          });
          connect(this, &PostProcessingShaderConfigWidget::resettingtoDefaults,
                  [&option, i, slider, slider_label, step_value]() {
                    QSignalBlocker sb(slider);
                    slider->setValue((option.default_value[i].int_value - option.min_value[i].int_value) / step_value);
                    slider_label->setText(QString::number(option.default_value[i].int_value));
                    option.value = option.default_value;
                  });
        }
        else
        {
          slider_label->setText(QString::number(option.value[i].float_value));

          const float range = std::max(option.max_value[i].float_value - option.min_value[i].float_value, 1.0f);
          const float step_value =
            (option.step_value[i].float_value != 0) ? option.step_value[i].float_value : ((range + 99.0f) / 100.0f);
          const float num_steps = range / step_value;
          slider->setMinimum(0);
          slider->setMaximum(num_steps);
          slider->setSingleStep(1);
          slider->setTickInterval(step_value);
          slider->setValue(
            static_cast<int>((option.value[i].float_value - option.min_value[i].float_value) / step_value));
          connect(slider, &QSlider::valueChanged, [this, &option, i, slider_label, step_value](int value) {
            const float new_value = std::clamp(option.min_value[i].float_value +
                                                 (static_cast<float>(value) * option.step_value[i].float_value),
                                               option.min_value[i].float_value, option.max_value[i].float_value);
            option.value[i].float_value = new_value;
            slider_label->setText(QString::number(new_value));
            configChanged();
          });
          connect(this, &PostProcessingShaderConfigWidget::resettingtoDefaults,
                  [&option, i, slider, slider_label, step_value]() {
                    QSignalBlocker sb(slider);
                    slider->setValue(static_cast<int>(
                      (option.default_value[i].float_value - option.min_value[i].float_value) / step_value));
                    slider_label->setText(QString::number(option.default_value[i].float_value));
                    option.value = option.default_value;
                  });
        }

        row++;
      }
    }
  }

  QDialogButtonBox* button_box = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults, this);
  connect(button_box, &QDialogButtonBox::clicked, this, &PostProcessingShaderConfigWidget::onResetToDefaultsClicked);
  m_layout->addWidget(button_box, row, 0, 1, -1);

  row++;
  m_layout->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding), row, 0, 1, 3);
}

void PostProcessingShaderConfigWidget::onResetToDefaultsClicked()
{
  resettingtoDefaults();
  configChanged();
}

PostProcessingShaderConfigDialog::PostProcessingShaderConfigDialog(QWidget* parent,
                                                                   FrontendCommon::PostProcessingShader* shader)
  : QDialog(parent)
{
  setWindowTitle(tr("%1 Shader Options").arg(QString::fromStdString(shader->GetName())));
  setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

  QGridLayout* layout = new QGridLayout(this);
  m_widget = new PostProcessingShaderConfigWidget(this, shader);
  layout->addWidget(m_widget);

  connect(m_widget, &PostProcessingShaderConfigWidget::configChanged, this,
          &PostProcessingShaderConfigDialog::onConfigChanged);

  QDialogButtonBox* button_box = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(button_box, &QDialogButtonBox::rejected, this, &PostProcessingShaderConfigDialog::onCloseClicked);
  m_widget->getLayout()->addWidget(button_box, m_widget->getLayout()->rowCount() - 1, 2, 1, 2);
}

PostProcessingShaderConfigDialog::~PostProcessingShaderConfigDialog() = default;

void PostProcessingShaderConfigDialog::onConfigChanged()
{
  configChanged();
}

void PostProcessingShaderConfigDialog::onCloseClicked()
{
  done(0);
}
