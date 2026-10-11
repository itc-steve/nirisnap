/** @fileoverview Exercises capture editor behavior without a live compositor.
 */
#include "capture.hpp"
#include "cli-path.hpp"
#include "clipboard-smoke.hpp"
#include "editor.hpp"
#include "instance-lock-smoke.hpp"
#include "pin-layout-smoke.hpp"
#include "pin-lifecycle-smoke.hpp"
#include "transform-smoke.hpp"
#include "eyedropper.hpp"

#include <QApplication>
#include <QBuffer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QClipboard>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QPainter>
#include <QTemporaryDir>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest/QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <fcntl.h>
#include <numbers>
#include <sys/resource.h>
#include <unistd.h>

namespace {
/**
 * The current output is a memory-only composite until an explicit save/copy.
 */
template <typename Editor>
QImage flushedSnapshot(Editor &editor, const QString &) {
  return editor.renderCurrentOutput();
}
/** Cached select-phase painting preserves bright and dimmed capture pixels. */
bool runBackdropCacheRenderingCheck(QString &error) {
  CaptureData capture;
  capture.monitor.geometry = QRect(0, 0, 800, 600);
  capture.monitor.pixelSize = QSize(800, 600);
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#6480a0")));
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  QApplication::processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(150, 120));
  QTest::mouseMove(&editor, QPoint(600, 430), 20);
  QApplication::processEvents();
  const QImage ui = editor.grab().toImage();

  for (const QPoint inside : {QPoint(300, 250), QPoint(500, 400)}) {
    if (ui.pixelColor(inside) != capture.source.pixelColor(inside)) {
      error = QStringLiteral("Cached selection backdrop changed source pixels");
      return false;
    }
  }
  for (const QPoint outside : {QPoint(40, 200), QPoint(720, 200)}) {
    const QColor source = capture.source.pixelColor(outside);
    const QColor dimmed = ui.pixelColor(outside);
    const auto matches = [](int actual, int expected) {
      return std::abs(actual - expected) <= 2;
    };
    if (!matches(dimmed.red(), source.red() * (255 - 143) / 255) ||
        !matches(dimmed.green(), source.green() * (255 - 143) / 255) ||
        !matches(dimmed.blue(), source.blue() * (255 - 143) / 255)) {
      error = QStringLiteral("Cached selection backdrop changed dimming");
      return false;
    }
  }
  return true;
}

/** Preview fits the entire export frame; radius and shadow scale with it. */
bool runBackdropParityChecks(QString &error) {
  const QSize widget(800, 600);
  const QRectF available(30, 68, 740, 474);
  for (const QSize logical :
       {QSize(320, 200), QSize(321, 201), QSize(960, 640), QSize(961, 641)}) {
    for (const qreal ratio : {1.0, 1.25, 1.5, 2.0}) {
      CaptureData capture;
      capture.previewSize = logical;
      capture.monitor.scale = ratio;
      capture.source = QImage(qRound(logical.width() * ratio),
                              qRound(logical.height() * ratio),
                              QImage::Format_ARGB32_Premultiplied);
      capture.source.fill(Qt::white);
      CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
      editor.setSuppressSnapshots(true);
      editor.resize(widget);
      editor.show();
      QTest::keyClick(&editor, Qt::Key_X); // No handles or tool-size tooltip.
      QTest::mouseMove(&editor, QPoint(5, 5));
      for (int style = 0; style < 5; ++style) {
        if (style)
          QTest::keyClick(&editor, Qt::Key_B);
        QApplication::processEvents();
        const QImage exported = editor.renderCurrentOutput();
        const int margin = style ? 64 : 0;
        if (exported.size() !=
            QSize(qRound(logical.width() * ratio) + 2 * qRound(margin * ratio),
                  qRound(logical.height() * ratio) +
                      2 * qRound(margin * ratio))) {
          error = QStringLiteral("Backdrop changed native export dimensions");
          return false;
        }
        const QSizeF framed(logical.width() + 2 * margin,
                            logical.height() + 2 * margin);
        const qreal scale =
            std::min<qreal>({1.0, available.width() / framed.width(),
                             available.height() / framed.height()});
        const QRectF frame(available.center() -
                               QPointF(framed.width(), framed.height()) *
                                   scale / 2,
                           framed * scale);
        const QRectF image = frame.adjusted(margin * scale, margin * scale,
                                            -margin * scale, -margin * scale);
        QImage expected(widget, QImage::Format_ARGB32_Premultiplied);
        expected.fill(QColor(0, 0, 0, 160));
        {
          QPainter painter(&expected);
          painter.setRenderHint(QPainter::SmoothPixmapTransform);
          painter.drawImage(frame, exported);
        }
        const QImage preview = editor.grab().toImage();
        if (editor.renderCurrentOutput() != exported) {
          error = QStringLiteral("Backdrop preview changed exported pixels");
          return false;
        }
        const auto matches = [&](const QPointF &position, int tolerance,
                                 const QString &part) {
          const QPoint point(qFloor(position.x()), qFloor(position.y()));
          const QColor actual = preview.pixelColor(point);
          const QColor wanted = expected.pixelColor(point);
          if (std::abs(actual.red() - wanted.red()) <= tolerance &&
              std::abs(actual.green() - wanted.green()) <= tolerance &&
              std::abs(actual.blue() - wanted.blue()) <= tolerance &&
              actual.alpha() == wanted.alpha())
            return true;
          error =
              QStringLiteral("Backdrop preview/export %1 mismatch: style %2, "
                             "DPI %3, fit %4, at %5,%6: %7 != %8")
                  .arg(part)
                  .arg(style)
                  .arg(ratio)
                  .arg(scale)
                  .arg(point.x())
                  .arg(point.y())
                  .arg(actual.name(QColor::HexArgb),
                       wanted.name(QColor::HexArgb));
          return false;
        };
        if (!matches(image.center(), 0, QStringLiteral("source")))
          return false;
        if (!style)
          continue;
        // Interior/exterior framing, independent of image-edge antialiasing.
        for (const QPointF point :
             {QPointF(frame.left() + 4, frame.center().y()),
              QPointF(frame.right() - 4, frame.center().y()),
              QPointF(frame.left() + 24, frame.top() + 4),
              QPointF(frame.center().x(), frame.bottom() - 4),
              QPointF(frame.left() - 2, frame.center().y()),
              QPointF(frame.right() + 2, frame.center().y())}) {
          if (!matches(point, 3, QStringLiteral("margin")))
            return false;
        }
        // Compare curve geometry, not AA coverage after native downsampling.
        const auto firstWhite = [&](const QImage &raster, int y) {
          for (int x = qCeil(image.left());
               x < qCeil(image.left() + 28 * scale); ++x) {
            const QColor pixel = raster.pixelColor(x, y);
            if (pixel.red() >= 253 && pixel.green() >= 253 &&
                pixel.blue() >= 253)
              return x;
          }
          return -1;
        };
        for (const qreal depth : {2.0, 4.0, 8.0, 13.0, 18.0}) {
          const int y = qFloor(image.top() + depth * scale);
          const int actual = firstWhite(preview, y);
          const int wanted = firstWhite(expected, y);
          if (actual < 0 || wanted < 0 || std::abs(actual - wanted) > 1) {
            error =
                QStringLiteral("Backdrop radius boundary mismatch: style %1, "
                               "DPI %2, fit %3, y %4: x %5 != %6")
                    .arg(style)
                    .arg(ratio)
                    .arg(scale)
                    .arg(y)
                    .arg(actual)
                    .arg(wanted);
            return false;
          }
        }
        for (const qreal offset : {4.0, 8.0, 14.0, 22.0, 30.0, 40.0}) {
          for (const QPointF point :
               {QPointF(image.center().x(), image.bottom() + offset * scale),
                QPointF(image.left() + 32 * scale,
                        image.top() - offset * scale),
                QPointF(image.left() - offset * scale, image.center().y())}) {
            if (!matches(point, 7, QStringLiteral("shadow")))
              return false;
          }
        }
      }
    }
  }
  return true;
}

/** Cutout joins exact native pixels and Escape closes every editor path. */
bool runCutoutAndEscapeChecks(QString &error) {
  CaptureData capture;
  capture.source = QImage(600, 400, QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < 400; ++y)
    for (int x = 0; x < 600; ++x)
      capture.source.setPixelColor(x, y,
                                   QColor(x % 256, y % 256, (x + y) % 256));
  capture.previewSize = QSize(480, 320);
  capture.monitor.scale = 1.25;
  capture.monitor.geometry = QRect(0, 0, 480, 320);
  capture.monitor.pixelSize = capture.source.size();
  const auto drag = [](CaptureEditor &editor, QPoint start, QPoint end) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(&editor, end);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, end);
    QApplication::processEvents();
  };
  for (bool horizontal : {true, false}) {
    for (bool solid : {true, false}) {
      CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
      editor.setSuppressSnapshots(true);
      editor.resize(800, 600);
      editor.show();
      QApplication::processEvents();
      // Full capture displays at (160,145), with 1.25 native pixels per unit.
      QTest::keyClick(&editor, Qt::Key_D);
      if (solid)
        QTest::keyClick(&editor,
                        Qt::Key_D); // Redaction crosses retained/cut parts.
      drag(editor, QPoint(192, 177), QPoint(352, 297));
      QTest::keyClick(&editor, Qt::Key_R);
      drag(editor, QPoint(400, 337), QPoint(480, 385));
      const QImage annotated = editor.renderCurrentOutput();
      QTest::keyClick(&editor, Qt::Key_B);
      const QImage decorated = editor.renderCurrentOutput();
      if (horizontal)
        QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(738, 53)); // Toolbar sits above full frame.
      else
        QTest::keyClick(&editor, Qt::Key_X);
      // Click-only and full/edge strips must not mutate image or history.
      for (const auto &points :
           {std::pair{QPoint(200, 200), QPoint(200, 200)},
            std::pair{QPoint(160, 145), QPoint(160, 465)},
            std::pair{QPoint(160, 145), QPoint(640, 145)},
            std::pair{QPoint(200, 145), QPoint(200, 225)}}) {
        drag(editor, points.first, points.second);
        if (editor.renderCurrentOutput() != decorated) {
          error = QStringLiteral("Invalid cutout changed pixels");
          return false;
        }
      }
      const QPoint start = horizontal ? QPoint(300, 225) : QPoint(256, 300);
      const QPoint end = horizontal ? QPoint(300, 305) : QPoint(352, 300);
      const QPoint previewPoint =
          horizontal ? QPoint(620, 260) : QPoint(300, 440);
      const QColor beforePreview =
          editor.grab().toImage().pixelColor(previewPoint);
      QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, end);
      QTest::mouseMove(&editor,
                       start); // Reverse drag works, preview spans image.
      QApplication::processEvents();
      const QColor preview = editor.grab().toImage().pixelColor(previewPoint);
      if (preview == beforePreview) {
        error = QStringLiteral("Cutout did not preview full strip");
        return false;
      }
      QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, start);
      QApplication::processEvents();
      QImage joined(horizontal ? QSize(600, 300) : QSize(480, 400),
                    QImage::Format_ARGB32_Premultiplied);
      for (int y = 0; y < joined.height(); ++y)
        for (int x = 0; x < joined.width(); ++x)
          joined.setPixel(
              x, y,
              annotated.pixel(x + (!horizontal && x >= 120 ? 120 : 0),
                              y + (horizontal && y >= 100 ? 100 : 0)));
      CaptureData expectedCapture = capture;
      expectedCapture.source = joined;
      expectedCapture.previewSize =
          horizontal ? QSize(480, 240) : QSize(384, 320);
      expectedCapture.monitor.scale = 1.0;
      const QImage expected = renderCapture(
          expectedCapture, QRectF(QPointF(), expectedCapture.previewSize), {},
          BackgroundStyle::Aurora);
      if (editor.renderCurrentOutput() != expected ||
          editor.captureData().source != joined) {
        error = QStringLiteral("Cutout lost native pixels, annotations, "
                               "redactions, or backdrop separation");
        return false;
      }
      QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
      if (editor.renderCurrentOutput() != decorated ||
          editor.captureData().source != capture.source) {
        error = QStringLiteral("Cutout undo lost source or layers");
        return false;
      }
      QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
      if (editor.renderCurrentOutput() != expected) {
        error = QStringLiteral("Cutout redo changed output");
        return false;
      }
    }
  }
  // Fractional source origins and odd pixel crop extents must not be resampled.
  for (bool horizontal : {true, false}) {
    CaptureEditor editor(capture);
    editor.setSuppressSnapshots(true);
    editor.resize(800, 600);
    editor.show();
    QApplication::processEvents();
    drag(editor, QPoint(101, 103), QPoint(479, 317));
    const QImage before = editor.renderCurrentOutput();
    CaptureData native = capture;
    native.monitor.scale = 1.0;
    const QImage cropped = renderCapture(native, QRectF(101, 103, 378, 214), {},
                                         BackgroundStyle::None);
    if (cropped.size() != QSize(473, 269)) {
      error = QStringLiteral("Fractional cutout fixture has wrong native crop");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_X);
    drag(editor, horizontal ? QPoint(400, 268) : QPoint(291, 300),
         horizontal ? QPoint(400, 318) : QPoint(341, 300));
    QImage joined(horizontal ? QSize(473, 205) : QSize(410, 269),
                  cropped.format());
    for (int y = 0; y < joined.height(); ++y)
      for (int x = 0; x < joined.width(); ++x)
        joined.setPixel(x, y,
                        cropped.pixel(x + (!horizontal && x >= 100 ? 63 : 0),
                                      y + (horizontal && y >= 87 ? 64 : 0)));
    if (editor.renderCurrentOutput() != joined) {
      error = QStringLiteral(
          "Fractional crop cutout resampled or misplaced native pixels");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_R);
    drag(editor, QPoint(400, 270), QPoint(440, 300));
    const QImage annotated = editor.renderCurrentOutput();
    if (annotated == joined) {
      error = QStringLiteral("Could not annotate after cutout");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    if (editor.renderCurrentOutput() != joined) {
      error = QStringLiteral("Undo after cutout lost joined image");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    if (editor.renderCurrentOutput() != before) {
      error = QStringLiteral("Fractional cutout undo lost original crop");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
    QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
    if (editor.renderCurrentOutput() != annotated) {
      error = QStringLiteral("Fractional cutout redo lost new annotation");
      return false;
    }
  }
  // Covered half-open native bands include both fractional boundary pixels.
  for (const qreal ratio : {1.0, 1.25, 2.0}) {
    CaptureData fitted = capture;
    fitted.source = QImage(1200, 800, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 800; ++y)
      for (int x = 0; x < 1200; ++x)
        fitted.source.setPixelColor(
            x, y, QColor(x % 256, y % 256, (x + y) % 256, 128));
    fitted.previewSize = QSize(qRound(1200 / ratio), qRound(800 / ratio));
    fitted.monitor.scale = ratio;
    for (bool horizontal : {true, false}) {
      for (int fixture = 0; fixture < 10; ++fixture) {
        const bool reversed = fixture % 2;
        const int background = fixture / 2;
        CaptureEditor editor(fitted, CaptureEditor::CaptureMode::Fullscreen);
        editor.setSuppressSnapshots(true);
        editor.resize(800, 600);
        editor.show();
        QApplication::processEvents();
        QTest::keyClick(&editor, Qt::Key_X);
        for (int style = 0; style < background; ++style)
          QTest::keyClick(&editor, Qt::Key_B);
        const int margin = background ? 64 : 0;
        const qreal scale = std::min<qreal>(
            {1.0, 740.0 / (fitted.previewSize.width() + 2 * margin),
             474.0 / (fitted.previewSize.height() + 2 * margin)});
        const QSizeF shown = QSizeF(fitted.previewSize) * scale;
        const QPointF origin(400 - shown.width() / 2, 305 - shown.height() / 2);
        const qreal nativeScale = scale / ratio;
        const QPointF first =
            origin + nativeScale * (horizontal ? QPointF(400, 90.001)
                                               : QPointF(90.001, 400));
        const QPointF last =
            origin + nativeScale * (horizontal ? QPointF(400, 155.999)
                                               : QPointF(155.999, 400));
        const auto send = [&editor](QEvent::Type type, const QPointF &point) {
          QMouseEvent event(type, point, point,
                            type == QEvent::MouseMove ? Qt::NoButton
                                                      : Qt::LeftButton,
                            type == QEvent::MouseButtonRelease ? Qt::NoButton
                                                               : Qt::LeftButton,
                            Qt::NoModifier);
          QApplication::sendEvent(&editor, &event);
        };
        const QPoint probe =
            (origin +
             nativeScale * (horizontal ? QPointF(20, 123) : QPointF(123, 780)))
                .toPoint();
        const QColor beforePreview = editor.grab().toImage().pixelColor(probe);
        send(QEvent::MouseButtonPress, reversed ? last : first);
        send(QEvent::MouseMove, reversed ? first : last);
        if (editor.grab().toImage().pixelColor(probe) == beforePreview) {
          error = QStringLiteral("Fitted backdrop cutout preview missed strip");
          return false;
        }
        send(QEvent::MouseButtonRelease, reversed ? first : last);
        const QImage joined = editor.captureData().source;
        if (joined.size() !=
            (horizontal ? QSize(1200, 734) : QSize(1134, 800))) {
          error = QStringLiteral(
              "Fractional cutout omitted covered boundary pixels");
          return false;
        }
        for (int y = 0; y < joined.height(); ++y)
          for (int x = 0; x < joined.width(); ++x)
            if (joined.pixel(x, y) !=
                fitted.source.pixel(x + (!horizontal && x >= 90 ? 66 : 0),
                                    y + (horizontal && y >= 90 ? 66 : 0))) {
              error = QStringLiteral(
                  "Fractional cutout changed retained pixels or alpha");
              return false;
            }
        QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
        if (editor.captureData().source != fitted.source) {
          error = QStringLiteral("Fitted cutout undo lost native source");
          return false;
        }
        QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
        if (editor.captureData().source != joined) {
          error = QStringLiteral("Fitted cutout redo changed joined source");
          return false;
        }
      }
    }
  }
  for (int path = 0; path < 9; ++path) {
    CaptureEditor editor(capture, path == 0 || path == 7
                                      ? CaptureEditor::CaptureMode::Region
                                      : CaptureEditor::CaptureMode::Fullscreen);
    editor.setSuppressSnapshots(true);
    editor.resize(800, 600);
    editor.show();
    QApplication::processEvents();
    QWidget *target = &editor;
    if (path == 1) {
      QTest::keyClick(&editor, Qt::Key_T);
      QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(300, 300));
      target = QApplication::focusWidget();
      QTest::keyClicks(target, QStringLiteral("uncommitted"));
      QTest::keyClick(target, Qt::Key_A, Qt::ControlModifier);
      QTest::keyClick(target, Qt::Key_X, Qt::ControlModifier);
      const auto *inlineText = qobject_cast<QLineEdit *>(target);
      if (!inlineText || !inlineText->text().isEmpty() ||
          !target->isVisible()) {
        error = QStringLiteral("Ctrl+X did not remain native text editing");
        return false;
      }
    } else if (path == 2 || path == 6) {
      QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(398, 118));
      if (path == 6)
        QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(470, 157));
    } else if (path == 8) {
      QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(153, 305));
      QTest::mouseMove(&editor, QPoint(200, 305));
    } else if (path >= 3) {
      QTest::keyClick(&editor, path == 3   ? Qt::Key_X
                               : path == 4 ? Qt::Key_R
                                           : Qt::Key_V);
      QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(250, 250));
      QTest::mouseMove(&editor, QPoint(350, 350));
    }
    QTest::keyClick(target, Qt::Key_Escape);
    QApplication::processEvents();
    if (editor.isVisible()) {
      error = QStringLiteral("Single Escape did not close editor path %1")
                  .arg(path);
      return false;
    }
  }
  return true;
}

