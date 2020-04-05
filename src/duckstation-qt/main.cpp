#include "common/log.h"
#include "mainwindow.h"
#include "qthostinterface.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <memory>

static void InitLogging()
{
  // set log flags
#ifdef Y_BUILD_CONFIG_DEBUG
  Log::SetConsoleOutputParams(true, nullptr, LOGLEVEL_DEBUG);
  Log::SetFilterLevel(LOGLEVEL_DEBUG);
#else
  Log::SetConsoleOutputParams(true, nullptr, LOGLEVEL_INFO);
  Log::SetFilterLevel(LOGLEVEL_INFO);
#endif
}

int main(int argc, char* argv[])
{
  InitLogging();

  QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
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
  if (!host_interface->Initialize())
  {
    host_interface->Shutdown();
    QMessageBox::critical(nullptr, QObject::tr("DuckStation Error"),
                          QObject::tr("Failed to initialize host interface. Cannot continue."), QMessageBox::Ok);
    return -1;
  }

  std::unique_ptr<MainWindow> window = std::make_unique<MainWindow>(host_interface.get());
  window->show();

  host_interface->refreshGameList();

  int result = app.exec();

  window.reset();
  host_interface->Shutdown();
  return result;
}
