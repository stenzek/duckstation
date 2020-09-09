#include "common/log.h"
#include "mainwindow.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <cstdlib>
#include <memory>

int main(int argc, char* argv[])
{
  // Register any standard types we need elsewhere
  qRegisterMetaType<std::optional<bool>>();

  QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

  QApplication app(argc, argv);

#ifdef _WIN32
  // Use Segoe UI on Windows rather than MS Shell Dlg 2, courtesy of Dolphin.
  // Can be removed once switched to Qt 6.
  QApplication::setFont(QApplication::font("QMenu"));
#endif

  std::unique_ptr<QtHostInterface> host_interface = std::make_unique<QtHostInterface>();
  std::unique_ptr<SystemBootParameters> boot_params;
  if (!host_interface->parseCommandLineParameters(argc, argv, &boot_params))
    return EXIT_FAILURE;

  if (!host_interface->Initialize())
  {
    host_interface->Shutdown();
    QMessageBox::critical(nullptr, QObject::tr("DuckStation Error"),
                          QObject::tr("Failed to initialize host interface. Cannot continue."), QMessageBox::Ok);
    return EXIT_FAILURE;
  }

  std::unique_ptr<MainWindow> window = std::make_unique<MainWindow>(host_interface.get());
  window->show();

  // if we're in batch mode, don't bother refreshing the game list as it won't be used
  if (!host_interface->inBatchMode())
    host_interface->refreshGameList();

  if (boot_params)
  {
    host_interface->bootSystem(*boot_params);
    boot_params.reset();
  }
  else
  {
    window->startupUpdateCheck();
  }

  int result = app.exec();

  window.reset();
  host_interface->Shutdown();
  return result;
}