// These synthetic editor fixtures display 1:1 at widget origin (100,105).
CaptureData editorRegressionCapture() {
  CaptureData capture;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < 600; ++y)
    for (int x = 0; x < 800; ++x)
      capture.source.setPixelColor(x, y,
                                   QColor(x % 256, y % 256, (x + y) % 256));
  capture.previewSize = capture.source.size();
  capture.monitor.geometry = QRect(0, 0, 800, 600);
  capture.monitor.pixelSize = capture.source.size();
  capture.monitor.scale = 1.0;
  return capture;
}

void editorPointer(CaptureEditor &editor, QEvent::Type type,
                   const QPointF &point,
                   Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QMouseEvent event(type, point, point,
                    type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                    type == QEvent::MouseButtonRelease ? Qt::NoButton
                                                       : Qt::LeftButton,
                    modifiers);
  QApplication::sendEvent(&editor, &event);
}

void editorDrag(CaptureEditor &editor, const QPointF &start, const QPointF &end,
                Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  editorPointer(editor, QEvent::MouseButtonPress, start, modifiers);
  editorPointer(editor, QEvent::MouseMove, end, modifiers);
  editorPointer(editor, QEvent::MouseButtonRelease, end, modifiers);
  QApplication::processEvents();
}

void showRegressionEditor(CaptureEditor &editor) {
  editor.setSuppressSnapshots(true);
  editor.resize(1000, 800);
  editor.show();
  QApplication::processEvents();
}

bool runSelectedLayerRecoveryChecks(QString &error) {
  const CaptureData capture = editorRegressionCapture();
  const QRectF full(QPointF(), capture.previewSize);
  for (bool firstHandle : {true, false}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
    showRegressionEditor(editor);
    QTest::keyClick(&editor, Qt::Key_S);
    editorDrag(editor, QPointF(300, 305), QPointF(500, 505));
    QTest::keyClick(&editor, Qt::Key_V);
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 405));
    const QImage before = editor.renderCurrentOutput();
    editorDrag(editor, firstHandle ? QPointF(300, 305) : QPointF(500, 505),
               firstHandle ? QPointF(320, 325) : QPointF(520, 525));
    Annotation spotlight;
    spotlight.kind = Annotation::Kind::Spotlight;
    spotlight.color = QColor(QStringLiteral("#ff375f"));
    spotlight.start = firstHandle ? QPointF(220, 220) : QPointF(200, 200);
    spotlight.end = firstHandle ? QPointF(400, 400) : QPointF(420, 420);
    if (editor.renderCurrentOutput() !=
        renderCapture(capture, full, {spotlight}, BackgroundStyle::None)) {
      error = QStringLiteral("Selected elliptical spotlight handle started "
                             "marquee instead of resize");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    if (editor.renderCurrentOutput() != before) {
      error = QStringLiteral("Spotlight handle resize undo failed");
      return false;
    }
  }
  for (bool handle : {true, false}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
    showRegressionEditor(editor);
    QTest::keyClick(&editor, Qt::Key_R);
    editorDrag(editor, QPointF(120, 325), QPointF(200, 405));
    QTest::keyClick(&editor, Qt::Key_V);
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(120, 365));
    Annotation rectangle;
    rectangle.kind = Annotation::Kind::Rectangle;
    rectangle.start = QPointF(20, 220);
    rectangle.end = QPointF(100, 300);
    rectangle.color = QColor(QStringLiteral("#ff375f"));
    for (int index = 0; index < 10; ++index) {
      QWheelEvent wheel(QPointF(120, 365), QPointF(120, 365), {},
                        QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                        Qt::NoScrollPhase, false);
      QApplication::sendEvent(&editor, &wheel);
      rectangle.start =
          QPointF(60, 260) + (rectangle.start - QPointF(60, 260)) * 1.1;
      rectangle.end =
          QPointF(60, 260) + (rectangle.end - QPointF(60, 260)) * 1.1;
      rectangle.size *= 1.1;
    }
    const QImage before = editor.renderCurrentOutput();
    const QPointF outside = handle ? rectangle.start + QPointF(100, 105)
                                   : QPointF(rectangle.start.x() + 100, 365);
    const Annotation originalRectangle = rectangle;
    const QPointF outsideDelta(-4, 3);
    editorDrag(editor, outside, outside + outsideDelta);
    rectangle.start += outsideDelta;
    if (!handle)
      rectangle.end += outsideDelta;
    if (editor.renderCurrentOutput() !=
        renderCapture(capture, full, {rectangle}, BackgroundStyle::None)) {
      error = QStringLiteral(
          "Outside selected layer handle/body motion snapped to image edge");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    if (editor.renderCurrentOutput() != before) {
      error = QStringLiteral("Outside selected layer motion undo failed");
      return false;
    }
    rectangle = originalRectangle;
    editorDrag(editor, outside, QPointF(150, handle ? 330 : 365));
    if (handle)
      rectangle.start = QPointF(50, 225);
    else {
      const QPointF delta = QPointF(150, 365) - outside;
      rectangle.start += delta;
      rectangle.end += delta;
    }
    if (editor.renderCurrentOutput() !=
        renderCapture(capture, full, {rectangle}, BackgroundStyle::None)) {
      error = QStringLiteral(
          "Outside selected layer handle/body could not recover layer");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    if (editor.renderCurrentOutput() != before) {
      error = QStringLiteral("Outside selected layer recovery undo failed");
      return false;
    }
    editorDrag(editor, QPointF(40, 500), QPointF(180, 500));
    if (editor.renderCurrentOutput() != before) {
      error = QStringLiteral("Unrelated surround drag changed selected layer");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_R);
    editorDrag(editor, QPointF(40, 500), QPointF(180, 500));
    if (editor.renderCurrentOutput() != before) {
      error = QStringLiteral("General off-canvas drawing became active");
      return false;
    }
  }
  return true;
}

bool runLaunchModifierChecks(QString &error) {
  const CaptureData capture = editorRegressionCapture();
  const QRectF full(QPointF(), capture.previewSize);
  for (const int freshEvent : {0, 1, 2}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
    showRegressionEditor(editor);
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(158, 77));
    if (freshEvent != 0) {
      QKeyEvent key(freshEvent == 1 ? QEvent::KeyPress : QEvent::KeyRelease,
                    Qt::Key_Control, Qt::ControlModifier);
      QApplication::sendEvent(&editor, &key);
    }
    QWheelEvent wheel(QPointF(700, 500), QPointF(700, 500), {}, QPoint(0, 120),
                      Qt::NoButton, Qt::ControlModifier | Qt::ShiftModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(&editor, &wheel);
    const QPointF start(200, 200);
    const QPointF end(350, 280);
    editorDrag(editor, start + QPointF(100, 105), end + QPointF(100, 105),
               Qt::ControlModifier | Qt::ShiftModifier);
    Annotation arrow;
    arrow.kind = Annotation::Kind::Arrow;
    arrow.start = start;
    arrow.end = freshEvent == 0 ? end
                                : constrainedCreationEndpoint(
                                      CaptureEditor::Tool::Arrow, start, end);
    arrow.size = 5.0;
    arrow.color = QColor(QStringLiteral("#ff375f"));
    if (editor.renderCurrentOutput() !=
        renderCapture(capture, full, {arrow}, BackgroundStyle::None)) {
      error = QStringLiteral("Stale launch Shift was trusted, or fresh key "
                             "event did not restore Shift");
      return false;
    }
    if (freshEvent == 0) {
      QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(78, 77));
      editorDrag(editor, QPointF(350, 332), QPointF(380, 352),
                 Qt::ControlModifier);
      arrow.start += QPointF(30, 20);
      arrow.end += QPointF(30, 20);
      if (editor.renderCurrentOutput() !=
          renderCapture(capture, full, {arrow}, BackgroundStyle::None)) {
        error = QStringLiteral("Stale launch Ctrl turned selected body drag "
                               "into additive selection");
        return false;
      }
    }
  }
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
  showRegressionEditor(editor);
  QTest::keyClick(&editor, Qt::Key_R);
  editorDrag(editor, QPointF(300, 305), QPointF(400, 405));
  editorDrag(editor, QPointF(500, 305), QPointF(600, 405));
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::ControlModifier,
                    QPoint(300, 355));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::ControlModifier,
                    QPoint(500, 355));
  editorDrag(editor, QPointF(300, 355), QPointF(330, 375));
  QVector<Annotation> rectangles;
  for (int x : {200, 400}) {
    Annotation rectangle;
    rectangle.kind = Annotation::Kind::Rectangle;
    rectangle.start = QPointF(x + 30, 220);
    rectangle.end = QPointF(x + 130, 320);
    rectangle.color = QColor(QStringLiteral("#ff375f"));
    rectangles.push_back(rectangle);
  }
  if (editor.renderCurrentOutput() !=
      renderCapture(capture, full, rectangles, BackgroundStyle::None)) {
    error = QStringLiteral(
        "Fresh Ctrl-click multiselection no longer moves both layers");
    return false;
  }
  return true;
}

bool runEyedropperStateChecks(QString &error) {
  const CaptureData capture = editorRegressionCapture();
  const QRectF full(QPointF(), capture.previewSize);
  const QPoint sample(750, 550);
  const QColor sampled =
      sampleSourceColor(capture.source, capture.previewSize, full,
                        QRectF(100, 105, 800, 600), sample);
  for (bool toolbar : {false, true}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
    showRegressionEditor(editor);
    QTest::keyClick(&editor, Qt::Key_A);
    for (int repeat = 0; repeat < 2; ++repeat) {
      if (toolbar) {
        QTest::mouseMove(&editor, QPoint(478, 77));
        QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(574, 117));
      } else {
        QTest::keyClick(&editor, Qt::Key_I);
      }
    }
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, sample);
    editorDrag(editor, QPointF(300, 305), QPointF(450, 385));
    Annotation arrow;
    arrow.kind = Annotation::Kind::Arrow;
    arrow.start = QPointF(200, 200);
    arrow.end = QPointF(350, 280);
    arrow.color = sampled;
    if (editor.renderCurrentOutput() !=
        renderCapture(capture, full, {arrow}, BackgroundStyle::None)) {
      error =
          QStringLiteral(
              "Repeated eyedropper activation lost prior drawing tool (%1)")
              .arg(toolbar ? QStringLiteral("toolbar") : QStringLiteral("key"));
      return false;
    }
  }
  CaptureEditor editor(capture, CaptureEditor::CaptureMode::Fullscreen);
  showRegressionEditor(editor);
  QTest::keyClick(&editor, Qt::Key_R);
  editorDrag(editor, QPointF(300, 305), QPointF(400, 405));
  editorDrag(editor, QPointF(500, 305), QPointF(600, 405));
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::ControlModifier,
                    QPoint(300, 355));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::ControlModifier,
                    QPoint(500, 355));
  const QImage before = editor.renderCurrentOutput();
  QVector<Annotation> rectangles;
  for (int x : {200, 400}) {
    Annotation rectangle;
    rectangle.kind = Annotation::Kind::Rectangle;
    rectangle.start = QPointF(x, 200);
    rectangle.end = QPointF(x + 100, 300);
    rectangle.color = QColor(QStringLiteral("#ff375f"));
    rectangles.push_back(rectangle);
  }
  for (int repeat = 0; repeat < 2; ++repeat) {
    QTest::keyClick(&editor, Qt::Key_I);
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, sample);
    rectangles.last().color = sampled;
    if (editor.renderCurrentOutput() !=
        renderCapture(capture, full, rectangles, BackgroundStyle::None)) {
      error = QStringLiteral(
          "Eyedropper lost primary selection during repeated recolor");
      return false;
    }
  }
  QTest::keyClick(&editor, Qt::Key_2);
  rectangles.last().color = QColor(QStringLiteral("#ff9f0a"));
  if (editor.renderCurrentOutput() !=
      renderCapture(capture, full, rectangles, BackgroundStyle::None)) {
    error = QStringLiteral("Eyedropper cleared primary selection");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Delete);
  if (editor.renderCurrentOutput() != capture.source) {
    error = QStringLiteral("Eyedropper cleared multiselection");
    return false;
  }
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  if (editor.renderCurrentOutput() != before) {
    error = QStringLiteral("Eyedropper recolor undo lost layers");
    return false;
  }
  for (Qt::Key key : {Qt::Key_D, Qt::Key_S}) {
    CaptureEditor excluded(capture, CaptureEditor::CaptureMode::Fullscreen);
    showRegressionEditor(excluded);
    QTest::keyClick(&excluded, key);
    editorDrag(excluded, QPointF(300, 305), QPointF(500, 505));
    QTest::keyClick(&excluded, Qt::Key_V);
    QTest::mouseClick(&excluded, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 405));
    const QImage unchanged = excluded.renderCurrentOutput();
    QTest::keyClick(&excluded, Qt::Key_I);
    QTest::mouseClick(&excluded, Qt::LeftButton, Qt::NoModifier, sample);
    if (excluded.renderCurrentOutput() != unchanged) {
      error =
          QStringLiteral("Eyedropper recolored excluded redaction/spotlight");
      return false;
    }
    QTest::keyClick(&excluded, Qt::Key_Delete);
    if (excluded.renderCurrentOutput() != capture.source) {
      error = QStringLiteral("Eyedropper lost excluded layer selection");
      return false;
    }
  }
  return true;
}

