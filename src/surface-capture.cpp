/** @fileoverview Captures clean windows through Niri's native screenshot action. */
#include "capture.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>

namespace {
class CaptureDirectory final {
public:
  CaptureDirectory(QString runtime, QString uuid)
      : runtime_(std::move(runtime)),
        name_(QStringLiteral("window-%1").arg(uuid)),
        tombstone_(QStringLiteral(".delete-%1").arg(uuid)) {}

  ~CaptureDirectory() { cleanup(); }

  QString path() const { return QDir(runtime_).filePath(name_); }

private:
  void cleanup() {
    if (runtime_.isEmpty())
      return;
    QDir runtime(runtime_);
    const QString capturePath = runtime.filePath(name_);
    const QString tombstonePath = runtime.filePath(tombstone_);
    QDir(tombstonePath).removeRecursively();
    if (runtime.rename(name_, tombstone_)) {
      if (!QDir(tombstonePath).removeRecursively())
        qWarning().noquote() << "Could not remove native capture tombstone"
                             << tombstonePath;
    } else if (QFileInfo::exists(capturePath) &&
               !QDir(capturePath).removeRecursively()) {
      qWarning().noquote() << "Could not remove native capture directory"
                           << capturePath;
    }
    runtime_.clear();
  }

  QString runtime_;
  QString name_;
  QString tombstone_;
};

bool terminationRequested(const std::atomic_bool *termination) {
  return termination && termination->load(std::memory_order_acquire);
}
} // namespace

CaptureOutcome captureWindowSurface(const WindowTarget &window, QImage &image,
                                    QString &error,
                                    const std::atomic_bool *termination) {
  static const QRegularExpression decimalId(QStringLiteral("^\\d+$"));
  bool validId = false;
  window.stableId.toULongLong(&validId);
  if (!validId || !decimalId.match(window.stableId).hasMatch()) {
    error = QStringLiteral("Window has no valid Niri id");
    return CaptureOutcome::Error;
  }

  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty()) {
    error = QStringLiteral("Could not create private runtime directory");
    return CaptureOutcome::Error;
  }
  const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
  CaptureDirectory cleanup(runtime, uuid);
  const QString directory = cleanup.path();
  if (!ensurePrivateDirectory(directory)) {
    error = QStringLiteral("Could not create private window capture directory");
    return CaptureOutcome::Error;
  }
  const QString path = QDir(directory).filePath(QStringLiteral("capture.png"));

  QProcess process;
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.start(
      QStringLiteral("niri"),
      {QStringLiteral("msg"), QStringLiteral("action"),
       QStringLiteral("screenshot-window"), QStringLiteral("--id"),
       window.stableId, QStringLiteral("--path"), path,
       QStringLiteral("--write-to-disk"), QStringLiteral("true"),
       QStringLiteral("--show-pointer"), QStringLiteral("false")});
  if (!process.waitForStarted(2000)) {
    error = QStringLiteral("Could not start Niri window capture: %1")
                .arg(process.errorString());
    return CaptureOutcome::Error;
  }

  QElapsedTimer actionTimer;
  actionTimer.start();
  while (process.state() != QProcess::NotRunning &&
         actionTimer.elapsed() < 10000) {
    process.waitForFinished(20);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if (terminationRequested(termination)) {
      process.kill();
      process.waitForFinished(500);
      error.clear();
      return CaptureOutcome::Cancelled;
    }
  }
  if (process.state() != QProcess::NotRunning) {
    process.kill();
    process.waitForFinished(500);
    error = QStringLiteral("Niri window capture timed out");
    return CaptureOutcome::Error;
  }
  if (terminationRequested(termination)) {
    error.clear();
    return CaptureOutcome::Cancelled;
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (detail.isEmpty())
      detail = QStringLiteral("niri exited with code %1").arg(process.exitCode());
    error = QStringLiteral("Niri window capture failed: %1").arg(detail);
    return CaptureOutcome::Error;
  }

  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 2000) {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if (terminationRequested(termination)) {
      error.clear();
      return CaptureOutcome::Cancelled;
    }
    const QFileInfo file(path);
    QImage captured;
    if (file.exists() && file.size() > 0 && captured.load(path, "PNG")) {
      image = std::move(captured);
      error.clear();
      return CaptureOutcome::Success;
    }
    QThread::msleep(20);
  }
  error = QStringLiteral(
      "Niri window capture produced no readable PNG; window may have closed");
  return CaptureOutcome::Error;
}
