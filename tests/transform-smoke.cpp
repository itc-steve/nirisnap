/** @fileoverview Tests the Niri IPC adapter and native window capture. */
#include "transform-smoke.hpp"

#include "capture.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <thread>

namespace {
bool writeExecutable(const QString &path, const QByteArray &contents) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
    return false;
  file.close();
  return QFile::setPermissions(path, QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner |
                                         QFileDevice::ExeOwner);
}

bool captureArtifactsRemain(const QString &path) {
  QDir runtime(QFileInfo(path).absolutePath());
  runtime.cdUp();
  return QFile::exists(path) || QFileInfo(path).dir().exists() ||
         !runtime.entryList({QStringLiteral("window-*"),
                             QStringLiteral(".delete-*")},
                            QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
              .isEmpty();
}
} // namespace

bool runTransformSmoke(QString &error) {
  QTemporaryDir commands;
  if (!commands.isValid()) {
    error = QStringLiteral("Could not create Niri adapter test directory");
    return false;
  }

  const QString fakeNiri = QDir(commands.path()).filePath(QStringLiteral("niri"));
  const QString fakeGrim = QDir(commands.path()).filePath(QStringLiteral("grim"));
  const QByteArray niriScript = QByteArrayLiteral(
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "printf '%s\\n' \"$*\" >> \"$NIRISNAP_TEST_NIRI_LOG\"\n"
      "case \"$*\" in\n"
      "  'msg -j focused-output')\n"
      "    if [[ -n \"${NIRISNAP_TEST_NULL_OUTPUT:-}\" ]]; then printf 'null\\n'; else\n"
      "      printf '%s\\n' '{\"name\":\"TEST-ROTATED\",\"modes\":[{\"width\":400,\"height\":600}],\"current_mode\":0,\"logical\":{\"x\":0,\"y\":0,\"width\":300,\"height\":200,\"scale\":2,\"transform\":\"90\"}}'\n"
      "    fi ;;\n"
      "  'msg -j workspaces')\n"
      "    if [[ -n \"${NIRISNAP_TEST_NO_WORKSPACE:-}\" ]]; then printf '%s\\n' '[{\"id\":\"bad\",\"output\":\"TEST-ROTATED\",\"is_active\":true}]'; else printf '%s\\n' '[{\"id\":0,\"output\":\"TEST-ROTATED\",\"is_active\":true}]'; fi ;;\n"
      "  'msg -j windows')\n"
      "    printf '%s\\n' '[{\"id\":18446744073709551615,\"workspace_id\":0,\"title\":\"floating\",\"layout\":{\"tile_pos_in_workspace_view\":[10.4,20.4],\"window_offset_in_tile\":[2,2],\"window_size\":[100,80]}}]' ;;\n"
      "  'msg -j pick-window')\n"
      "    if [[ -n \"${NIRISNAP_TEST_BLOCK_PICK:-}\" ]]; then\n"
      "      : > \"$NIRISNAP_TEST_PICK_MARKER\"\n"
      "      while [[ ! -f \"$NIRISNAP_TEST_PICK_RESPONSE\" ]]; do sleep 0.02; done\n"
      "      cat -- \"$NIRISNAP_TEST_PICK_RESPONSE\"\n"
      "      exit 0\n"
      "    fi\n"
      "    case \"${NIRISNAP_TEST_PICK_RESULT:-success}\" in\n"
      "      success) printf '%s\\n' '{\"id\":42,\"title\":\"picked\",\"app_id\":\"test.app\"}' ;;\n"
      "      cancel) printf 'null\\n' ;;\n"
      "      malformed) printf '{' ;;\n"
      "      missing) printf '%s\\n' '{\"title\":\"missing\"}' ;;\n"
      "      nonzero) printf 'picker failed\\n' >&2; exit 3 ;;\n"
      "    esac ;;\n"
      "  'msg action screenshot-window '*)\n"
      "    path=''; id=''; disk=''; pointer=''\n"
      "    while (($#)); do case \"$1\" in --path) path=$2; shift 2;; --id) id=$2; shift 2;; --write-to-disk) disk=$2; shift 2;; --show-pointer) pointer=$2; shift 2;; *) shift;; esac; done\n"
      "    [[ $path = /* && $id =~ ^[0-9]+$ && $disk == true && $pointer == false ]] || exit 2\n"
      "    printf '%s' \"$path\" > \"$NIRISNAP_TEST_CAPTURE_PATH\"\n"
      "    case \"${NIRISNAP_TEST_WINDOW_RESULT:-async}\" in\n"
      "      async) (sleep 0.05; cp -- \"$NIRISNAP_TEST_PNG\" \"$path\") >/dev/null 2>&1 & ;;\n"
      "      delayed) (sleep 0.30; cp -- \"$NIRISNAP_TEST_PNG\" \"$path\") >/dev/null 2>&1 & ;;\n"
      "      none) : ;;\n"
      "    esac ;;\n"
      "  *) exit 64 ;;\n"
      "esac\n");
  const QByteArray grimScript = QByteArrayLiteral(
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "printf '<%s>\\n' \"$@\" > \"$NIRISNAP_TEST_GRIM_LOG\"\n"
      "if [[ -n \"${NIRISNAP_TEST_GRIM_GARBAGE:-}\" ]]; then printf 'not-a-ppm'; else cat -- \"$NIRISNAP_TEST_PPM\"; fi\n");
  if (!writeExecutable(fakeNiri, niriScript) ||
      !writeExecutable(fakeGrim, grimScript)) {
    error = QStringLiteral("Could not create Niri adapter test commands");
    return false;
  }

  QImage source(600, 400, QImage::Format_RGB32);
  source.fill(QColor(QStringLiteral("#123456")));
  QImage png(3, 2, QImage::Format_RGB32);
  png.fill(QColor(QStringLiteral("#abcdef")));
  const QString ppmPath = QDir(commands.path()).filePath(QStringLiteral("source.ppm"));
  const QString pngPath = QDir(commands.path()).filePath(QStringLiteral("window.png"));
  const QString niriLogPath = QDir(commands.path()).filePath(QStringLiteral("niri.log"));
  const QString grimLogPath = QDir(commands.path()).filePath(QStringLiteral("grim.log"));
  const QString capturePath = QDir(commands.path()).filePath(QStringLiteral("capture-path"));
  if (!source.save(ppmPath, "PPM") || !png.save(pngPath, "PNG")) {
    error = QStringLiteral("Could not create Niri adapter test images");
    return false;
  }

  const QByteArray originalPath = qgetenv("PATH");
  qputenv("PATH", commands.path().toUtf8() + ':' + originalPath);
  qputenv("NIRISNAP_TEST_PPM", ppmPath.toUtf8());
  qputenv("NIRISNAP_TEST_PNG", pngPath.toUtf8());
  qputenv("NIRISNAP_TEST_NIRI_LOG", niriLogPath.toUtf8());
  qputenv("NIRISNAP_TEST_GRIM_LOG", grimLogPath.toUtf8());
  qputenv("NIRISNAP_TEST_CAPTURE_PATH", capturePath.toUtf8());
  const auto restore = [&] {
    qputenv("PATH", originalPath);
    for (const char *name : {"NIRISNAP_TEST_PPM", "NIRISNAP_TEST_PNG",
                             "NIRISNAP_TEST_NIRI_LOG", "NIRISNAP_TEST_GRIM_LOG",
                             "NIRISNAP_TEST_CAPTURE_PATH",
                             "NIRISNAP_TEST_NULL_OUTPUT",
                             "NIRISNAP_TEST_NO_WORKSPACE",
                             "NIRISNAP_TEST_PICK_RESULT",
                             "NIRISNAP_TEST_BLOCK_PICK",
                             "NIRISNAP_TEST_PICK_MARKER",
                             "NIRISNAP_TEST_PICK_RESPONSE",
                             "NIRISNAP_TEST_WINDOW_RESULT",
                             "NIRISNAP_TEST_GRIM_GARBAGE"})
      qunsetenv(name);
  };
  const auto fail = [&](const QString &message) {
    error = message;
    restore();
    return false;
  };

  CaptureData capture;
  if (!captureFocusedMonitor(capture, true, error) ||
      capture.monitor.geometry != QRect(0, 0, 300, 200) ||
      capture.monitor.pixelSize != QSize(600, 400) ||
      !capture.monitor.workspaceId.has_value() ||
      *capture.monitor.workspaceId != 0 || capture.previewSize != QSize(300, 200) ||
      capture.windows.size() != 1 ||
      capture.windows.first().rect != QRect(12, 22, 100, 80) ||
      capture.windows.first().stableId != QStringLiteral("18446744073709551615"))
    return fail(error.isEmpty()
                    ? QStringLiteral("Transformed output or workspace-zero mapping was incorrect")
                    : error);

  QFile grimLog(grimLogPath);
  if (!grimLog.open(QIODevice::ReadOnly) ||
      grimLog.readAll() != QByteArrayLiteral(
                               "<-t>\n<ppm>\n<-s>\n<2>\n<-g>\n<0,0 300x200>\n<->\n"))
    return fail(QStringLiteral("grim argv did not preserve transformed logical geometry and scale"));

  qputenv("NIRISNAP_TEST_NO_WORKSPACE", QByteArrayLiteral("1"));
  CaptureData noWorkspace;
  if (!captureFocusedMonitor(noWorkspace, true, error) ||
      noWorkspace.monitor.workspaceId.has_value() || !noWorkspace.windows.isEmpty())
    return fail(QStringLiteral("Missing workspace was not represented as nullopt"));
  qunsetenv("NIRISNAP_TEST_NO_WORKSPACE");

  QFile::remove(niriLogPath);
  CaptureData withoutWindows;
  if (!captureFocusedMonitor(withoutWindows, false, error) ||
      !withoutWindows.windows.isEmpty())
    return fail(error.isEmpty()
                    ? QStringLiteral("Capture without Niri window discovery failed")
                    : error);
  QFile niriLog(niriLogPath);
  if (!niriLog.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("Could not read fake Niri command log"));
  const QByteArray calls = niriLog.readAll();
  if (calls.contains("workspaces") || calls.contains("windows"))
    return fail(QStringLiteral("Window-disabled capture still queried Niri windows"));

  qputenv("NIRISNAP_TEST_NULL_OUTPUT", QByteArrayLiteral("1"));
  MonitorInfo invalidMonitor;
  QString invalidError;
  if (probeFocusedMonitor(invalidMonitor, invalidError) || invalidError.isEmpty())
    return fail(QStringLiteral("Null focused output did not fail cleanly"));
  qunsetenv("NIRISNAP_TEST_NULL_OUTPUT");

  WindowTarget picked;
  if (pickWindowTarget(picked, error) != CaptureOutcome::Success ||
      picked.stableId != QStringLiteral("42") ||
      picked.title != QStringLiteral("picked"))
    return fail(error.isEmpty() ? QStringLiteral("Niri picker result was incorrect")
                                : error);

  qputenv("NIRISNAP_TEST_PICK_RESULT", QByteArrayLiteral("cancel"));
  QString cancelError;
  if (pickWindowTarget(picked, cancelError) != CaptureOutcome::Cancelled ||
      !cancelError.isEmpty())
    return fail(QStringLiteral("Niri picker cancellation was not distinct from error"));

  for (const QByteArray &mode : {QByteArrayLiteral("malformed"),
                                QByteArrayLiteral("missing"),
                                QByteArrayLiteral("nonzero")}) {
    qputenv("NIRISNAP_TEST_PICK_RESULT", mode);
    QString pickError;
    if (pickWindowTarget(picked, pickError) != CaptureOutcome::Error ||
        pickError.isEmpty() || pickError.contains(QStringLiteral("cancelled"),
                                                  Qt::CaseInsensitive))
      return fail(QStringLiteral("Niri picker error was reported as cancellation"));
  }
  qunsetenv("NIRISNAP_TEST_PICK_RESULT");

  QFile::remove(niriLogPath);
  const QString pickMarker =
      QDir(commands.path()).filePath(QStringLiteral("pick-attached"));
  const QString pickResponse =
      QDir(commands.path()).filePath(QStringLiteral("pick-response"));
  qputenv("NIRISNAP_TEST_BLOCK_PICK", QByteArrayLiteral("1"));
  qputenv("NIRISNAP_TEST_PICK_MARKER", pickMarker.toUtf8());
  qputenv("NIRISNAP_TEST_PICK_RESPONSE", pickResponse.toUtf8());
  const QString childRuntime =
      QDir(commands.path()).filePath(QStringLiteral("child-runtime"));
  QDir().mkpath(childRuntime);
  QFile::setPermissions(childRuntime, QFileDevice::ReadOwner |
                                          QFileDevice::WriteOwner |
                                          QFileDevice::ExeOwner);
  QProcess child;
  QProcessEnvironment childEnvironment = QProcessEnvironment::systemEnvironment();
  childEnvironment.insert(QStringLiteral("XDG_RUNTIME_DIR"), childRuntime);
  child.setProcessEnvironment(childEnvironment);
  child.setProcessChannelMode(QProcess::SeparateChannels);
  child.start(QDir(QCoreApplication::applicationDirPath())
                  .filePath(QStringLiteral("nirisnap")),
              {QStringLiteral("--capture-window")});
  if (!child.waitForStarted(5000))
    return fail(QStringLiteral("Could not start SIGTERM picker smoke child"));
  QElapsedTimer attachTimer;
  attachTimer.start();
  while (!QFile::exists(pickMarker) && attachTimer.elapsed() < 10000)
    QThread::msleep(10);
  if (!QFile::exists(pickMarker)) {
    const QByteArray out = child.readAllStandardOutput();
    const QByteArray err = child.readAllStandardError();
    const auto state = child.state();
    const int code = child.exitCode();
    const QString errorString = child.errorString();
    child.kill();
    child.waitForFinished(500);
    return fail(QStringLiteral("SIGTERM picker smoke did not attach (state=%1 "
                               "exit=%2 err=%3)\nstdout: %4\nstderr: %5")
                    .arg(int(state))
                    .arg(code)
                    .arg(errorString, QString::fromLocal8Bit(out),
                         QString::fromLocal8Bit(err)));
  }
  child.terminate();
  QThread::msleep(100);
  if (child.state() != QProcess::Running) {
    return fail(QStringLiteral("SIGTERM detached the active Niri picker"));
  }
  QFile response(pickResponse);
  const QByteArray pickedAfterSignal = QByteArrayLiteral(
      "{\"id\":77,\"title\":\"discard after signal\"}\n");
  if (!response.open(QIODevice::WriteOnly) ||
      response.write(pickedAfterSignal) != pickedAfterSignal.size()) {
    child.kill();
    child.waitForFinished(500);
    return fail(QStringLiteral("Could not release SIGTERM picker smoke"));
  }
  response.close();
  if (!child.waitForFinished(3000) || child.exitCode() != 0)
    return fail(QStringLiteral("SIGTERM picker did not exit cleanly after resolution"));
  QFile pickerCalls(niriLogPath);
  if (!pickerCalls.open(QIODevice::ReadOnly) ||
      pickerCalls.readAll().contains("screenshot-window"))
    return fail(QStringLiteral("Window selected after SIGTERM was not discarded"));
  qunsetenv("NIRISNAP_TEST_BLOCK_PICK");
  qunsetenv("NIRISNAP_TEST_PICK_MARKER");
  qunsetenv("NIRISNAP_TEST_PICK_RESPONSE");

  QFile::remove(capturePath);
  qputenv("NIRISNAP_TEST_WINDOW_RESULT", QByteArrayLiteral("delayed"));
  QProcess nativeChild;
  QProcessEnvironment nativeEnvironment =
      QProcessEnvironment::systemEnvironment();
  nativeEnvironment.insert(QStringLiteral("XDG_RUNTIME_DIR"), childRuntime);
  nativeChild.setProcessEnvironment(nativeEnvironment);
  nativeChild.setProcessChannelMode(QProcess::SeparateChannels);
  nativeChild.start(QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("nirisnap")),
                    {QStringLiteral("--capture-window")});
  if (!nativeChild.waitForStarted(5000))
    return fail(QStringLiteral("Could not start SIGTERM native capture child"));
  QElapsedTimer nativeTimer;
  nativeTimer.start();
  while (!QFile::exists(capturePath) && nativeTimer.elapsed() < 10000)
    QThread::msleep(10);
  if (!QFile::exists(capturePath)) {
    nativeChild.kill();
    nativeChild.waitForFinished(500);
    return fail(QStringLiteral("SIGTERM native capture did not start"));
  }
  nativeChild.terminate();
  if (!nativeChild.waitForFinished(3000) || nativeChild.exitCode() != 0)
    return fail(QStringLiteral("SIGTERM native capture did not exit cleanly"));
  QFile nativeTargetFile(capturePath);
  if (!nativeTargetFile.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("Could not read SIGTERM native capture path"));
  const QString signalTarget = QString::fromUtf8(nativeTargetFile.readAll());
  QThread::msleep(400);
  if (captureArtifactsRemain(signalTarget))
    return fail(QStringLiteral("SIGTERM native capture left private artifacts"));
  qunsetenv("NIRISNAP_TEST_WINDOW_RESULT");

  QImage windowImage;
  if (captureWindowSurface({{}, QStringLiteral("42"), QStringLiteral("picked")},
                           windowImage, error) != CaptureOutcome::Success ||
      windowImage.size() != QSize(3, 2))
    return fail(error.isEmpty()
                    ? QStringLiteral("Async Niri window screenshot failed")
                    : error);
  QFile capturedPathFile(capturePath);
  if (!capturedPathFile.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("Fake Niri did not record capture path"));
  const QString nativePath = QString::fromUtf8(capturedPathFile.readAll());
  if (captureArtifactsRemain(nativePath))
    return fail(QStringLiteral("Native window screenshot directory was not cleaned up"));

  QFile::remove(capturePath);
  qputenv("NIRISNAP_TEST_WINDOW_RESULT", QByteArrayLiteral("delayed"));
  std::atomic_bool termination{false};
  std::thread cancel([&] {
    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(capturePath) && timer.elapsed() < 1000)
      QThread::msleep(5);
    termination.store(true, std::memory_order_release);
  });
  QImage cancelledImage;
  QString terminatedError;
  const CaptureOutcome terminated = captureWindowSurface(
      {{}, QStringLiteral("43"), QStringLiteral("delayed")}, cancelledImage,
      terminatedError, &termination);
  cancel.join();
  if (terminated != CaptureOutcome::Cancelled || !terminatedError.isEmpty())
    return fail(QStringLiteral("Terminated native capture did not cancel cleanly"));
  QFile terminatedPathFile(capturePath);
  if (!terminatedPathFile.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("Terminated fake capture did not record path"));
  const QString terminatedPath = QString::fromUtf8(terminatedPathFile.readAll());
  QThread::msleep(400);
  if (captureArtifactsRemain(terminatedPath))
    return fail(QStringLiteral("Delayed native write escaped tombstone cleanup"));

  qputenv("NIRISNAP_TEST_WINDOW_RESULT", QByteArrayLiteral("none"));
  QImage missingImage;
  QString missingError;
  if (captureWindowSurface({{}, QStringLiteral("999"), QStringLiteral("gone")},
                           missingImage, missingError) != CaptureOutcome::Error ||
      missingError.isEmpty())
    return fail(QStringLiteral("Exit-zero window capture without file was not an error"));

  qputenv("NIRISNAP_TEST_GRIM_GARBAGE", QByteArrayLiteral("1"));
  CaptureData undecodable;
  QString decodeError;
  if (captureFocusedMonitor(undecodable, false, decodeError) || decodeError.isEmpty())
    return fail(QStringLiteral("Undecodable grim output did not report an error"));

  restore();
  return true;
}