bool runStableCropDragChecks(QString &error) {
  const CaptureData capture = editorRegressionCapture();
  for (int fixture = 0; fixture < 6; ++fixture) {
    const int handle = std::array{7, 1, 0}.at(fixture % 3);
    const BackgroundStyle background =
        fixture < 3 ? BackgroundStyle::None : BackgroundStyle::Aurora;
    CaptureEditor editor(capture);
    showRegressionEditor(editor);
    editorDrag(editor, QPointF(200, 150), QPointF(600, 450));
    if (background != BackgroundStyle::None)
      QTest::keyClick(&editor, Qt::Key_B);
    QTest::keyClick(&editor, Qt::Key_R);
    editorDrag(editor, QPointF(450, 355), QPointF(550, 455));
    QTest::keyClick(&editor, Qt::Key_V);
    const QImage before = editor.renderCurrentOutput();
    const QPointF press = handle == 7   ? QPointF(293, 405)
                          : handle == 1 ? QPointF(500, 248)
                                        : QPointF(293, 248);
    const QPointF move = handle == 7   ? QPointF(340, 405)
                         : handle == 1 ? QPointF(500, 295)
                                       : QPointF(340, 295);
    const QPoint sourceProbe(650, 500);
    const QColor sourceColor = capture.source.pixelColor(550, 395);
    editorPointer(editor, QEvent::MouseButtonPress, press);
    for (const qreal fraction : {0.5, 1.0}) {
      const QPointF intermediate = press + fraction * (move - press);
      editorPointer(
          editor, QEvent::MouseMove,
          QPointF(qRound(intermediate.x()), qRound(intermediate.y())));
      QApplication::processEvents();
      const QImage ui = editor.grab().toImage();
      if (ui.pixelColor(sourceProbe) != sourceColor ||
          ui.pixelColor(450, 405) != QColor(QStringLiteral("#ff375f"))) {
        error = QStringLiteral(
                    "Live crop recentered source pixels or detached "
                    "annotations (%1): source %2 expected %3, annotation %4")
                    .arg(handle)
                    .arg(ui.pixelColor(sourceProbe).name(), sourceColor.name(),
                         ui.pixelColor(450, 405).name());
        return false;
      }
    }
    editorPointer(editor, QEvent::MouseButtonRelease, move);
    QApplication::processEvents();
    const QPointF offset(handle == 1 ? 0 : 40, handle == 7 ? 0 : 40);
    const QRectF selection(QPointF(200, 150) + offset,
                           QSizeF(400 - offset.x(), 300 - offset.y()));
    Annotation rectangle;
    rectangle.kind = Annotation::Kind::Rectangle;
    rectangle.start = QPointF(150, 100) - offset;
    rectangle.end = QPointF(250, 200) - offset;
    rectangle.color = QColor(QStringLiteral("#ff375f"));
    if (editor.renderCurrentOutput() !=
            renderCapture(capture, selection, {rectangle}, background) ||
        editor.grab().toImage().pixelColor(sourceProbe) == sourceColor) {
      error = QStringLiteral(
          "Crop release did not refit, or source/annotation output shifted");
      return false;
    }
    const QImage cropped = editor.renderCurrentOutput();
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    if (editor.renderCurrentOutput() != before) {
      error =
          QStringLiteral("Stable crop undo lost source/annotation alignment");
      return false;
    }
    QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
    if (editor.renderCurrentOutput() != cropped) {
      error =
          QStringLiteral("Stable crop redo lost source/annotation alignment");
      return false;
    }
  }
  // Fit-down keeps the same source sample; allow native-crop interpolation.
  for (const int margin : {0, 64}) {
    CaptureEditor fitted(capture);
    fitted.setSuppressSnapshots(true);
    fitted.resize(800, 600);
    fitted.show();
    QApplication::processEvents();
    editorDrag(fitted, QPointF(0, 0), QPointF(800, 600));
    if (margin)
      QTest::keyClick(&fitted, Qt::Key_B);
    const qreal scale = 474.0 / (600 + 2 * margin);
    const QPointF press(400 - 400 * scale - 7, 305 - 300 * scale - 7);
    const QPointF moved = press + QPointF(46.5, 46.5);
    const QPoint probe(500, 400);
    const QColor fittedBefore = fitted.grab().toImage().pixelColor(probe);
    editorPointer(fitted, QEvent::MouseButtonPress, press);
    editorPointer(fitted, QEvent::MouseMove, moved);
    QApplication::processEvents();
    const QColor fittedDuring = fitted.grab().toImage().pixelColor(probe);
    if (std::abs(fittedBefore.red() - fittedDuring.red()) > 2 ||
        std::abs(fittedBefore.green() - fittedDuring.green()) > 2 ||
        std::abs(fittedBefore.blue() - fittedDuring.blue()) > 2) {
      error = QStringLiteral("Fitted live crop recentered source sample");
      return false;
    }
    editorPointer(fitted, QEvent::MouseButtonRelease, moved);
  }
  // Enlarging a crop keeps chrome clear even when its live image reaches it.
  CaptureEditor expanded(capture);
  showRegressionEditor(expanded);
  editorDrag(expanded, QPointF(200, 300), QPointF(600, 400));
  editorPointer(expanded, QEvent::MouseButtonPress, QPointF(293, 348));
  editorPointer(expanded, QEvent::MouseMove, QPointF(100, 55));
  QApplication::processEvents();
  if (expanded.grab().toImage().pixelColor(200, 60) != QColor(0, 0, 0, 160)) {
    error = QStringLiteral("Expanded live crop painted into editor chrome: %1")
                .arg(expanded.grab().toImage().pixelColor(200, 60).name(
                    QColor::HexArgb));
    return false;
  }
  editorPointer(expanded, QEvent::MouseButtonRelease, QPointF(100, 55));
  if (expanded.renderCurrentOutput() !=
      renderCapture(capture, QRectF(0, 0, 600, 400), {},
                    BackgroundStyle::None)) {
    error = QStringLiteral("Expanded crop release changed native output");
    return false;
  }
  return true;
}

/** Checks that positional local image targets are recognized. */
bool runPositionalImageTargetCheck(QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create positional target directory");
    return false;
  }
  const QString path = QDir(directory.path())
                           .filePath(QStringLiteral("image with # marker.png"));
  QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::white);
  const QString url = QUrl::fromLocalFile(path).toString();
  const QString previousDirectory = QDir::currentPath();
  const QString colonName = QStringLiteral("image:one.png");
  const QString colonPath = QDir(directory.path()).filePath(colonName);
  const QString remoteLookalikeName =
      QStringLiteral("https:/example.com/image.png");
  const bool savedColonImage = image.save(colonPath, "PNG");
  const bool changedDirectory = QDir::setCurrent(directory.path());
  const QString resolvedColonPath = resolveLocalImagePath(colonName);
  const bool createdRemoteLookalike =
      changedDirectory && QDir().mkpath(QStringLiteral("https:/example.com")) &&
      image.save(remoteLookalikeName, "PNG");
  const QString resolvedRemoteUrl = createdRemoteLookalike
                                        ? resolveLocalImagePath(QStringLiteral(
                                              "https://example.com/image.png"))
                                        : QStringLiteral("setup failed");
  QDir::setCurrent(previousDirectory);
  if (!image.save(path, "PNG") || resolveLocalImagePath(path) != path ||
      resolveLocalImagePath(url) != path || !changedDirectory ||
      resolvedColonPath != colonPath || !savedColonImage ||
      !createdRemoteLookalike || !resolvedRemoteUrl.isEmpty() ||
      !resolveLocalImagePath(
           QDir(directory.path()).filePath(QStringLiteral("missing.png")))
           .isEmpty()) {
    error =
        QStringLiteral("Local file URL was not recognized as an image target");
    return false;
  }
  return true;
}

/** Guards the working-snapshot lifecycle: create and overwrite in place. */
bool runTemporarySnapshotChecks(QString &error) {
  const QString path = temporarySnapshotPath();
  QImage firstImage(64, 64, QImage::Format_ARGB32_Premultiplied);
  firstImage.fill(QColor(QStringLiteral("#123456")));
  // PNG loading yields straight-alpha ARGB32; compare in our paint format so
  // pixel equality alone decides the outcome.
  const auto reload = [](const QString &file) {
    return QImage(file).convertToFormat(QImage::Format_ARGB32_Premultiplied);
  };
  if (!saveTemporarySnapshot(firstImage, path, error) ||
      reload(path) != firstImage || reload(path).isNull()) {
    if (error.isEmpty())
      error = QStringLiteral("Snapshot reload did not match the saved image");
    return false;
  }

  QImage secondImage(64, 64, QImage::Format_ARGB32_Premultiplied);
  secondImage.fill(QColor(QStringLiteral("#654321")));
  if (!saveTemporarySnapshot(secondImage, path, error))
    return false; // O_EXCL used to fail on the second call.
  if (reload(path) != secondImage) {
    error = QStringLiteral("Overwritten snapshot did not replace the first");
    return false;
  }

  QFile baselineFile(path);
  if (!baselineFile.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read the valid snapshot baseline");
    return false;
  }
  const QByteArray baseline = baselineFile.readAll();
  baselineFile.close();

  QImage largeImage(512, 512, QImage::Format_ARGB32_Premultiplied);
  quint32 noise = 0x12345678U;
  for (int y = 0; y < largeImage.height(); ++y) {
    for (int x = 0; x < largeImage.width(); ++x) {
      noise ^= noise << 13;
      noise ^= noise >> 17;
      noise ^= noise << 5;
      largeImage.setPixel(x, y, 0xff000000U | (noise & 0x00ffffffU));
    }
  }

  struct rlimit originalLimit{};
  struct sigaction originalSignal{};
  struct sigaction ignoredSignal{};
  ignoredSignal.sa_handler = SIG_IGN;
  sigemptyset(&ignoredSignal.sa_mask);
  if (::getrlimit(RLIMIT_FSIZE, &originalLimit) != 0 ||
      ::sigaction(SIGXFSZ, &ignoredSignal, &originalSignal) != 0) {
    error = QStringLiteral("Could not prepare the interrupted-write check");
    return false;
  }
  struct rlimit limited = originalLimit;
  limited.rlim_cur = std::min<rlim_t>(originalLimit.rlim_cur, 1024);
  const bool limitSet = ::setrlimit(RLIMIT_FSIZE, &limited) == 0;
  QString writeError;
  const int savedStderr = ::dup(STDERR_FILENO);
  const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (savedStderr >= 0 && devnull >= 0)
    ::dup2(devnull, STDERR_FILENO);
  const bool interruptedSave =
      limitSet && saveTemporarySnapshot(largeImage, path, writeError);
  if (savedStderr >= 0) {
    ::dup2(savedStderr, STDERR_FILENO);
    ::close(savedStderr);
  }
  if (devnull >= 0)
    ::close(devnull);
  const bool limitRestored = ::setrlimit(RLIMIT_FSIZE, &originalLimit) == 0;
  const bool signalRestored =
      ::sigaction(SIGXFSZ, &originalSignal, nullptr) == 0;

  QFile preservedFile(path);
  const bool preservedOpened = preservedFile.open(QIODevice::ReadOnly);
  const QByteArray preserved = preservedFile.readAll();
  if (!limitSet || interruptedSave || !limitRestored || !signalRestored ||
      !preservedOpened || preserved != baseline) {
    error = QStringLiteral(
        "A failed snapshot write did not preserve the previous PNG");
    return false;
  }

  const QFileInfo fileInfo(path);
  // The runtime directory must stay private and the snapshot must not be
  // readable by group or other.
  const QFileInfo dirInfo = QFileInfo(fileInfo.absolutePath());
  const QFileDevice::Permissions owner =
      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
  const QFileDevice::Permissions shared =
      QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
      QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
  if ((dirInfo.permissions() & owner) != owner ||
      (dirInfo.permissions() & shared) != 0 ||
      (fileInfo.permissions() & shared) != 0)
    return false;
  QTemporaryDir securityRoot;
  const QString repairPath =
      QDir(securityRoot.path()).filePath(QStringLiteral("repair-me"));
  if (!securityRoot.isValid() || !QDir().mkdir(repairPath) ||
      !QFile::setPermissions(repairPath, owner | shared) ||
      !ensurePrivateDirectory(repairPath) ||
      (QFileInfo(repairPath).permissions() & shared) != 0) {
    error = QStringLiteral("Private directory permissions were not repaired");
    return false;
  }
  const QString symlinkPath =
      QDir(securityRoot.path()).filePath(QStringLiteral("directory-link"));
  if (!QFile::link(repairPath, symlinkPath) ||
      ensurePrivateDirectory(symlinkPath)) {
    error = QStringLiteral("Private directory accepted a symbolic link");
    return false;
  }
  const QString outsideDirectory =
      QDir(securityRoot.path()).filePath(QStringLiteral("outside"));
  const QFileDevice::Permissions visibleToGroup =
      QFileDevice::ReadGroup | QFileDevice::ExeGroup;
  if (!QDir().mkdir(outsideDirectory) ||
      !QFile::setPermissions(outsideDirectory, owner | visibleToGroup)) {
    error = QStringLiteral("Could not prepare snapshot boundary check");
    return false;
  }
  error.clear();
  const QString outsideSnapshot =
      QDir(outsideDirectory).filePath(QStringLiteral("snapshot.png"));
  if (saveTemporarySnapshot(secondImage, outsideSnapshot, error) ||
      QFile::exists(outsideSnapshot) ||
      (QFileInfo(outsideDirectory).permissions() & visibleToGroup) !=
          visibleToGroup) {
    error = QStringLiteral("Temporary snapshot escaped its private directory");
    return false;
  }
  QFile::remove(path);
  return true;
}

bool runHighlighterRenderingCheck(QString &error) {
  error = QStringLiteral("Highlighter rendering check failed");
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {200, 100};
  // Transparent canvas: the selected capture is composited underneath, so a
  // white source would make every alpha check read 255.
  capture.source = QImage(200, 100, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(Qt::transparent);
  capture.previewSize = capture.source.size();

  Annotation highlight;
  highlight.kind = Annotation::Kind::Highlighter;
  highlight.color = QColor(QStringLiteral("#ffd60a"));
  highlight.size = 4;
  highlight.points = {{50, 50}, {90, 50}, {130, 50}, {170, 50}};
  const QImage rendered = renderCapture(capture, QRectF(0, 0, 200, 100),
                                        {highlight}, BackgroundStyle::None);
  if (rendered.isNull())
    return false;

  int midAlpha = 0;
  bool ok = true;
  for (const int x : {60, 90, 120, 150}) {
    const int a = rendered.pixelColor(x, 50).alpha();
    midAlpha = std::max(midAlpha, a);
    if (a < 96 || a > 160)
      ok = false; // translucent middle of the stroke
  }
  for (const int offset : {-5, 5}) {
    if (rendered.pixelColor(100, 50 + offset).alpha() <= 0)
      ok = false; // width ~= size*3 keeps the band opaque enough
  }
  if (rendered.pixelColor(10, 50).alpha() != 0)
    ok = false; // untouched area stays transparent
  if (midAlpha == 255)
    ok = false; // highlight must blend, not paint solid
  if (!ok) {
    error = QStringLiteral("Highlighter stroke was not translucent/wide");
    return false;
  }
  return true;
}

bool runSecureRedactionChecks(QString &error) {
  error = QStringLiteral("Secure redaction rendering check failed");
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {96, 72};
  capture.source = QImage(96, 72, QImage::Format_RGB32);
  for (int y = 0; y < capture.source.height(); ++y) {
    for (int x = 0; x < capture.source.width(); ++x) {
      capture.source.setPixelColor(
          x, y, QColor((x * 3) % 256, (y * 5) % 256, ((x + y) * 7) % 256));
    }
  }
  capture.previewSize = capture.source.size();

  Annotation pixelate;
  pixelate.kind = Annotation::Kind::Redaction;
  pixelate.start = {12, 12};
  pixelate.end = {84, 60};
  pixelate.redactionStyle = RedactionStyle::Pixelate;
  pixelate.redactionSeed = 0x1234abcdU;
  const QRect redactionBounds(12, 12, 72, 48);
  const QImage first = renderCapture(capture, QRectF(0, 0, 96, 72), {pixelate},
                                     BackgroundStyle::None);
  const QImage repeated = renderCapture(capture, QRectF(0, 0, 96, 72),
                                        {pixelate}, BackgroundStyle::None);
  if (first.isNull() || first != repeated ||
      first.pixelColor(2, 2) != capture.source.pixelColor(2, 2))
    return false;

  // Every exported mosaic cell is a flat synthetic color. No fine-grained
  // source pixels survive inside a cell.
  constexpr int blockSize = 12;
  for (int top = redactionBounds.top(); top <= redactionBounds.bottom();
       top += blockSize) {
    for (int left = redactionBounds.left(); left <= redactionBounds.right();
         left += blockSize) {
      const QColor cell = first.pixelColor(left, top);
      if (cell.alpha() != 255)
        return false;
      const int bottom =
          std::min(top + blockSize - 1, redactionBounds.bottom());
      const int right = std::min(left + blockSize - 1, redactionBounds.right());
      for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
          if (first.pixelColor(x, y) != cell)
            return false;
        }
      }
    }
  }

  // Mirroring the source preserves the region's aggregate palette but changes
  // every spatial relationship. Identical redaction output proves the mosaic
  // does not retain source-block positions.
  CaptureData mirroredCapture = capture;
  for (int y = 0; y < capture.source.height(); ++y) {
    for (int x = 0; x < capture.source.width(); ++x) {
      mirroredCapture.source.setPixelColor(
          x, y, capture.source.pixelColor(capture.source.width() - x - 1, y));
    }
  }
  mirroredCapture.previewSize = mirroredCapture.source.size();
  const QImage mirrored = renderCapture(mirroredCapture, QRectF(0, 0, 96, 72),
                                        {pixelate}, BackgroundStyle::None);
  if (first.copy(redactionBounds) != mirrored.copy(redactionBounds))
    return false;

  Annotation differentSeed = pixelate;
  ++differentSeed.redactionSeed;
  const QImage reseeded = renderCapture(capture, QRectF(0, 0, 96, 72),
                                        {differentSeed}, BackgroundStyle::None);
  if (first.copy(redactionBounds) == reseeded.copy(redactionBounds))
    return false;

  Annotation solid = pixelate;
  solid.redactionStyle = RedactionStyle::Solid;
  const QImage solidOutput = renderCapture(capture, QRectF(0, 0, 96, 72),
                                           {solid}, BackgroundStyle::None);
  const QColor solidColor(QStringLiteral("#121216"));
  for (int y = redactionBounds.top(); y <= redactionBounds.bottom(); ++y) {
    for (int x = redactionBounds.left(); x <= redactionBounds.right(); ++x) {
      if (solidOutput.pixelColor(x, y) != solidColor)
        return false;
    }
  }

  Annotation line;
  line.kind = Annotation::Kind::Line;
  line.start = {12, 36};
  line.end = {83, 36};
  line.color = Qt::white;
  line.size = 4;
  const QImage annotated = renderCapture(capture, QRectF(0, 0, 96, 72),
                                         {solid, line}, BackgroundStyle::None);
  if (annotated.pixelColor(48, 36) != QColor(Qt::white))
    return false;

  Annotation clipped = solid;
  clipped.start = {-10, -10};
  clipped.end = {20, 20};
  const QImage clippedOutput = renderCapture(capture, QRectF(0, 0, 96, 72),
                                             {clipped}, BackgroundStyle::None);
  if (clippedOutput.pixelColor(0, 0) != solidColor ||
      clippedOutput.pixelColor(21, 21) != capture.source.pixelColor(21, 21))
    return false;

  CaptureData highDpi = capture;
  highDpi.monitor.scale = 2.0;
  highDpi.monitor.pixelSize = {192, 144};
  highDpi.source = capture.source.scaled(192, 144, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
  highDpi.previewSize = capture.source.size();
  Annotation highDpiSolid = solid;
  highDpiSolid.start = {10, 10};
  highDpiSolid.end = {30, 30};
  const QImage highDpiOutput = renderCapture(
      highDpi, QRectF(0, 0, 96, 72), {highDpiSolid}, BackgroundStyle::None);
  if (highDpiOutput.size() != QSize(192, 144) ||
      highDpiOutput.pixelColor(20, 20) != solidColor ||
      highDpiOutput.pixelColor(59, 59) != solidColor ||
      highDpiOutput.pixelColor(61, 61) == solidColor)
    return false;

  // Fractional crop rounding can force a one-pixel smooth resize. Redact the
  // native source first so protected colors cannot bleed just outside the
  // final mask through the resampling kernel.
  CaptureData fractional;
  fractional.monitor.scale = 1.25;
  fractional.previewSize = QSize(100, 100);
  fractional.source = QImage(125, 125, QImage::Format_RGB32);
  fractional.source.fill(Qt::white);
  for (int y = 13; y <= 38; ++y) {
    for (int x = 13; x <= 38; ++x)
      fractional.source.setPixelColor(x, y, Qt::red);
  }
  Annotation fractionalSolid;
  fractionalSolid.kind = Annotation::Kind::Redaction;
  fractionalSolid.redactionStyle = RedactionStyle::Solid;
  fractionalSolid.start = {10, 10};
  fractionalSolid.end = {30, 30};
  const QImage fractionalOutput =
      renderCapture(fractional, QRectF(1, 1, 81, 81), {fractionalSolid},
                    BackgroundStyle::None);
  const QColor outsideHorizontal = fractionalOutput.pixelColor(12, 11);
  const QColor outsideVertical = fractionalOutput.pixelColor(11, 12);
  if (fractionalOutput.size() != QSize(101, 101) ||
      outsideHorizontal.red() != outsideHorizontal.green() ||
      outsideHorizontal.green() != outsideHorizontal.blue() ||
      outsideVertical.red() != outsideVertical.green() ||
      outsideVertical.green() != outsideVertical.blue() ||
      fractionalOutput.pixelColor(12, 12) != solidColor)
    return false;

  CaptureData matchingFractional;
  matchingFractional.monitor.scale = 1.25;
  matchingFractional.previewSize = QSize(160, 160);
  matchingFractional.source = QImage(200, 200, QImage::Format_RGB32);
  matchingFractional.source.fill(Qt::white);
  for (int y = 13; y <= 26; ++y) {
    for (int x = 13; x <= 26; ++x)
      matchingFractional.source.setPixelColor(x, y, Qt::red);
  }
  Annotation matchingSolid;
  matchingSolid.kind = Annotation::Kind::Redaction;
  matchingSolid.redactionStyle = RedactionStyle::Solid;
  matchingSolid.start = {10.75, 10.75};
  matchingSolid.end = {20.75, 20.75};
  const QImage matchingOutput =
      renderCapture(matchingFractional, QRectF(0.08, 0.08, 79.68, 79.68),
                    {matchingSolid}, BackgroundStyle::None);
  if (matchingOutput.size() != QSize(100, 100) ||
      matchingOutput.pixelColor(26, 26) != solidColor)
    return false;

  CaptureData nonIntegralSourceScale;
  nonIntegralSourceScale.monitor.scale = 1.5;
  nonIntegralSourceScale.previewSize = QSize(1707, 100);
  nonIntegralSourceScale.source = QImage(2561, 150, QImage::Format_RGB32);
  nonIntegralSourceScale.source.fill(Qt::white);
  for (int y = 0; y < nonIntegralSourceScale.source.height(); ++y)
    nonIntegralSourceScale.source.setPixelColor(1500, y, Qt::red);
  Annotation sourceScaleSolid;
  sourceScaleSolid.kind = Annotation::Kind::Redaction;
  sourceScaleSolid.redactionStyle = RedactionStyle::Solid;
  sourceScaleSolid.start = {0, 0};
  sourceScaleSolid.end = {1000, 100};
  const QImage sourceScaleOutput =
      renderCapture(nonIntegralSourceScale, QRectF(0, 0, 1707, 100),
                    {sourceScaleSolid}, BackgroundStyle::None);
  if (sourceScaleOutput.pixelColor(1500, 50) != solidColor)
    return false;
  return true;
}

