#include "common/crash_handler.h"
#include "mainwindow.h"
#include "qthostinterface.h"
#include "qtutils.h"
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>

static bool ParseCommandLineParameters(QApplication& app, QtHostInterface* host_interface,
                                       std::unique_ptr<SystemBootParameters>* boot_params)
{
  const QStringList args(app.arguments());
  std::vector<std::string> converted_args;
  std::vector<char*> converted_argv;
  converted_args.reserve(args.size());
  converted_argv.reserve(args.size());

  for (const QString& arg : args)
    converted_args.push_back(arg.toStdString());

  for (std::string& arg : converted_args)
    converted_argv.push_back(arg.data());

  return host_interface->ParseCommandLineParameters(args.size(), converted_argv.data(), boot_params);
}

static void SignalHandler(int signal)
{
  // First try the normal (graceful) shutdown/exit.
  static bool graceful_shutdown_attempted = false;
  if (!graceful_shutdown_attempted)
  {
    std::fprintf(stderr, "Received CTRL+C, attempting graceful shutdown. Press CTRL+C again to force.\n");
    graceful_shutdown_attempted = true;
    QtHostInterface::GetInstance()->requestExit();
    return;
  }

  std::signal(signal, SIG_DFL);

  // MacOS is missing std::quick_exit() despite it being C++11...
#ifndef __APPLE__
  std::quick_exit(1);
#else
  _Exit(1);
#endif
}

static void HookSignals()
{
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
}

int main(int argc, char* argv[])
{
  CrashHandler::Install();

  // Register any standard types we need elsewhere
  qRegisterMetaType<std::optional<bool>>();
  qRegisterMetaType<std::function<void()>>();

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif

  QApplication app(argc, argv);

#ifdef _WIN32
  // Use Segoe UI on Windows rather than MS Shell Dlg 2, courtesy of Dolphin.
  // Can be removed once switched to Qt 6.
  QApplication::setFont(QApplication::font("QMenu"));
#endif

  std::unique_ptr<QtHostInterface> host_interface = std::make_unique<QtHostInterface>();
  std::unique_ptr<SystemBootParameters> boot_params;
  if (!ParseCommandLineParameters(app, host_interface.get(), &boot_params))
    return EXIT_FAILURE;

  MainWindow* window = new MainWindow(host_interface.get());

  if (!host_interface->Initialize())
  {
    host_interface->Shutdown();
    QMessageBox::critical(nullptr, QObject::tr("DuckStation Error"),
                          QObject::tr("Failed to initialize host interface. Cannot continue."), QMessageBox::Ok);
    return EXIT_FAILURE;
  }

  window->initializeAndShow();
  HookSignals();

  // if we're in batch mode, don't bother refreshing the game list as it won't be used
  if (!host_interface->inBatchMode())
    host_interface->refreshGameList();

  if (boot_params)
  {
    host_interface->bootSystem(std::move(boot_params));
  }
  else
  {
    window->startupUpdateCheck();
  }

  int result = app.exec();

  host_interface->Shutdown();
  return result;
}

#ifdef _WIN32

// Apparently Qt6 got rid of this?
#include "common/windows_headers.h"
#include <shellapi.h>

/*
  WinMain() - Initializes Windows and calls user's startup function main().
  NOTE: WinMain() won't be called if the application was linked as a "console"
  application.
*/

// Convert a wchar_t to char string, equivalent to QString::toLocal8Bit()
// when passed CP_ACP.
static inline char* wideToMulti(unsigned int codePage, const wchar_t* aw)
{
  const int required = WideCharToMultiByte(codePage, 0, aw, -1, nullptr, 0, nullptr, nullptr);
  char* result = new char[required];
  WideCharToMultiByte(codePage, 0, aw, -1, result, required, nullptr, nullptr);
  return result;
}

extern "C" int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR /*cmdParamarg*/, int /* cmdShow */)
{
  int argc = 0;
  wchar_t** argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argvW == nullptr)
    return -1;
  char** argv = new char* [argc + 1];
  for (int i = 0; i != argc; ++i)
    argv[i] = wideToMulti(CP_ACP, argvW[i]);
  argv[argc] = nullptr;
  LocalFree(argvW);
  const int exitCode = main(argc, argv);
  for (int i = 0; (i != argc) && (argv[i] != nullptr); ++i)
    delete[] argv[i];
  delete[] argv;
  return exitCode;
}

#endif