bool runCreationConstraintCheck(QString &error) {
  const QPointF start(10, 10);
  const QPointF square = constrainedCreationEndpoint(
      CaptureEditor::Tool::Rectangle, start, QPointF(40, 30));
  if (square != QPointF(40, 40)) {
    error = QStringLiteral(
        "Rectangle creation constraint did not preserve a 1:1 bounding box");
    return false;
  }

  for (const CaptureEditor::Tool tool :
       {CaptureEditor::Tool::Line, CaptureEditor::Tool::Arrow}) {
    const QPointF rawEnd(40, 22);
    const QPointF snapped = constrainedCreationEndpoint(tool, start, rawEnd);
    const QPointF delta = snapped - start;
    const qreal snappedAngle = std::atan2(delta.y(), delta.x());
    constexpr qreal angleStep = std::numbers::pi_v<qreal> / 4.0;
    const qreal steps = snappedAngle / angleStep;
    if (std::abs(steps - std::round(steps)) > 0.0001 ||
        std::abs(QLineF(start, snapped).length() -
                 QLineF(start, rawEnd).length()) > 0.0001) {
      error = QStringLiteral(
          "Line creation constraint did not snap to 45 degrees");
      return false;
    }
  }

  const QPointF unchanged = constrainedCreationEndpoint(
      CaptureEditor::Tool::Freehand, start, QPointF(33, 52));
  if (unchanged != QPointF(33, 52)) {
    error = QStringLiteral("Creation constraint changed an unrelated tool");
    return false;
  }
  return true;
}

bool runQuickOutputChecks(QString &error) {
  QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
  image.fill(QColor(QStringLiteral("#345678")));
  QString outputError = QStringLiteral("unchanged");
  if (quickOutput({}, QuickOutputMode::Save, outputError) ||
      outputError == QStringLiteral("unchanged") ||
      quickOutput(image, QuickOutputMode::None, outputError)) {
    error = QStringLiteral("quickOutput accepted an invalid request");
    return false;
  }

  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create quick-output directory");
    return false;
  }
  const QByteArray previousDir = qgetenv("NIRISNAP_SCREENSHOT_DIR");
  qputenv("NIRISNAP_SCREENSHOT_DIR", directory.path().toUtf8());
  outputError.clear();
  const bool saved = quickOutput(image, QuickOutputMode::Save, outputError);
  const QStringList files =
      QDir(directory.path()).entryList({QStringLiteral("*.png")}, QDir::Files);
  if (previousDir.isEmpty())
    qunsetenv("NIRISNAP_SCREENSHOT_DIR");
  else
    qputenv("NIRISNAP_SCREENSHOT_DIR", previousDir);
  if (!saved || !outputError.isEmpty() || files.size() != 1 ||
      QImage(QDir(directory.path()).filePath(files.constFirst())).isNull() ||
      QFile::exists(temporarySnapshotPath())) {
    error = QStringLiteral("quickOutput did not save exactly one PNG");
    return false;
  }
  return true;
}

/**
 * Crash recovery: the working snapshot must reach disk while editing, must be
 * consumed by the save as a default-encoded file, and must not outlive a quit.
 */
bool runCrashSnapshotChecks(const CaptureData &capture, QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create crash-snapshot directory");
    return false;
  }
  const QByteArray previousDir = qgetenv("NIRISNAP_SCREENSHOT_DIR");
  qputenv("NIRISNAP_SCREENSHOT_DIR", directory.path().toUtf8());
  const auto restoreDir = qScopeGuard(
      [&previousDir] { qputenv("NIRISNAP_SCREENSHOT_DIR", previousDir); });
  const QString snapshotPath = temporarySnapshotPath();
  QFile::remove(snapshotPath);
  const auto settleUntilWritten = [&snapshotPath] {
    QElapsedTimer settle;
    settle.start();
    while (!QFile::exists(snapshotPath) && settle.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      QThread::msleep(2);
    }
    return QFile::exists(snapshotPath);
  };
  const auto annotate = [](CaptureEditor &editor) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&editor, QPoint(650, 470), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    QTest::keyClick(&editor, Qt::Key_R);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 220));
    QTest::mouseMove(&editor, QPoint(420, 320), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 320));
    QCoreApplication::processEvents();
  };

  {
    CaptureEditor editor(capture);
    editor.resize(800, 600);
    editor.show();
    annotate(editor);
    if (!settleUntilWritten()) {
      error = QStringLiteral("No working snapshot was written while editing");
      return false;
    }
    editor.waitForSnapshot();
    const QImage current = editor.renderCurrentOutput();
    if (QImage(snapshotPath).convertToFormat(QImage::Format_ARGB32) !=
        current.convertToFormat(QImage::Format_ARGB32)) {
      error = QStringLiteral("Working snapshot did not reflect the edit");
      return false;
    }
    QByteArray reference;
    QBuffer referenceBuffer(&reference);
    referenceBuffer.open(QIODevice::WriteOnly);
    current.save(&referenceBuffer, "PNG");
    QTest::keyClick(&editor, Qt::Key_S, Qt::ControlModifier);
    QCoreApplication::processEvents();
    const QStringList files =
        QDir(directory.path())
            .entryList({QStringLiteral("*.png")}, QDir::Files);
    if (files.size() != 1 || QFile::exists(snapshotPath) ||
        QFileInfo(QDir(directory.path()).filePath(files.constFirst())).size() >
            reference.size() * 3 / 2) {
      error = QStringLiteral(
          "Save did not leave exactly one default-encoded screenshot");
      return false;
    }
  }

  {
    CaptureEditor quitEditor(capture);
    quitEditor.resize(800, 600);
    quitEditor.show();
    annotate(quitEditor);
    QTest::keyClick(&quitEditor, Qt::Key_Escape);
    QCoreApplication::processEvents();
    if (!settleUntilWritten() || quitEditor.isVisible()) {
      error = QStringLiteral("Editing before a quit left no snapshot to clean");
      return false;
    }
  }
  if (QFile::exists(snapshotPath)) {
    error = QStringLiteral("Quitting left the working snapshot behind");
    return false;
  }
  return true;
}

bool runSpotlightAndSampleChecks(QString &error) {
  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {80, 40};
  capture.source = QImage(80, 40, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#204080")));
  capture.source.setPixelColor(10, 20, QColor(QStringLiteral("#ff0000")));
  capture.previewSize = capture.source.size();

  Annotation line;
  line.kind = Annotation::Kind::Line;
  line.start = {5, 20};
  line.end = {75, 20};
  line.color = QColor(QStringLiteral("#00ff00"));
  line.size = 4;
  Annotation lens;
  lens.kind = Annotation::Kind::Spotlight;
  lens.start = {0, 0};
  lens.end = {80, 40};
  lens.magnification = 2.0;
  lens.color = Qt::white;
  const QImage rendered =
      renderCapture(capture, QRectF(0, 0, 80, 40), {line, lens},
                    BackgroundStyle::None);
  if (rendered.isNull() || rendered.pixelColor(40, 2).alpha() < 100) {
    error = QStringLiteral("Spotlight did not dim the surrounding image");
    return false;
  }

  QImage checker(8, 4, QImage::Format_ARGB32);
  checker.fill(QColor(255, 0, 0));
  checker.setPixelColor(0, 0, QColor(0, 255, 0));
  checker.setPixelColor(7, 0, QColor(0, 0, 255));
  checker.setPixelColor(3, 1, QColor(10, 20, 30, 80));
  const QColor mid = sampleSourceColor(checker, QSizeF(8, 4), QRectF(0, 0, 8, 4),
                                       QRectF(0, 0, 80, 40), QPointF(35, 15));
  const QColor right =
      sampleSourceColor(checker, QSizeF(8, 4), QRectF(1.5, 0, 5, 4),
                        QRectF(0, 0, 80, 40), QPointF(79, 5));
  const QColor faded =
      sampleSourceColor(checker, QSizeF(8, 4), QRectF(0, 0, 8, 4),
                        QRectF(0, 0, 80, 40), QPointF(35, 15));
  if (!mid.isValid() || right == QColor(0, 0, 255) || faded.alpha() != 255) {
    error = QStringLiteral("Eyedropper sampled outside the crop or kept alpha");
    return false;
  }
  return true;
}

/** Checks that clicking away commits in-progress text instead of losing it. */
bool runTextClickAwayCommitCheck(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  const QString snapshotPath = temporarySnapshotPath();
  const QRectF selection(100, 100, 550, 370);
  const qreal ascent = QFontMetricsF(annotationTextFont(5.0)).ascent();
  const auto textAnnotation = [&ascent](const QPointF &origin,
                                        const QString &value) {
    Annotation text;
    text.kind = Annotation::Kind::Text;
    text.start = origin + QPointF(0, ascent);
    text.text = value;
    text.color = QColor(QStringLiteral("#ff375f"));
    text.size = 5.0;
    return text;
  };
  CaptureEditor editor(capture);
  const auto snapshotMatches = [&editor, &snapshotPath](const QImage &expected) {
    return !expected.isNull() &&
           flushedSnapshot(editor, snapshotPath)
                   .convertToFormat(expected.format()) == expected;
  };
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(650, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(650, 470));
  application.processEvents();

  QTest::keyClick(&editor, Qt::Key_T);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 220));
  application.processEvents();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 400));
  application.processEvents();
  if (!snapshotMatches(
          renderCapture(capture, selection, {}, BackgroundStyle::None))) {
    error = QStringLiteral("Clicking away from empty text added an annotation");
    return false;
  }

  const Annotation clicked =
      textAnnotation({125, 280}, QStringLiteral("Clicked away"));
  QTest::keyClicks(QApplication::focusWidget(), clicked.text);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 300));
  application.processEvents();
  if (!snapshotMatches(
          renderCapture(capture, selection, {clicked}, BackgroundStyle::None))) {
    error = QStringLiteral("Clicking the canvas discarded in-progress text");
    return false;
  }

  const Annotation toolbar =
      textAnnotation({325, 180}, QStringLiteral("Toolbar"));
  // 100,92 is the arrow tool button: the toolbar row sits above the capture.
  QTest::keyClicks(QApplication::focusWidget(), toolbar.text);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 92));
  QTest::mouseMove(&editor, QPoint(400, 300), 10);
  application.processEvents();
  if (!snapshotMatches(renderCapture(capture, selection, {clicked, toolbar},
                                     BackgroundStyle::None)) ||
      editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Clicking the toolbar discarded in-progress text");
    return false;
  }
  editor.close();
  return true;
}

bool showsSecretRed(const QColor &color) {
  return color.red() > 180 && color.green() < 70 && color.blue() < 70;
}

/** Redaction sits under every other tool in both preview and export. */
bool runAnnotationLayerChecks(QApplication &application, QString &error) {
  const QColor solid(QStringLiteral("#121216"));
  const QColor secret(QStringLiteral("#ff0000"));
  const QColor backdrop(QStringLiteral("#182030"));

  if (annotationLayer(Annotation::Kind::Redaction) !=
          AnnotationLayer::Redaction ||
      annotationLayer(Annotation::Kind::Arrow) != AnnotationLayer::Default ||
      annotationLayer(Annotation::Kind::Spotlight) !=
          AnnotationLayer::Default ||
      annotationLayer(Annotation::Kind::Text) != AnnotationLayer::Default) {
    error = QStringLiteral("Annotation kinds did not map onto two layers");
    return false;
  }

  // Selection-relative redactions must stay put when the crop is not at the
  // origin. The old applyRedactionsScaled offset subtracted selection.topLeft()
  // and slid the block off the secret.
  QImage scaledBase(100, 80, QImage::Format_ARGB32_Premultiplied);
  scaledBase.fill(secret);
  Annotation scaledRedaction;
  scaledRedaction.kind = Annotation::Kind::Redaction;
  scaledRedaction.start = {10, 10};
  scaledRedaction.end = {40, 40};
  scaledRedaction.redactionStyle = RedactionStyle::Solid;
  const QImage scaled = applyRedactionsScaled(
      scaledBase, {scaledRedaction}, QRectF(200, 150, 100, 80), QSizeF(100, 80));
  if (scaled.pixelColor(20, 20) != solid ||
      scaled.pixelColor(2, 2) != secret) {
    error = QStringLiteral(
        "Scaled redaction layer used capture coordinates instead of "
        "selection-relative ones");
    return false;
  }

  CaptureData capture;
  capture.monitor.scale = 1.0;
  capture.monitor.pixelSize = {80, 40};
  capture.source = QImage(80, 40, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(backdrop);
  for (int y = 10; y < 30; ++y) {
    for (int x = 20; x < 40; ++x)
      capture.source.setPixelColor(x, y, secret);
  }
  capture.previewSize = capture.source.size();

  Annotation redaction;
  redaction.kind = Annotation::Kind::Redaction;
  redaction.start = {20, 10};
  redaction.end = {40, 30};
  redaction.redactionStyle = RedactionStyle::Solid;

  Annotation lens;
  lens.kind = Annotation::Kind::Spotlight;
  lens.start = {16, 6};
  lens.end = {44, 34};
  lens.magnification = 2.0;
  lens.color = Qt::white;
  lens.size = 2;

  const QImage spotlightExport =
      renderCapture(capture, QRectF(0, 0, 80, 40), {redaction, lens},
                    BackgroundStyle::None);
  if (spotlightExport.isNull() ||
      showsSecretRed(spotlightExport.pixelColor(30, 20)) ||
      spotlightExport.pixelColor(30, 20) != solid) {
    error = QStringLiteral("Export spotlight sampled un-redacted source pixels");
    return false;
  }

  Annotation arrow;
  arrow.kind = Annotation::Kind::Arrow;
  arrow.start = {22, 20};
  arrow.end = {38, 20};
  arrow.color = QColor(QStringLiteral("#0a84ff"));
  arrow.size = 4;
  Annotation label;
  label.kind = Annotation::Kind::Text;
  label.start = {24, 26};
  label.text = QStringLiteral("X");
  label.color = Qt::white;
  label.size = 8;
  const QImage redactionOnly =
      renderCapture(capture, QRectF(0, 0, 80, 40), {redaction},
                    BackgroundStyle::None);
  const QImage overlayExport =
      renderCapture(capture, QRectF(0, 0, 80, 40), {redaction, arrow, label},
                    BackgroundStyle::None);
  const QColor overlayFill = overlayExport.pixelColor(22, 12);
  const QColor overlayStroke = overlayExport.pixelColor(26, 20);
  if (overlayExport.isNull() || redactionOnly.isNull() ||
      redactionOnly.pixelColor(22, 12) != solid || showsSecretRed(overlayFill) ||
      overlayStroke.blue() <= overlayStroke.red() + 20) {
    error = QStringLiteral(
        "Export did not keep redaction under arrow and text");
    return false;
  }

  CaptureData editorCapture;
  editorCapture.monitor.name = QStringLiteral("TEST");
  editorCapture.monitor.geometry = {0, 0, 800, 600};
  editorCapture.monitor.pixelSize = {800, 600};
  editorCapture.monitor.scale = 1.0;
  editorCapture.source =
      QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  editorCapture.source.fill(backdrop);
  {
    QPainter painter(&editorCapture.source);
    painter.fillRect(QRect(200, 200, 100, 60), secret);
  }
  editorCapture.previewSize = editorCapture.source.size();

  // Fullscreen draws the 800x600 capture at 84,68 scaled by 0.79. The secret
  // then covers 242,226 to 321,273, centered on 281,250. A loupe smaller than
  // the redaction can only show redacted pixels if it samples the redaction
  // layer.
  const auto spotlightPreviewColor = [&](bool redact) {
    CaptureEditor editor(editorCapture, CaptureEditor::CaptureMode::Fullscreen);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    if (redact) {
      QTest::keyClick(&editor, Qt::Key_D);
      QTest::keyClick(&editor, Qt::Key_D);
      QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(230, 214));
      QTest::mouseMove(&editor, QPoint(333, 285), 20);
      QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(333, 285));
      application.processEvents();
    }
    QTest::keyClick(&editor, Qt::Key_S);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(251, 230));
    QTest::mouseMove(&editor, QPoint(311, 270), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(311, 270));
    application.processEvents();
    const QColor color = editor.grab().toImage().pixelColor(QPoint(281, 250));
    editor.close();
    return color;
  };
  if (!showsSecretRed(spotlightPreviewColor(false))) {
    error = QStringLiteral("Spotlight preview did not magnify the capture");
    return false;
  }
  if (showsSecretRed(spotlightPreviewColor(true))) {
    error = QStringLiteral("Spotlight preview magnified un-redacted pixels");
    return false;
  }

  {
    CaptureEditor editor(editorCapture, CaptureEditor::CaptureMode::Fullscreen);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(230, 214));
    QTest::mouseMove(&editor, QPoint(333, 285), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(333, 285));
    QTest::keyClick(&editor, Qt::Key_5);
    QTest::keyClick(&editor, Qt::Key_A);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(250, 250));
    QTest::mouseMove(&editor, QPoint(312, 250), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(312, 250));
    application.processEvents();
    const QImage preview = editor.grab().toImage();
    const QImage exported = editor.renderCurrentOutput();
    editor.close();
    const QColor arrowColor(QStringLiteral("#0a84ff"));
    if (preview.pixelColor(250, 226) != solid ||
        showsSecretRed(preview.pixelColor(250, 226)) ||
        preview.pixelColor(260, 250) != arrowColor) {
      error = QStringLiteral("Preview did not keep redaction under the arrow");
      return false;
    }
    if (exported.pixelColor(210, 202) != solid ||
        showsSecretRed(exported.pixelColor(210, 202)) ||
        exported.pixelColor(220, 230) != arrowColor) {
      error = QStringLiteral("Export did not keep redaction under the arrow");
      return false;
    }
  }

  {
    CaptureData offsetCapture = editorCapture;
    {
      QPainter painter(&offsetCapture.source);
      painter.fillRect(QRect(450, 350, 80, 60), secret);
    }
    offsetCapture.previewSize = offsetCapture.source.size();
    CaptureEditor editor(offsetCapture);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();

    // Region 200,150 400x300 draws at 200,155 with edit scale 1. The secret
    // is then at 450,355 to 530,415. A preview that subtracts the selection
    // origin lands the block around 250,205 instead.
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
    QTest::mouseMove(&editor, QPoint(600, 450), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(600, 450));
    application.processEvents();
    const QPoint secretCenter(490, 385);
    const QPoint displacedCenter(250, 205);
    if (!showsSecretRed(editor.grab().toImage().pixelColor(secretCenter))) {
      error = QStringLiteral("Editor did not show the secret before redacting");
      return false;
    }

    QTest::keyClick(&editor, Qt::Key_D);
    QTest::keyClick(&editor, Qt::Key_D);
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(450, 355));
    QTest::mouseMove(&editor, QPoint(530, 415), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(530, 415));
    application.processEvents();
    const QImage preview = editor.grab().toImage();
    const QImage exported = editor.renderCurrentOutput();
    editor.close();
    if (showsSecretRed(preview.pixelColor(secretCenter)) ||
        preview.pixelColor(secretCenter) != solid) {
      error = QStringLiteral("Redaction preview left the secret visible");
      return false;
    }
    if (preview.pixelColor(displacedCenter) == solid) {
      error = QStringLiteral(
          "Redaction preview drew the block at the selection offset");
      return false;
    }
    if (exported.size() != QSize(400, 300) ||
        exported.pixelColor(290, 230) != solid ||
        showsSecretRed(exported.pixelColor(290, 230)) ||
        exported.pixelColor(10, 10) != backdrop) {
      error = QStringLiteral(
          "Export redaction did not stay on the off-origin secret");
      return false;
    }
  }
  return true;
}

bool runContinuousAnnotationToolsSmoke(QApplication &application,
                                       QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = capture.source.size();

  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();

  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(700, 500), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(700, 500));
  application.processEvents();

  QTest::keyClick(&editor, Qt::Key_A);
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Arrow tool did not set cross cursor");
    return false;
  }

  for (const auto &[startX, startY, endX, endY] : {
           std::tuple{150, 150, 250, 200},
           std::tuple{300, 150, 400, 200},
           std::tuple{450, 150, 550, 200},
       }) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(startX, startY));
    QTest::mouseMove(&editor, QPoint(endX, endY), 10);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(endX, endY));
    application.processEvents();
    if (editor.cursor().shape() != Qt::CrossCursor) {
      error = QStringLiteral("Arrow tool did not remain active after drawing");
      return false;
    }
  }

  QTest::mouseClick(&editor, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("Right-click did not return to Select tool");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_V);
  const QImage beforeMarquee = editor.grab().toImage();
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(120, 120));
  QTest::mouseMove(&editor, QPoint(560, 260), 20);
  application.processEvents();
  const QImage activeMarquee = editor.grab().toImage();
  bool foundBlueMarqueeEdge = false;
  for (int y = 117; y <= 123 && !foundBlueMarqueeEdge; ++y) {
    for (int x = 336; x <= 344; ++x) {
      const QColor pixel = activeMarquee.pixelColor(x, y);
      if (pixel.blue() > 180 && pixel.green() > 80 && pixel.red() < 80) {
        foundBlueMarqueeEdge = true;
        break;
      }
    }
  }
  if (!foundBlueMarqueeEdge ||
      activeMarquee.pixelColor(340, 130) ==
          beforeMarquee.pixelColor(340, 130)) {
    error = QStringLiteral("Select drag did not paint a visible marquee box");
    return false;
  }
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(560, 260));
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_M);
  application.processEvents();
  if (editor.cursor().shape() != Qt::PointingHandCursor) {
    error = QStringLiteral("Marker tool did not set pointing hand cursor");
    return false;
  }

  for (const auto &[x, y] :
       {std::pair{200, 300}, std::pair{250, 300}, std::pair{300, 300}}) {
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
    application.processEvents();
    if (editor.cursor().shape() != Qt::PointingHandCursor) {
      error = QStringLiteral("Marker tool did not remain active after click");
      return false;
    }
  }

  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("V did not return to Select tool");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_R);
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Rectangle tool did not set cross cursor");
    return false;
  }

  for (const auto &[startX, startY, endX, endY] : {
           std::tuple{150, 350, 250, 420},
           std::tuple{300, 350, 400, 420},
       }) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(startX, startY));
    QTest::mouseMove(&editor, QPoint(endX, endY), 10);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(endX, endY));
    application.processEvents();
    if (editor.cursor().shape() != Qt::CrossCursor) {
      error =
          QStringLiteral("Rectangle tool did not remain active after drawing");
      return false;
    }
  }

  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral("V key did not return to Select tool");
    return false;
  }

  QTest::keyClick(&editor, Qt::Key_T);
  application.processEvents();
  if (editor.cursor().shape() != Qt::IBeamCursor) {
    error = QStringLiteral("Text tool did not set IBeam cursor");
    return false;
  }

  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(200, 450));
  application.processEvents();
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Text 1"));
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
  application.processEvents();
  if (editor.cursor().shape() != Qt::IBeamCursor) {
    error = QStringLiteral(
        "Text tool did not remain active after submitting new text");
    return false;
  }

  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(350, 450));
  application.processEvents();
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Text 2"));
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
  application.processEvents();
  if (editor.cursor().shape() != Qt::IBeamCursor) {
    error = QStringLiteral(
        "Text tool did not remain active after second text commit");
    return false;
  }

  QTest::mouseClick(&editor, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor) {
    error = QStringLiteral(
        "Right-click did not return from Text tool to Select tool");
    return false;
  }

  editor.close();
  return true;
}
bool runAsyncCaptureRegionSmoke(QApplication &application, QString &error) {
  QTemporaryDir commands;
  if (!commands.isValid()) {
    error = QStringLiteral("Could not create async capture command directory");
    return false;
  }

  const auto writeExecutable = [](const QString &path,
                                  const QByteArray &contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
      return false;
    file.close();
    return QFile::setPermissions(
        path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                  QFileDevice::ExeOwner);
  };
  const QString fakeNiri = QDir(commands.path()).filePath(QStringLiteral("niri"));
  const QString fakeGrim = QDir(commands.path()).filePath(QStringLiteral("grim"));
  const QByteArray niriScript =
      QByteArrayLiteral("#!/usr/bin/env bash\n"
                        "case \"$*\" in\n"
                        "  'msg -j focused-output') printf '%s\\n' "
                        "'{\"name\":\"TEST\",\"modes\":[{\"width\":320,"
                        "\"height\":240}],\"current_mode\":0,\"logical\":{"
                        "\"x\":0,\"y\":0,\"width\":320,\"height\":240,"
                        "\"scale\":1,\"transform\":\"Normal\"}}' ;;\n"
                        "  'msg -j windows'|'msg -j workspaces') printf '[]\\n' ;;\n"
                        "  *) exit 64 ;;\n"
                        "esac\n");
  // grim streams the capture on stdout.
  const QByteArray grimScript =
      QByteArrayLiteral("#!/usr/bin/env bash\n"
                        "set -euo pipefail\n"
                        "cat -- \"$NIRISNAP_TEST_PPM\"\n");
  if (!writeExecutable(fakeNiri, niriScript) ||
      !writeExecutable(fakeGrim, grimScript)) {
    error = QStringLiteral("Could not create async capture commands");
    return false;
  }

  QImage source(320, 240, QImage::Format_RGB32);
  source.fill(QColor(QStringLiteral("#28405c")));
  const QString sourcePath =
      QDir(commands.path()).filePath(QStringLiteral("source.ppm"));
  if (!source.save(sourcePath, "PPM")) {
    error = QStringLiteral("Could not create async capture source");
    return false;
  }

  const QByteArray oldPath = qgetenv("PATH");
  const QByteArray oldPpm = qgetenv("NIRISNAP_TEST_PPM");
  qputenv("PATH", commands.path().toUtf8() + ':' + oldPath);
  qputenv("NIRISNAP_TEST_PPM", sourcePath.toUtf8());

  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 320, 240};
  capture.monitor.pixelSize = {320, 240};
  CaptureEditor editor(capture);
  editor.resize(320, 240);
  editor.show();
  application.processEvents();

  bool ready = false;
  QString captureError;
  QObject::connect(&editor, &CaptureEditor::captureReady, &editor,
                   [&ready, &captureError](bool ok, const QString &message) {
                     ready = ok;
                     captureError = message;
                   });
  editor.startCapture(CaptureEditor::CaptureMode::Region, true);

  QElapsedTimer timer;
  timer.start();
  while (!ready && timer.elapsed() < 5000) {
    application.processEvents();
    QThread::msleep(1);
  }
  if (!ready) {
    error = captureError.isEmpty()
                ? QStringLiteral("Async monitor capture did not complete")
                : captureError;
    editor.close();
    qputenv("PATH", oldPath);
    if (oldPpm.isEmpty())
      qunsetenv("NIRISNAP_TEST_PPM");
    else
      qputenv("NIRISNAP_TEST_PPM", oldPpm);
    return false;
  }

  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(40, 30));
  QTest::mouseMove(&editor, QPoint(200, 180), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(200, 180));
  application.processEvents();
  const QImage selected = editor.renderCurrentOutput();
  editor.close();
  qputenv("PATH", oldPath);
  if (oldPpm.isEmpty())
    qunsetenv("NIRISNAP_TEST_PPM");
  else
    qputenv("NIRISNAP_TEST_PPM", oldPpm);

  if (selected.size() != QSize(160, 150)) {
    error = QStringLiteral("Async capture region selection produced %1x%2")
                .arg(selected.width())
                .arg(selected.height());
    return false;
  }
  return true;
}

/** Checks that the wheel retargets the spotlight under the cursor. */
bool runSpotlightWheelSmoke(QApplication &application, QString &error) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.scale = 1.0;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  // Fine banding so a magnification change moves the sampled loupe pixels.
  for (int y = 0; y < capture.source.height(); ++y) {
    for (int x = 0; x < capture.source.width(); ++x) {
      const int band = ((x / 3) + (y / 5)) % 3;
      capture.source.setPixelColor(
          x, y, QColor(40 + band * 70, 60 + band * 50, 90 + band * 40));
    }
  }
  capture.previewSize = capture.source.size();

  const QString snapshotPath = temporarySnapshotPath();
  QFile::remove(snapshotPath);

  const auto armSpotlightTool = [&application](CaptureEditor &editor) {
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
    QTest::mouseMove(&editor, QPoint(700, 500), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(700, 500));
    QTest::keyClick(&editor, Qt::Key_S);
    application.processEvents();
  };
  const auto drawSpotlight = [&application](CaptureEditor &editor) {
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(300, 250));
    QTest::mouseMove(&editor, QPoint(500, 400), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(500, 400));
    application.processEvents();
  };
  const auto sendWheel = [&application](CaptureEditor &editor,
                                        const QPointF &position, int notches) {
    for (int notch = 0; notch < notches; ++notch) {
      QWheelEvent wheel(position, position, {}, {0, 120}, Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&editor, &wheel);
    }
    application.processEvents();
  };
  // Widget centre of the spotlight drawn above, and a point clear of it.
  const QPointF overSpotlight(400, 325);
  const QPointF awayFromSpotlight(160, 160);

  CaptureEditor editor(capture);
  armSpotlightTool(editor);
  drawSpotlight(editor);
  if (editor.cursor().shape() != Qt::CrossCursor) {
    error = QStringLiteral("Spotlight tool did not stay armed after placement");
    return false;
  }
  const QImage placed = flushedSnapshot(editor, snapshotPath);
  if (placed.isNull()) {
    error = QStringLiteral("Spotlight placement did not render a snapshot");
    return false;
  }

  sendWheel(editor, overSpotlight, 4);
  const QImage adjusted = flushedSnapshot(editor, snapshotPath);
  if (adjusted.isNull() || adjusted == placed) {
    error = QStringLiteral(
        "Wheel over a placed spotlight did not change its magnification");
    return false;
  }
  for (int notch = 0; notch < 4; ++notch)
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != placed) {
    error = QStringLiteral("Undo did not restore the spotlight magnification");
    return false;
  }
  for (int notch = 0; notch < 4; ++notch)
    QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != adjusted) {
    error = QStringLiteral("Redo did not restore the adjusted magnification");
    return false;
  }

  sendWheel(editor, awayFromSpotlight, 4);
  if (flushedSnapshot(editor, snapshotPath) != adjusted) {
    error = QStringLiteral(
        "Wheel away from a spotlight changed the placed spotlight");
    return false;
  }
  editor.close();

  // Away from any spotlight the wheel still moves the default that the next
  // spotlight inherits.
  CaptureEditor defaultEditor(capture);
  armSpotlightTool(defaultEditor);
  sendWheel(defaultEditor, awayFromSpotlight, 4);
  drawSpotlight(defaultEditor);
  const QImage inherited = flushedSnapshot(defaultEditor, snapshotPath);
  if (inherited.isNull() || inherited == placed) {
    error = QStringLiteral(
        "Wheel away from any spotlight did not move the default magnification");
    return false;
  }
  defaultEditor.close();
  QFile::remove(snapshotPath);
  return true;
}

bool runWindowWorkerDrainCheck(QApplication &application, QString &error) {
  QTemporaryDir commands;
  if (!commands.isValid()) {
    error = QStringLiteral("Could not create worker-drain test directory");
    return false;
  }
  const QString fakeNiri = QDir(commands.path()).filePath(QStringLiteral("niri"));
  QFile script(fakeNiri);
  const QByteArray contents = QByteArrayLiteral(
      "#!/usr/bin/env bash\n"
      "set -euo pipefail\n"
      "path=''\n"
      "while (($#)); do case \"$1\" in --path) path=$2; shift 2;; *) shift;; esac; done\n"
      "[[ $path = /* ]] || exit 2\n"
      "printf '%s' \"$path\" > \"$NIRISNAP_TEST_EDITOR_CAPTURE_PATH\"\n"
      "(sleep 0.30; cp -- \"$NIRISNAP_TEST_EDITOR_PNG\" \"$path\") >/dev/null 2>&1 &\n");
  if (!script.open(QIODevice::WriteOnly) ||
      script.write(contents) != contents.size()) {
    error = QStringLiteral("Could not write worker-drain fake Niri");
    return false;
  }
  script.close();
  if (!QFile::setPermissions(fakeNiri, QFileDevice::ReadOwner |
                                           QFileDevice::WriteOwner |
                                           QFileDevice::ExeOwner)) {
    error = QStringLiteral("Could not make worker-drain fake Niri executable");
    return false;
  }

  QImage png(4, 3, QImage::Format_RGB32);
  png.fill(Qt::green);
  const QString pngPath = QDir(commands.path()).filePath(QStringLiteral("window.png"));
  const QString capturePath =
      QDir(commands.path()).filePath(QStringLiteral("capture-path"));
  if (!png.save(pngPath, "PNG")) {
    error = QStringLiteral("Could not write worker-drain PNG");
    return false;
  }

  const QByteArray originalPath = qgetenv("PATH");
  qputenv("PATH", commands.path().toUtf8() + ':' + originalPath);
  qputenv("NIRISNAP_TEST_EDITOR_PNG", pngPath.toUtf8());
  qputenv("NIRISNAP_TEST_EDITOR_CAPTURE_PATH", capturePath.toUtf8());
  std::atomic_bool termination{false};
  CaptureData capture;
  capture.monitor.geometry = QRect(0, 0, 100, 100);
  capture.monitor.pixelSize = QSize(100, 100);
  capture.monitor.scale = 1.0;
  capture.source = QImage(100, 100, QImage::Format_RGB32);
  capture.source.fill(Qt::black);
  capture.previewSize = capture.source.size();
  capture.windows = {{{10, 10, 60, 60}, QStringLiteral("7"),
                      QStringLiteral("worker")}};

  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::Window,
                         QuickOutputMode::None, &termination);
    editor.resize(100, 100);
    editor.show();
    application.processEvents();
    QTest::mouseMove(&editor, QPoint(30, 30), 20);
    QTest::keyClick(&editor, Qt::Key_Return);
    QElapsedTimer timer;
    timer.start();
    while (!QFile::exists(capturePath) && timer.elapsed() < 1000) {
      application.processEvents(QEventLoop::ExcludeUserInputEvents);
      QThread::msleep(5);
    }
    if (!QFile::exists(capturePath)) {
      error = QStringLiteral("Overlay clean-window worker did not start");
      qputenv("PATH", originalPath);
      qunsetenv("NIRISNAP_TEST_EDITOR_PNG");
      qunsetenv("NIRISNAP_TEST_EDITOR_CAPTURE_PATH");
      return false;
    }
    termination.store(true, std::memory_order_release);
  }

  QFile pathFile(capturePath);
  if (!pathFile.open(QIODevice::ReadOnly)) {
    error = QStringLiteral("Could not read overlay worker target path");
    qputenv("PATH", originalPath);
    return false;
  }
  const QString target = QString::fromUtf8(pathFile.readAll());
  QThread::msleep(400);
  QDir runtime(QFileInfo(target).absolutePath());
  runtime.cdUp();
  const bool remains = QFile::exists(target) || QFileInfo(target).dir().exists() ||
                       !runtime.entryList({QStringLiteral("window-*"),
                                           QStringLiteral(".delete-*")},
                                          QDir::Dirs | QDir::Hidden |
                                              QDir::NoDotAndDotDot)
                            .isEmpty();
  qputenv("PATH", originalPath);
  qunsetenv("NIRISNAP_TEST_EDITOR_PNG");
  qunsetenv("NIRISNAP_TEST_EDITOR_CAPTURE_PATH");
  if (remains) {
    error = QStringLiteral("CaptureEditor destruction did not drain native worker cleanup");
    return false;
  }
  return true;
}
} // namespace
/** Runs the interaction and rendering smoke checks. */
int main(int argc, char **argv) {
  // Re-executed by the instance-lock checks as the process holding the lock.
  const QString heldLockPath =
      qEnvironmentVariable(kInstanceLockHolderVariable);
  if (!heldLockPath.isEmpty())
    return runInstanceLockHolder(heldLockPath);

  QApplication application(argc, argv);
  if (!loadCaptureFonts())
    return 17;
  // Exercise production Open dispatch, then exit without an event-loop turn.
  if (application.arguments().contains(QStringLiteral("--open-app-exit-helper"))) {
    CaptureData synthetic;
    synthetic.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
    synthetic.source.fill(Qt::green);
    synthetic.previewSize = synthetic.source.size();
    synthetic.monitor.geometry = QRect(0, 0, 800, 600);
    synthetic.monitor.pixelSize = synthetic.source.size();
    synthetic.monitor.scale = 1.0;
    CaptureEditor helper(synthetic, CaptureEditor::CaptureMode::File);
    helper.resize(1000, 700);
    helper.show();
    application.processEvents();
    QTest::mouseClick(&helper, Qt::LeftButton, Qt::NoModifier,
                      QPoint(60 + 764 + 18, 28));
    return helper.isVisible() ? 118 : 0;
  }
  // Never send smoke notifications to the user's live desktop.
  QTemporaryDir smokeCommands;
  if (!smokeCommands.isValid())
    return 119;
  QFile notifyStub(
      QDir(smokeCommands.path()).filePath(QStringLiteral("notify-send")));
  if (!notifyStub.open(QIODevice::WriteOnly) ||
      notifyStub.write("#!/bin/sh\nexit 0\n") < 0 ||
      !notifyStub.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                 QFile::ExeOwner))
    return 119;
  notifyStub.close();
  const QByteArray smokePath = qgetenv("PATH");
  qputenv("PATH", smokeCommands.path().toUtf8() + ':' + smokePath);
  const auto restoreSmokePath =
      qScopeGuard([smokePath] { qputenv("PATH", smokePath); });
  QString snapshotError;
  if (!runBackdropParityChecks(snapshotError)) {
    qCritical().noquote() << snapshotError;
    return 124;
  }
  if (!runCutoutAndEscapeChecks(snapshotError)) {
    qCritical().noquote() << snapshotError;
    return 100;
  }
  if (!runSelectedLayerRecoveryChecks(snapshotError)) {
    qCritical().noquote() << snapshotError;
    return 120;
  }
  if (!runLaunchModifierChecks(snapshotError)) {
    qCritical().noquote() << snapshotError;
    return 121;
  }
  if (!runEyedropperStateChecks(snapshotError)) {
    qCritical().noquote() << snapshotError;
    return 122;
  }
  if (!runStableCropDragChecks(snapshotError)) {
    qCritical().noquote() << snapshotError;
    return 123;
  }
  if (!runPositionalImageTargetCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 70;
  }
  if (!runTemporarySnapshotChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 68;
  }
  if (!runHighlighterRenderingCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 69;
  }
  if (!runSecureRedactionChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 71;
  }
  if (!runBackdropCacheRenderingCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 88;
  }
  if (!runCreationConstraintCheck(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 90;
  }
  if (!runQuickOutputChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 73;
  }
  if (!runAsyncCaptureRegionSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 82;
  }
  if (!runPinLayoutSmoke(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 77;
  }
  if (!runPinLifecycleSmoke(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 78;
  }
  if (!runSpotlightAndSampleChecks(snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 79;
  }
  if (!runTextClickAwayCommitCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 87;
  }
  if (!runAnnotationLayerChecks(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 86;
  }
  if (!runContinuousAnnotationToolsSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 80;
  }
  if (!runSpotlightWheelSmoke(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 94;
  }
  if (!runWindowWorkerDrainCheck(application, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 95;
  }
  const QString outputRoot =
      argc > 1 ? QString::fromLocal8Bit(argv[1])
               : QDir(QDir::tempPath())
                     .filePath(QStringLiteral("nirisnap-native-smoke"));
  const QString snapshotPath = temporarySnapshotPath();
  QFile::remove(snapshotPath);
  const QString savedRoot = QDir(outputRoot).filePath(QStringLiteral("saved"));
  QDir(savedRoot).removeRecursively();
  qputenv("NIRISNAP_SCREENSHOT_DIR", savedRoot.toUtf8());

  const QString nativeStableId =
      qEnvironmentVariable("NIRISNAP_SMOKE_NATIVE_STABLE_ID");
  if (!nativeStableId.isEmpty()) {
    QImage nativeSurface;
    QString nativeError;
    if (captureWindowSurface(
            {{}, nativeStableId, QStringLiteral("native smoke")}, nativeSurface,
            nativeError) != CaptureOutcome::Success) {
      qWarning().noquote() << nativeError;
      return 10;
    }
    if (!nativeSurface.save(outputRoot + QStringLiteral("-native-window.png"),
                            "PNG"))
      return 11;
    return 0;
  }

  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = {0, 0, 800, 600};
  capture.monitor.pixelSize = {800, 600};
  capture.monitor.workspaceId = 42;
  capture.source = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
  {
    QPainter painter(&capture.source);
    QLinearGradient gradient(0, 0, 800, 600);
    gradient.setColorAt(0, QColor(QStringLiteral("#172033")));
    gradient.setColorAt(1, QColor(QStringLiteral("#5278b5")));
    painter.fillRect(capture.source.rect(), gradient);
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Noto Sans"), 34, QFont::Bold));
    painter.drawText(capture.source.rect(), Qt::AlignCenter,
                     QStringLiteral("Native Qt capture editor"));
  }
  capture.previewSize = capture.source.size();
  capture.windows = {
      {{80, 80, 300, 220}, QStringLiteral("1"), QStringLiteral("first")},
      {{420, 120, 300, 320}, QStringLiteral("2"), QStringLiteral("second")}};

  if (!runCrashSnapshotChecks(capture, snapshotError)) {
    qWarning().noquote() << snapshotError;
    return 99;
  }

  {
    CaptureEditor constraintEditor(capture);
    constraintEditor.resize(800, 600);
    constraintEditor.show();
    application.processEvents();
    QTest::mousePress(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&constraintEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    application.processEvents();

    const auto expectedRectangle = [&](const QPointF &end) {
      Annotation rectangle;
      rectangle.kind = Annotation::Kind::Rectangle;
      rectangle.start = {175, 100};
      rectangle.end = end;
      rectangle.color = QColor(QStringLiteral("#ff375f"));
      rectangle.size = 4;
      return renderCapture(capture, QRectF(100, 100, 550, 370), {rectangle},
                           BackgroundStyle::None);
    };
    const auto dragRectangle = [&](bool releaseShiftBeforeMouse) {
      QTest::keyClick(&constraintEditor, Qt::Key_R);
      QTest::mousePress(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(300, 220));
      QTest::mouseMove(&constraintEditor, QPoint(420, 280), 20);
      QTest::keyPress(&constraintEditor, Qt::Key_Shift);
      application.processEvents();
      if (releaseShiftBeforeMouse)
        QTest::keyRelease(&constraintEditor, Qt::Key_Shift);
      QTest::mouseRelease(&constraintEditor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(420, 280));
      if (!releaseShiftBeforeMouse)
        QTest::keyRelease(&constraintEditor, Qt::Key_Shift);
      application.processEvents();
    };

    dragRectangle(true);
    const QImage freeExpected = expectedRectangle(QPointF(295, 160));
    if (flushedSnapshot(constraintEditor, snapshotPath).convertToFormat(freeExpected.format()) !=
        freeExpected)
      return 91;
    QTest::keyClick(&constraintEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();

    dragRectangle(false);
    const QImage constrainedExpected = expectedRectangle(QPointF(295, 220));
    if (flushedSnapshot(constraintEditor, snapshotPath).convertToFormat(constrainedExpected.format()) !=
        constrainedExpected)
      return 92;
    constraintEditor.close();
  }

  {
    CaptureEditor quickEditor(capture, CaptureEditor::CaptureMode::Region,
                              QuickOutputMode::Save);
    quickEditor.resize(800, 600);
    quickEditor.show();
    application.processEvents();
    QTest::mousePress(&quickEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&quickEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&quickEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    application.processEvents();
    const QStringList files =
        QDir(savedRoot).entryList({QStringLiteral("*.png")}, QDir::Files);
    if (quickEditor.isVisible() || files.size() != 1 ||
        QImage(QDir(savedRoot).filePath(files.constFirst())).isNull())
      return 74;
  }
  QDir(savedRoot).removeRecursively();

  {
    CaptureData cropCapture;
    cropCapture.monitor.geometry = {0, 0, 800, 600};
    cropCapture.monitor.pixelSize = {64, 64};
    cropCapture.source = QImage(64, 64, QImage::Format_ARGB32_Premultiplied);
    cropCapture.source.fill(QColor(QStringLiteral("#112233")));
    cropCapture.previewSize = cropCapture.source.size();
    CaptureEditor cropEditor(cropCapture, CaptureEditor::CaptureMode::File);
    cropEditor.resize(800, 600);
    cropEditor.show();
    application.processEvents();
    const QRectF available(30, 68, 740, 474);
    const qreal scale = std::min<qreal>(
        {1.0, available.width() / 64.0, available.height() / 64.0});
    const QSizeF shown(64.0 * scale, 64.0 * scale);
    const QRectF image(available.center().x() - shown.width() / 2.0,
                       available.center().y() - shown.height() / 2.0,
                       shown.width(), shown.height());
    const QPoint leftHandle(qRound(image.left() - 7.0),
                            qRound(image.center().y()));
    QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier, leftHandle);
    QTest::mouseMove(&cropEditor, QPoint(0, leftHandle.y()), 20);
    QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(0, leftHandle.y()));
    application.processEvents();
    const QImage cropped = flushedSnapshot(cropEditor, snapshotPath);
    if (cropped.isNull() || cropped.width() < 16 || cropped.width() > 64 ||
        cropped.height() != 64)
      return 93;
    cropEditor.close();
    QFile::remove(snapshotPath);
  }


  CaptureEditor editor(capture);
  editor.resize(800, 600);
  editor.show();
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Space);
  QTest::mouseMove(&editor, QPoint(200, 160), 20);
  application.processEvents();
  const QImage hoverUi = editor.grab().toImage();
  if (hoverUi.pixelColor(200, 160) != capture.source.pixelColor(200, 160))
    return 7;
  QTest::keyClick(&editor, Qt::Key_Right, Qt::MetaModifier);
  application.processEvents();
  const QImage keyboardWindowUi = editor.grab().toImage();
  if (keyboardWindowUi.pixelColor(500, 200) !=
          capture.source.pixelColor(500, 200) ||
      keyboardWindowUi.pixelColor(200, 160) ==
          capture.source.pixelColor(200, 160))
    return 8;
  QTest::keyClick(&editor, Qt::Key_Space);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
  QTest::mouseMove(&editor, QPoint(650, 470), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 470));
  application.processEvents();
  const QImage initialSnapshot = flushedSnapshot(editor, snapshotPath);
  if (initialSnapshot.isNull())
    return 37;
  if (editor.cursor().shape() != Qt::ArrowCursor ||
      !editor.grab().save(outputRoot + QStringLiteral("-selector-default.png"),
                          "PNG"))
    return 29;
  const QImage editBackdropUi = editor.grab().toImage();
  const QColor backdropCorner = editBackdropUi.pixelColor(5, 5);
  if (backdropCorner.alpha() != 160 || backdropCorner.red() != 0 ||
      backdropCorner.green() != 0 || backdropCorner.blue() != 0)
    return 9;
  QTest::keyClick(&editor, Qt::Key_A);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(230, 250));
  QTest::mouseMove(&editor, QPoint(570, 350), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(570, 350));
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor)
    return 26;
  const QImage arrowSnapshot = flushedSnapshot(editor, snapshotPath);
  if (arrowSnapshot.isNull() || arrowSnapshot == initialSnapshot)
    return 38;
  QTest::mouseClick(&editor, Qt::RightButton, Qt::NoModifier, QPoint(100, 100));
  application.processEvents();
  if (editor.cursor().shape() != Qt::ArrowCursor)
    return 26;
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(420, 315), 20);
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(420, 315));
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != initialSnapshot)
    return 56;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != arrowSnapshot)
    return 57;
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(140, 140));
  application.processEvents();
  QTest::keyClick(&editor, Qt::Key_Delete);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != arrowSnapshot)
    return 39;
  QTest::keyClick(&editor, Qt::Key_L);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(260, 210));
  QTest::mouseMove(&editor, QPoint(520, 265), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(520, 265));
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor)
    return 27;
  const QImage lineSnapshot = flushedSnapshot(editor, snapshotPath);
  if (lineSnapshot.isNull() || lineSnapshot == arrowSnapshot)
    return 40;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != arrowSnapshot)
    return 41;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != lineSnapshot)
    return 42;

  QTest::keyClick(&editor, Qt::Key_V);
  application.processEvents();
  const QImage lineUnselectedUi = editor.grab().toImage();
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(390, 238));
  application.processEvents();
  const QImage lineSelectedUi = editor.grab().toImage();
  if (lineSelectedUi.pixelColor(260, 210) !=
          QColor(QStringLiteral("#0a84ff")) ||
      lineSelectedUi.pixelColor(520, 265) != QColor(QStringLiteral("#0a84ff")))
    return 43;
  for (int x = 280; x <= 500; ++x) {
    if (lineSelectedUi.pixelColor(x, 206) !=
        lineUnselectedUi.pixelColor(x, 206))
      return 44;
  }

  const QImage beforeSelectorZoom =
      editor.grab().toImage().copy(QRect(100, 150, 600, 350));
  QWheelEvent selectorZoom(QPointF(520, 265), QPointF(520, 265), {}, {0, 120},
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                           false);
  QApplication::sendEvent(&editor, &selectorZoom);
  application.processEvents();
  const QImage afterSelectorZoom =
      editor.grab().toImage().copy(QRect(100, 150, 600, 350));
  if (beforeSelectorZoom == afterSelectorZoom)
    return 30;
  const QImage scaledLineSnapshot = flushedSnapshot(editor, snapshotPath);
  if (scaledLineSnapshot.isNull() || scaledLineSnapshot == lineSnapshot)
    return 45;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != lineSnapshot)
    return 46;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != scaledLineSnapshot)
    return 47;
  const QImage beforeFreehandSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_F);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(270, 390));
  QTest::mouseMove(&editor, QPoint(330, 360), 10);
  QTest::mouseMove(&editor, QPoint(390, 410), 10);
  QTest::mouseMove(&editor, QPoint(460, 370), 10);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(530, 420));
  application.processEvents();
  if (editor.cursor().shape() != Qt::CrossCursor ||
      flushedSnapshot(editor, snapshotPath) == beforeFreehandSnapshot)
    return 28;
  const QImage beforeMarkerSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_2);
  QTest::keyClick(&editor, Qt::Key_C);
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(470, 300));
  application.processEvents();
  if (editor.cursor().shape() != Qt::PointingHandCursor ||
      flushedSnapshot(editor, snapshotPath) == beforeMarkerSnapshot)
    return 48;
  const QImage beforeTextSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_T);
  // Text size panel under the Text toolbar button (shifted by Highlighter).
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(327, 133));
  QWheelEvent textSizeWheel(QPointF(360, 320), QPointF(360, 320), {}, {0, -120},
                            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                            false);
  QApplication::sendEvent(&editor, &textSizeWheel);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-text-sizes.png"),
                          "PNG"))
    return 18;
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(360, 320));
  QTest::keyClicks(QApplication::focusWidget(), QStringLiteral("Inline text"));
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-text-inline.png"),
                          "PNG"))
    return 19;
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-text-committed.png"),
                          "PNG") ||
      flushedSnapshot(editor, snapshotPath) == beforeTextSnapshot)
    return 20;
  const QImage beforeMoveSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_V);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(400, 300));
  QTest::mouseMove(&editor, QPoint(420, 315), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(420, 315));
  application.processEvents();
  const QImage movedSnapshot = flushedSnapshot(editor, snapshotPath);
  if (movedSnapshot.isNull() || movedSnapshot == beforeMoveSnapshot)
    return 49;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != beforeMoveSnapshot)
    return 50;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != movedSnapshot)
    return 51;
  const QImage beforeEndpointSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(590, 365));
  QTest::mouseMove(&editor, QPoint(620, 380), 20);
  QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(620, 380));
  application.processEvents();
  const QImage resizedSnapshot = flushedSnapshot(editor, snapshotPath);
  if (resizedSnapshot.isNull() || resizedSnapshot == beforeEndpointSnapshot)
    return 52;
  QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != beforeEndpointSnapshot)
    return 53;
  QTest::keyClick(&editor, Qt::Key_Y, Qt::ControlModifier);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) != resizedSnapshot)
    return 54;
  if (!editor.grab().save(outputRoot + QStringLiteral("-vector-selected.png"),
                          "PNG"))
    return 21;
  QTest::mouseDClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(380, 330));
  if (QApplication::focusWidget() != nullptr &&
      QApplication::focusWidget() != &editor) {
    QTest::keyClicks(QApplication::focusWidget(),
                     QStringLiteral("Edited Neucha"));
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Return);
  } else {
    return 22;
  }
  const QImage beforeBackdropSnapshot = flushedSnapshot(editor, snapshotPath);
  QTest::keyClick(&editor, Qt::Key_B);
  application.processEvents();
  if (flushedSnapshot(editor, snapshotPath) == beforeBackdropSnapshot)
    return 55;
  // Framed 678×498 crop fits available height; toolbar stays above frame.
  QTest::mouseMove(&editor, QPoint(398, 42), 20);
  application.processEvents();
  if (!editor.grab().save(outputRoot + QStringLiteral("-palette.png"), "PNG"))
    return 14;
  // Custom color control in the open palette strip.
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(470, 82));
  QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(320, 200));
  // Freehand toolbar button.
  QTest::mouseMove(&editor, QPoint(198, 42), 20);
  application.processEvents();
  if (editor.cursor().shape() != Qt::PointingHandCursor)
    return 12;
  if (!editor.grab().save(outputRoot + QStringLiteral("-ui.png"), "PNG") ||
      !hoverUi.save(outputRoot + QStringLiteral("-window-hover.png"), "PNG") ||
      !keyboardWindowUi.save(
          outputRoot + QStringLiteral("-window-keyboard.png"), "PNG"))
    return 2;

  {
    CaptureEditor redactionEditor(capture);
    redactionEditor.resize(800, 600);
    redactionEditor.show();
    application.processEvents();
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&redactionEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    const QImage beforeRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    const QImage beforeRedactionUi = redactionEditor.grab().toImage();
    QTest::keyClick(&redactionEditor, Qt::Key_D);
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(420, 200), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 200));
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != beforeRedaction)
      return 72;
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(420, 260), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 260));
    application.processEvents();
    const QImage pixelatedRedactionUi = redactionEditor.grab().toImage();
    if (pixelatedRedactionUi == beforeRedactionUi)
      return 83;
    const QImage pixelatedRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    if (pixelatedRedaction.isNull() || pixelatedRedaction == beforeRedaction)
      return 73;

    QTest::keyClick(&redactionEditor, Qt::Key_V);
    QTest::mouseClick(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(360, 230));
    QTest::keyClick(&redactionEditor, Qt::Key_D);
    application.processEvents();
    const QImage solidRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    if (solidRedaction.isNull() || solidRedaction == pixelatedRedaction)
      return 74;
    QTest::keyClick(&redactionEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != pixelatedRedaction)
      return 75;
    QTest::keyClick(&redactionEditor, Qt::Key_Y, Qt::ControlModifier);
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != solidRedaction)
      return 76;

    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(360, 230));
    QTest::mouseMove(&redactionEditor, QPoint(380, 245), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(380, 245));
    application.processEvents();
    const QImage movedRedaction = flushedSnapshot(redactionEditor, snapshotPath);
    if (movedRedaction.isNull() || movedRedaction == solidRedaction)
      return 77;
    QTest::keyClick(&redactionEditor, Qt::Key_Z, Qt::ControlModifier);
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) != solidRedaction)
      return 78;

    QTest::keyClick(&redactionEditor, Qt::Key_D);
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    QTest::mouseMove(&redactionEditor, QPoint(280, 190), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(280, 190));
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) == solidRedaction ||
        !redactionEditor.grab().save(
            outputRoot + QStringLiteral("-secure-redaction.png"), "PNG"))
      return 79;
    QTest::mousePress(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(280, 190));
    QTest::mouseMove(&redactionEditor, QPoint(420, 190), 20);
    QTest::mouseRelease(&redactionEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(420, 190));
    application.processEvents();
    if (flushedSnapshot(redactionEditor, snapshotPath) == beforeRedaction)
      return 81;
  }

  {
    CaptureEditor overlapEditor(capture);
    overlapEditor.resize(800, 600);
    overlapEditor.show();
    application.processEvents();
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&overlapEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    QTest::keyClick(&overlapEditor, Qt::Key_L);
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 300));
    QTest::mouseMove(&overlapEditor, QPoint(500, 300), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(500, 300));
    QTest::keyClick(&overlapEditor, Qt::Key_D);
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(350, 270));
    QTest::mouseMove(&overlapEditor, QPoint(450, 330), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(450, 330));
    QTest::keyClick(&overlapEditor, Qt::Key_V);
    QTest::mouseClick(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 300));
    application.processEvents();
    const QImage overlapUi = overlapEditor.grab().toImage();
    if (overlapUi.pixelColor(300, 300) != QColor(QStringLiteral("#0a84ff")) ||
        overlapUi.pixelColor(500, 300) != QColor(QStringLiteral("#0a84ff")) ||
        !overlapUi.save(outputRoot + QStringLiteral("-redaction-overlap.png"),
                        "PNG"))
      return 80;

    QTest::keyClick(&overlapEditor, Qt::Key_R);
    QTest::mousePress(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(330, 250));
    QTest::mouseMove(&overlapEditor, QPoint(470, 350), 20);
    QTest::mouseRelease(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(470, 350));
    QTest::keyClick(&overlapEditor, Qt::Key_V);
    QTest::mouseClick(&overlapEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(400, 285));
    application.processEvents();
    const QImage enclosedRedactionUi = overlapEditor.grab().toImage();
    if (enclosedRedactionUi.pixelColor(350, 270) !=
            QColor(QStringLiteral("#0a84ff")) ||
        enclosedRedactionUi.pixelColor(450, 330) !=
            QColor(QStringLiteral("#0a84ff")))
      return 84;
  }

  CaptureEditor fullscreenEditor(capture,
                                 CaptureEditor::CaptureMode::Fullscreen);
  fullscreenEditor.resize(800, 600);
  fullscreenEditor.show();
  application.processEvents();
  CaptureEditor compactToolbarEditor(capture,
                                     CaptureEditor::CaptureMode::Fullscreen);
  compactToolbarEditor.resize(720, 600);
  compactToolbarEditor.show();
  application.processEvents();
  QTest::mouseMove(&compactToolbarEditor, QPoint(20, 45), 20);
  application.processEvents();
  QTest::mouseMove(&compactToolbarEditor, QPoint(700, 45), 20);
  application.processEvents();
  const QImage compactToolbarUi = compactToolbarEditor.grab().toImage();
  if (compactToolbarUi.pixelColor(20, 45).alpha() < 240 ||
      compactToolbarUi.pixelColor(700, 45).alpha() < 240 ||
      !compactToolbarUi.save(
          outputRoot + QStringLiteral("-compact-toolbar.png"), "PNG"))
    return 83;
  CaptureEditor windowModeEditor(capture, CaptureEditor::CaptureMode::Window);
  windowModeEditor.resize(800, 600);
  windowModeEditor.show();
  application.processEvents();
  if (!windowModeEditor.grab().save(
          outputRoot + QStringLiteral("-window-mode.png"), "PNG"))
    return 36;

  CaptureData failedWindowCapture = capture;
  failedWindowCapture.windows = {{{80, 80, 300, 220},
                                  QStringLiteral("missing-stable-id"),
                                  QStringLiteral("missing")}};
  CaptureEditor failedWindowEditor(failedWindowCapture,
                                   CaptureEditor::CaptureMode::Window);
  failedWindowEditor.resize(800, 600);
  failedWindowEditor.show();
  QTest::mouseMove(&failedWindowEditor, QPoint(200, 160), 20);
  QTest::keyClick(&failedWindowEditor, Qt::Key_Return);
  application.processEvents();
  if (failedWindowEditor.cursor().shape() != Qt::PointingHandCursor ||
      !failedWindowEditor.grab().save(
          outputRoot + QStringLiteral("-window-capture-failed.png"), "PNG"))
    return 63;

  CaptureData nativePreviewCapture = capture;
  nativePreviewCapture.monitor.scale = 2.0;
  nativePreviewCapture.monitor.pixelSize = {1600, 1200};
  nativePreviewCapture.source = QImage(1600, 1200, QImage::Format_RGB32);
  nativePreviewCapture.source.fill(QColor(QStringLiteral("#123456")));
  nativePreviewCapture.previewSize = QSize(800, 600);
  CaptureEditor nativePreviewEditor(nativePreviewCapture,
                                    CaptureEditor::CaptureMode::Fullscreen);
  nativePreviewEditor.resize(800, 600);
  nativePreviewEditor.show();
  application.processEvents();
  const QImage nativePreviewUi = nativePreviewEditor.grab().toImage();
  if (nativePreviewUi.pixelColor(400, 300) !=
          QColor(QStringLiteral("#123456")) ||
      !nativePreviewUi.save(outputRoot + QStringLiteral("-native-preview.png"),
                            "PNG"))
    return 23;
  if (!fullscreenEditor.grab().save(
          outputRoot + QStringLiteral("-fullscreen-editor.png"), "PNG"))
    return 16;

  // File mode opens an existing image straight into the edit phase: whole
  // image selected, arrow cursor, annotation canvas showing the pixels.
  CaptureData fileData;
  fileData.monitor.scale = 1.0;
  fileData.monitor.pixelSize = {300, 200};
  fileData.source = QImage(300, 200, QImage::Format_RGB32);
  fileData.source.fill(QColor(QStringLiteral("#1a2b3c")));
  fileData.previewSize = fileData.source.size();
  CaptureEditor fileEditor(fileData, CaptureEditor::CaptureMode::File);
  fileEditor.resize(800, 600);
  fileEditor.show();
  application.processEvents();
  const QImage fileUi = fileEditor.grab().toImage();
  if (fileEditor.cursor().shape() != Qt::ArrowCursor ||
      fileUi.pixelColor(400, 305) != QColor(QStringLiteral("#1a2b3c")) ||
      !fileUi.save(outputRoot + QStringLiteral("-file-mode.png"), "PNG"))
    return 70;

  CaptureData windowSurfaceCapture = capture;
  windowSurfaceCapture.monitor.scale = 1.0;
  windowSurfaceCapture.monitor.pixelSize = {320, 180};
  windowSurfaceCapture.source =
      QImage(320, 180, QImage::Format_ARGB32_Premultiplied);
  windowSurfaceCapture.source.fill(QColor(QStringLiteral("#d12f45")));
  windowSurfaceCapture.previewSize = windowSurfaceCapture.source.size();
  CaptureEditor windowSurfaceEditor(windowSurfaceCapture,
                                    CaptureEditor::CaptureMode::Fullscreen);
  windowSurfaceEditor.resize(800, 600);
  windowSurfaceEditor.show();
  application.processEvents();
  const QImage windowSurfaceUi = windowSurfaceEditor.grab().toImage();
  const QColor windowSurfaceCorner = windowSurfaceUi.pixelColor(5, 5);
  if (windowSurfaceCorner.alpha() != 160 || windowSurfaceCorner.red() != 0 ||
      windowSurfaceCorner.green() != 0 || windowSurfaceCorner.blue() != 0 ||
      windowSurfaceUi.pixelColor(400, 300) !=
          QColor(QStringLiteral("#d12f45")) ||
      !windowSurfaceUi.save(
          outputRoot + QStringLiteral("-window-surface-editor.png"), "PNG"))
    return 62;

  CaptureEditor cropEditor(capture);
  cropEditor.resize(800, 600);
  cropEditor.show();
  application.processEvents();
  QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(100, 100));
  QTest::mouseMove(&cropEditor, QPoint(650, 470), 20);
  QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 470));
  application.processEvents();
  QTest::keyClick(&cropEditor, Qt::Key_L);
  QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(260, 210));
  QTest::mouseMove(&cropEditor, QPoint(520, 265), 20);
  QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(520, 265));
  QTest::keyClick(&cropEditor, Qt::Key_V);
  QTest::mouseClick(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(390, 238));
  application.processEvents();
  const QImage beforeCrop = cropEditor.grab().toImage();
  if (!beforeCrop.save(outputRoot + QStringLiteral("-crop-handles.png"), "PNG"))
    return 31;
  QTest::mouseMove(&cropEditor, QPoint(682, 497), 20);
  QTest::mousePress(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(682, 497));
  QTest::mouseMove(&cropEditor, QPoint(620, 440), 20);
  QTest::mouseRelease(&cropEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(620, 440));
  application.processEvents();
  const QImage afterCrop = cropEditor.grab().toImage();
  if (beforeCrop == afterCrop ||
      !afterCrop.save(outputRoot + QStringLiteral("-cropped.png"), "PNG"))
    return 32;
  QTest::keyClick(&cropEditor, Qt::Key_Z, Qt::ControlModifier);
  application.processEvents();
  if (cropEditor.grab().toImage().pixelColor(260, 210) !=
      QColor(QStringLiteral("#0a84ff")))
    return 58;

  CaptureEditor previewClipEditor(capture);
  previewClipEditor.resize(800, 600);
  previewClipEditor.show();
  application.processEvents();
  QTest::mousePress(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(100, 100));
  QTest::mouseMove(&previewClipEditor, QPoint(650, 470), 20);
  QTest::mouseRelease(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(650, 470));
  QTest::keyClick(&previewClipEditor, Qt::Key_C);
  QTest::mouseClick(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(640, 300));
  QTest::keyClick(&previewClipEditor, Qt::Key_B);
  QTest::mousePress(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                    QPoint(669, 305)); // Framed crop scale = 474/498.
  QTest::mouseMove(&previewClipEditor, QPoint(500, 305), 20);
  QTest::mouseRelease(&previewClipEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(500, 305));
  application.processEvents();
  const QImage clippedPreview = previewClipEditor.grab().toImage();
  for (int y = 270; y <= 330; ++y) {
    for (int x = 690; x <= 760; ++x) {
      const QColor pixel = clippedPreview.pixelColor(x, y);
      if (pixel.red() > 240 && pixel.green() < 96 && pixel.blue() < 128)
        return 66;
    }
  }

  QVector<Annotation> annotations;
  annotations.push_back({Annotation::Kind::Arrow,
                         {50, 60},
                         {390, 150},
                         {},
                         QColor(QStringLiteral("#ff375f")),
                         5,
                         0,
                         {}});
  annotations.push_back({Annotation::Kind::Line,
                         {40, 260},
                         {430, 250},
                         {},
                         QColor(QStringLiteral("#bf5af2")),
                         4,
                         0,
                         {}});
  Annotation freehand;
  freehand.kind = Annotation::Kind::Freehand;
  freehand.color = QColor(QStringLiteral("#ffd60a"));
  freehand.size = 4;
  freehand.points = {{40, 170}, {110, 145}, {165, 185}, {225, 150}};
  annotations.push_back(std::move(freehand));
  Annotation highlighter;
  highlighter.kind = Annotation::Kind::Highlighter;
  highlighter.color = QColor(QStringLiteral("#ffd60a"));
  highlighter.size = 5;
  highlighter.points = {{50, 210}, {140, 205}, {220, 215}, {300, 200}};
  annotations.push_back(std::move(highlighter));
  annotations.push_back({Annotation::Kind::Marker,
                         {260, 180},
                         {},
                         {},
                         QColor(QStringLiteral("#ff9f0a")),
                         5,
                         1,
                         {}});
  annotations.push_back({Annotation::Kind::Rectangle,
                         {30, 30},
                         {450, 230},
                         {},
                         QColor(QStringLiteral("#30d158")),
                         4,
                         0,
                         {}});
  annotations.push_back({Annotation::Kind::Text,
                         {80, 210},
                         {},
                         QStringLiteral("Qt C++"),
                         QColor(QStringLiteral("#0a84ff")),
                         4,
                         0,
                         {}});
  const QImage rendered = renderCapture(capture, QRectF(100, 100, 500, 300),
                                        annotations, BackgroundStyle::Aurora);
  if (rendered.isNull() || rendered.size() != QSize(628, 428) ||
      !rendered.save(outputRoot + QStringLiteral("-render.png"), "PNG"))
    return 3;

  CaptureData clippingCapture;
  clippingCapture.monitor.scale = 1.0;
  clippingCapture.source = QImage(100, 100, QImage::Format_RGB32);
  clippingCapture.source.fill(Qt::white);
  clippingCapture.previewSize = clippingCapture.source.size();
  Annotation croppedOut;
  croppedOut.kind = Annotation::Kind::Rectangle;
  croppedOut.start = {120, 20};
  croppedOut.end = {150, 60};
  croppedOut.color = QColor(QStringLiteral("#ff00ff"));
  croppedOut.size = 4;
  const QImage clippedExport =
      renderCapture(clippingCapture, QRectF(0, 0, 100, 100), {croppedOut},
                    BackgroundStyle::Aurora);
  const QRect exportedImageBounds(64, 64, 100, 100);
  for (int y = 0; y < clippedExport.height(); ++y) {
    for (int x = 0; x < clippedExport.width(); ++x) {
      if (exportedImageBounds.contains(x, y))
        continue;
      const QColor pixel = clippedExport.pixelColor(x, y);
      if (pixel.red() > 240 && pixel.green() < 32 && pixel.blue() > 240)
        return 65;
    }
  }

  CaptureData highDpiCapture = capture;
  highDpiCapture.monitor.scale = 2.0;
  highDpiCapture.monitor.pixelSize = {1600, 1200};
  highDpiCapture.source = capture.source.scaled(
      1500, 1125, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  const QImage highDpiRendered = renderCapture(
      highDpiCapture, QRectF(100, 100, 500, 300), {}, BackgroundStyle::None);
  if (highDpiRendered.size() != QSize(1000, 600))
    return 13;
  const QImage fullHighDpi = renderCapture(
      highDpiCapture, QRectF(0, 0, 800, 600), {}, BackgroundStyle::None);
  if (fullHighDpi.size() != QSize(1600, 1200) ||
      !fullHighDpi.save(outputRoot + QStringLiteral("-fullscreen-hidpi.png"),
                        "PNG"))
    return 15;

  QImage ocrImage(1000, 260, QImage::Format_RGB32);
  ocrImage.fill(Qt::white);
  {
    QPainter painter(&ocrImage);
    painter.setPen(Qt::black);
    painter.setFont(QFont(QStringLiteral("Noto Sans"), 64, QFont::Bold));
    painter.drawText(ocrImage.rect(), Qt::AlignCenter,
                     QStringLiteral("OCR smoke test 42"));
  }
  QString ocrError;
  if (!recognizeText(ocrImage, ocrError)
           .contains(QStringLiteral("OCR smoke test 42")))
    return 5;
  const QByteArray savedNirisnapLangs = qgetenv("NIRISNAP_OCR_LANGS");
  qunsetenv("NIRISNAP_OCR_LANGS");
  const QString fallbackOcr = recognizeText(ocrImage, ocrError);
  if (savedNirisnapLangs.isEmpty())
    qunsetenv("NIRISNAP_OCR_LANGS");
  else
    qputenv("NIRISNAP_OCR_LANGS", savedNirisnapLangs);
  if (!fallbackOcr.contains(QStringLiteral("OCR smoke test 42")))
    return 65;
  qputenv("NIRISNAP_OCR_LANGS", "eng;sh");
  QString invalidOcrError;
  if (!recognizeText(ocrImage, invalidOcrError).isEmpty() ||
      invalidOcrError.isEmpty())
    return 66;
  if (savedNirisnapLangs.isEmpty())
    qunsetenv("NIRISNAP_OCR_LANGS");
  else
    qputenv("NIRISNAP_OCR_LANGS", savedNirisnapLangs);
  QTest::mouseMove(&editor, QPoint(400, 300), 20);
  QTest::keyClick(&editor, Qt::Key_T);
  QTest::keyClick(&editor, Qt::Key_Escape);
  application.processEvents();
  if (editor.isVisible())
    return 24;

  // Only a temporary fake xdg-open may launch; never start a real viewer.
  {
    QTemporaryDir fixture;
    if (!fixture.isValid())
      return 110;
    QDir exportRoot(fixture.filePath(QStringLiteral("exports with spaces é")));
    const QString launcher = fixture.filePath(QStringLiteral("xdg-open"));
    const QString record = fixture.filePath(QStringLiteral("opened"));
    const QString gate = fixture.filePath(QStringLiteral("parent-exited"));
    if (!QDir().mkpath(exportRoot.path()))
      return 110;
    const QProcessEnvironment previousEnvironment = QProcessEnvironment::systemEnvironment();
    const auto restoreDirectory = qScopeGuard([&] {
      for (const char *key : {"PATH", "NIRISNAP_SCREENSHOT_DIR",
                              "QT_WAYLAND_SHELL_INTEGRATION", "NIRISNAP_TEST_OPEN_RECORD",
                              "NIRISNAP_TEST_OPEN_GATE", "NIRISNAP_TEST_OPEN_SENTINEL"}) {
        if (previousEnvironment.contains(QString::fromLatin1(key)))
          qputenv(key, previousEnvironment.value(QString::fromLatin1(key)).toUtf8());
        else
          qunsetenv(key);
      }
    });
    qputenv("PATH", fixture.path().toUtf8());
    qputenv("NIRISNAP_SCREENSHOT_DIR", exportRoot.path().toUtf8());
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
    qputenv("NIRISNAP_TEST_OPEN_RECORD", record.toUtf8());
    qputenv("NIRISNAP_TEST_OPEN_SENTINEL", "inherited");
    qunsetenv("NIRISNAP_TEST_OPEN_GATE");
    QFile opener(launcher);
    if (!opener.open(QIODevice::WriteOnly))
      return 110;
    const QByteArray script =
        "#!/bin/sh\n"
        "if [ -n \"$NIRISNAP_TEST_OPEN_GATE\" ]; then\n"
        "  n=0; while [ ! -f \"$NIRISNAP_TEST_OPEN_GATE\" ]; do\n"
        "    n=$((n+1)); [ \"$n\" -lt 100 ] || exit 9; /bin/sleep 0.05\n"
        "  done\n"
        "fi\n"
        "printf '%s\\n' \"$#\" \"${QT_WAYLAND_SHELL_INTEGRATION-unset}\" "
        "\"$NIRISNAP_TEST_OPEN_SENTINEL\" \"$1\" > \"$NIRISNAP_TEST_OPEN_RECORD\"\n"
        "exit 7\n";
    if (opener.write(script) != script.size())
      return 110;
    opener.close();
    const auto waitForRecord = [&](const QString &path) {
      const QByteArray expected = "1\nunset\ninherited\n" + path.toUtf8() + '\n';
      QElapsedTimer deadline;
      deadline.start();
      while (deadline.elapsed() < 5000) {
        QFile logged(record);
        if (logged.open(QIODevice::ReadOnly) && logged.readAll() == expected)
          return true;
        QTest::qWait(10);
      }
      return false;
    };
    const QString clipboardSentinel = QStringLiteral("open-app-keeps-clipboard");
    QApplication::clipboard()->setText(clipboardSentinel);
    for (const bool launchSucceeds : {true, false}) {
      QFile::remove(record);
      // A nonexecutable launcher fails to start; exit 7 above does not.
      if (!QFile::setPermissions(launcher, QFile::ReadOwner | QFile::WriteOwner |
                                             (launchSucceeds ? QFile::ExeOwner : QFile::Permissions{})))
        return 110;
      QString exportedPath;
      {
        CaptureEditor openEditor(capture);
        openEditor.resize(1000, 700);
        openEditor.show();
        application.processEvents();
        QTest::mousePress(&openEditor, Qt::LeftButton, Qt::NoModifier,
                         QPoint(180, 160));
        QTest::mouseMove(&openEditor, QPoint(680, 480), 20);
        QTest::mouseRelease(&openEditor, Qt::LeftButton, Qt::NoModifier,
                           QPoint(680, 480));
        // Solid secure redaction and marker over a cropped selection.
        QTest::keyClick(&openEditor, Qt::Key_D);
        QTest::keyClick(&openEditor, Qt::Key_D);
        QTest::mousePress(&openEditor, Qt::LeftButton, Qt::NoModifier,
                         QPoint(350, 300));
        QTest::mouseMove(&openEditor, QPoint(440, 360), 20);
        QTest::mouseRelease(&openEditor, Qt::LeftButton, Qt::NoModifier,
                           QPoint(440, 360));
        QTest::keyClick(&openEditor, Qt::Key_C);
        QTest::mouseClick(&openEditor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(550, 400));
        application.processEvents();
        const QImage expected = openEditor.renderCurrentOutput()
                                    .convertToFormat(QImage::Format_ARGB32);
        if (expected.size() != QSize(500, 320) ||
            expected.pixelColor(145, 135) != QColor(QStringLiteral("#121216")))
          return 115;
        const QStringList before = QDir(exportRoot.path()).entryList(
            {QStringLiteral("*.png")}, QDir::Files);
        // Open app follows Save, before Cutout and Close: button index 19.
        QTest::mouseClick(&openEditor, Qt::LeftButton, Qt::NoModifier,
                          QPoint(60 + 764 + 18, 167));
        application.processEvents();
        QStringList after = QDir(exportRoot.path()).entryList(
            {QStringLiteral("*.png")}, QDir::Files);
        for (const QString &existing : before)
          after.removeAll(existing);
        if (after.size() != 1) {
          qWarning() << "Open app did not export one durable PNG";
          return 111;
        }
        exportedPath = QDir(exportRoot.path()).filePath(after.constFirst());
        const auto permissions = QFileInfo(exportedPath).permissions();
        if (QImage(exportedPath).convertToFormat(QImage::Format_ARGB32) != expected ||
            (permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther |
                            QFile::WriteOther)) ||
            QApplication::clipboard()->text() != clipboardSentinel ||
            openEditor.isVisible() == launchSucceeds ||
            (launchSucceeds && !waitForRecord(exportedPath)) ||
            (!launchSucceeds && QFileInfo::exists(record)))
          return 112;
        if (!launchSucceeds) {
          // Subsequent edits must use runtime snapshots, not overwrite export.
          QTest::keyClick(&openEditor, Qt::Key_C);
          QTest::mouseClick(&openEditor, Qt::LeftButton, Qt::NoModifier,
                            QPoint(500, 400));
          if (!openEditor.waitForSnapshot() ||
              QImage(exportedPath).convertToFormat(QImage::Format_ARGB32) != expected)
            return 113;
          QTest::keyClick(&openEditor, Qt::Key_Escape);
        }
      }
      if (!QFileInfo::exists(exportedPath))
        return 114;
    }
    // Detached launcher waits until its short-lived editor parent has exited.
    QFile::remove(record);
    if (!QFile::setPermissions(launcher, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
      return 118;
    const QString lifecycleRoot = fixture.filePath(QStringLiteral("parent exit exports é"));
    if (!QDir().mkpath(lifecycleRoot))
      return 118;
    QProcess helper;
    QProcessEnvironment helperEnvironment = QProcessEnvironment::systemEnvironment();
    helperEnvironment.insert(QStringLiteral("NIRISNAP_SCREENSHOT_DIR"), lifecycleRoot);
    helperEnvironment.insert(QStringLiteral("NIRISNAP_TEST_OPEN_GATE"), gate);
    helper.setProcessEnvironment(helperEnvironment);
    helper.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--open-app-exit-helper")});
    if (!helper.waitForFinished(10000) || helper.exitStatus() != QProcess::NormalExit ||
        helper.exitCode() != 0 || QFileInfo::exists(record))
      return 118;
    const QStringList lifecycleFiles = QDir(lifecycleRoot).entryList({QStringLiteral("*.png")}, QDir::Files);
    if (lifecycleFiles.size() != 1)
      return 118;
    QFile release(gate);
    if (!release.open(QIODevice::WriteOnly))
      return 118;
    release.close();
    const QString lifecyclePath = QDir(lifecycleRoot).filePath(lifecycleFiles.constFirst());
    if (!waitForRecord(lifecyclePath) || QImage(lifecyclePath).isNull())
      return 118;

    // Failed durable save must not start a launcher or discard the editor.
    QFile::remove(record);
    const QString blockedDirectory = QDir(exportRoot.path()).filePath("not-a-directory");
    QFile blocker(blockedDirectory);
    if (!blocker.open(QIODevice::WriteOnly))
      return 116;
    blocker.close();
    qputenv("NIRISNAP_SCREENSHOT_DIR", blockedDirectory.toUtf8());
    CaptureEditor blockedEditor(capture, CaptureEditor::CaptureMode::File);
    blockedEditor.resize(1000, 700);
    blockedEditor.show();
    application.processEvents();
    // Full 800×600 image fits available height: toolbar is above image at y=10.
    QTest::mouseClick(&blockedEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(60 + 764 + 18, 28));
    application.processEvents();
    QTest::qWait(100);
    if (QFileInfo::exists(record) || !blockedEditor.isVisible() ||
        QApplication::clipboard()->text() != clipboardSentinel)
      return 117;
    QTest::keyClick(&blockedEditor, Qt::Key_Escape);
  }

  QString savedPath;
  {
    CaptureEditor finishEditor(capture);
    finishEditor.resize(800, 600);
    finishEditor.show();
    application.processEvents();
    QTest::mousePress(&finishEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(100, 100));
    QTest::mouseMove(&finishEditor, QPoint(650, 470), 20);
    QTest::mouseRelease(&finishEditor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(650, 470));
    QTest::keyClick(&finishEditor, Qt::Key_C);
    QTest::mouseClick(&finishEditor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(470, 300));
    application.processEvents();
    const QImage snapshotBeforeSave =
        flushedSnapshot(finishEditor, snapshotPath)
            .convertToFormat(QImage::Format_ARGB32);
    if (snapshotBeforeSave.isNull())
      return 59;
    QTest::keyClick(&finishEditor, Qt::Key_S, Qt::ControlModifier);
    application.processEvents();
    const QStringList savedFiles =
        QDir(QDir(outputRoot).filePath(QStringLiteral("saved")))
            .entryList({QStringLiteral("*.png")}, QDir::Files);
    if (finishEditor.isVisible() || savedFiles.size() != 1)
      return 60;
    savedPath = QDir(QDir(outputRoot).filePath(QStringLiteral("saved")))
                    .filePath(savedFiles.constFirst());
    if (QImage(savedPath).convertToFormat(QImage::Format_ARGB32) !=
            snapshotBeforeSave ||
        QFile::exists(snapshotPath))
      return 61;
  }
  if (!QFile::exists(savedPath))
    return 70;
  if (qEnvironmentVariableIsSet("NIRISNAP_SMOKE_COPY")) {
    QString clipboardError;
    if (!copyPngFileToClipboard(savedPath, clipboardError)) {
      qWarning().noquote() << clipboardError;
      return 64;
    }
  }

  QString clipboardError;
  if (!runClipboardSmoke(clipboardError)) {
    qWarning().noquote() << clipboardError;
    return 88;
  }

  QString transformError;
  if (!runTransformSmoke(transformError)) {
    qWarning().noquote() << transformError;
    return 67;
  }
  QString instanceError;
  if (!runInstanceLockSmoke(instanceError)) {
    qWarning().noquote() << instanceError;
    return 85;
  }
  return 0;
}
