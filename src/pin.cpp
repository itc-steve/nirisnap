#include "pin.hpp"
#include "pin-file.hpp"
#include "pin-layout.hpp"
#include "icons.hpp"

#include <LayerShellQt/Window>
#include <QApplication>
#include <QBuffer>
#include <QDrag>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QScreen>
#include <QTimer>

#include <QMargins>
#include <QUrl>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <utility>

namespace {

constexpr qreal kCloseButtonSize = 22;
constexpr qreal kCloseButtonInset = 8;
constexpr qreal kControlGap = 6;
constexpr qreal kDragButtonWidth = kCloseButtonSize * 2 + kControlGap;
constexpr qreal kCornerMargin = 14;
constexpr int kPinGap = 10;
constexpr int kToastMs = 1200;
constexpr qreal kVisualInset = 8;
constexpr qreal kVisualRadius = 12;
constexpr qreal kControlsHeight = 36;
constexpr int kPinWidth = 250;
constexpr int kPinHeight = 200;

// wl-copy backgrounds itself and keeps serving the selection after this process
// exits, which QClipboard cannot do on Wayland.
bool copyPngToClipboard(const QImage &image, QString &error) {
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "PNG")) {
    error = QStringLiteral("Could not encode PNG");
    return false;
  }

  QProcess copy;
  copy.start(QStringLiteral("wl-copy"),
             {QStringLiteral("--type"), QStringLiteral("image/png")});
  if (!copy.waitForStarted(2000)) {
    error = copy.errorString();
    return false;
  }
  copy.write(png);
  copy.closeWriteChannel();
  if (!copy.waitForFinished(5000) || copy.exitCode() != 0) {
    error = QString::fromUtf8(copy.readAllStandardError()).trimmed();
    if (error.isEmpty())
      error = QStringLiteral("wl-copy failed");
    return false;
  }
  return true;
}

bool copyPinTextToClipboard(const QString &text, QString &error) {
  QProcess copy;
  copy.start(
      QStringLiteral("wl-copy"),
      {QStringLiteral("--type"), QStringLiteral("text/plain;charset=utf-8")});
  if (!copy.waitForStarted(2000)) {
    error = copy.errorString();
    return false;
  }
  copy.write(text.toUtf8());
  copy.closeWriteChannel();
  if (!copy.waitForFinished(5000) || copy.exitCode() != 0) {
    error = QString::fromUtf8(copy.readAllStandardError()).trimmed();
    if (error.isEmpty())
      error = QStringLiteral("wl-copy failed");
    return false;
  }
  return true;
}

class PinWindow final : public QWidget {
public:
  explicit PinWindow(QImage image, QString path)
      : image_(std::move(image)), path_(std::move(path)), snapshotFile_(path_) {
    setWindowTitle(QStringLiteral("nirisnap-pin"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(initialSize());
  }

  [[nodiscard]] bool hasPinLock() const {
    return snapshotFile_.isLocked() && slotLock_.isLocked();
  }

  [[nodiscard]] int slotIndex() const { return slotLock_.index(); }

  [[nodiscard]] QSize availableSize() const {
    const QScreen *target =
        screen() ? screen() : QGuiApplication::primaryScreen();
    return target ? target->availableGeometry().size() : QSize(1920, 1080);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRectF frame =
        QRectF(rect()).adjusted(kVisualInset, kVisualInset, -kVisualInset,
                                -kVisualInset);
    for (int layer = 8; layer > 0; --layer) {
      const qreal spread = layer * 1.5;
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0, 0, 0, 10 + (8 - layer) * 3));
      painter.drawRoundedRect(
          frame.adjusted(-spread, -spread, spread, spread),
          kVisualRadius + spread, kVisualRadius + spread);
    }
    painter.setBrush(QColor(18, 18, 22, 245));
    painter.drawRoundedRect(frame, kVisualRadius, kVisualRadius);

    const QRectF imageArea =
        frame.adjusted(0, kControlsHeight, 0, -kVisualInset);
    const QSize fitted =
        image_.size().scaled(imageArea.size().toSize(), Qt::KeepAspectRatio);
    const QRectF imageRect(
        imageArea.center().x() - fitted.width() / 2.0,
        imageArea.center().y() - fitted.height() / 2.0, fitted.width(),
        fitted.height());
    QPainterPath clip;
    clip.addRoundedRect(imageRect, kVisualRadius, kVisualRadius);
    painter.save();
    painter.setClipPath(clip);
    painter.drawImage(imageRect, image_);
    painter.restore();
    if (!toast_.isEmpty())
      paintToast(painter);
    if (!hovered_)
      return;

    drawControlButton(painter, dragButtonRect(), QStringLiteral("drag-handle"));
    drawControlButton(painter, editButtonRect(), QStringLiteral("edit"));
    drawControlButton(painter, pathButtonRect(), QStringLiteral("path"));
    drawControlButton(painter, copyButtonRect(), QStringLiteral("copy"));
    drawControlButton(painter, closeButtonRect(), QStringLiteral("close"));
    if (!hoverTip_.isEmpty())
      paintHoverTip(painter);
  }

  // Layer surfaces cannot reliably map an independent QToolTip toplevel, so
  // the hint is drawn inside the window like the toast pill.
  void paintHoverTip(QPainter &painter) const {
    const QFontMetrics metrics(painter.font());
    const qreal width = metrics.horizontalAdvance(hoverTip_) + 22;
    const QRectF pill(std::max(0.0, this->width() - width - kCloseButtonInset),
                      kCloseButtonInset + kCloseButtonSize + 5, width, 22);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 210));
    painter.drawRoundedRect(pill, 11, 11);
    painter.setPen(QColor(240, 240, 245));
    painter.drawText(pill, Qt::AlignCenter, hoverTip_);
  }

  void drawControlButton(QPainter &painter, const QRectF &rect,
                         const QString &action) const {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 190));
    painter.drawRoundedRect(rect, 6, 6);
    drawToolbarIcon(painter, rect, action, {}, QColor(245, 245, 247));
  }

  void paintToast(QPainter &painter) const {
    const QFontMetrics metrics(painter.font());
    const QRectF pill((width() - metrics.horizontalAdvance(toast_) - 28) / 2.0,
                      height() - 42, metrics.horizontalAdvance(toast_) + 28,
                      26);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(12, 12, 16, 205));
    painter.drawRoundedRect(pill, 13, 13);
    painter.setPen(QColor(240, 240, 245));
    painter.drawText(pill, Qt::AlignCenter, toast_);
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::MiddleButton) {
      close();
      return;
    }
    const QPointF position = event->position();
    if (event->button() == Qt::LeftButton) {
      if (closeButtonRect().contains(position)) {
        close();
        return;
      }
      if (dragButtonRect().contains(position)) {
        beginFileDrag();
        return;
      }
      if (copyButtonRect().contains(position)) {
        QString error;
        showToast(copyPngToClipboard(image_, error)
                      ? QStringLiteral("Copied to clipboard")
                      : error);
        return;
      }
      if (pathButtonRect().contains(position)) {
        QString error;
        showToast(copyPinTextToClipboard(path_, error)
                      ? QStringLiteral("Copied path")
                      : error);
        return;
      }
      if (editButtonRect().contains(position)) {
        reopenInEditor();
        return;
      }
      dragging_ = true;
      dragOffset_ = position.toPoint();
      event->accept();
    }
  }

  void reopenInEditor() {
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {path_}))
      showToast(QStringLiteral("Could not start nirisnap"));
    else {
      snapshotFile_.preserveForEditor();
      close();
    }
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    const QPointF position = event->position();
    if (dragging_) {
      const QScreen *target =
          screen() ? screen() : QGuiApplication::primaryScreen();
      const QPoint origin =
          target ? target->availableGeometry().topLeft() : QPoint();
      const QRect bounds(QPoint(),
                         target ? target->availableGeometry().size()
                                : availableSize());
      const QPoint requested = pinPositionFromGlobalPointer(
          event->globalPosition().toPoint(), origin, dragOffset_);
      applyPosition(clampPinGeometry(QRect(requested, size()), bounds).topLeft());
      event->accept();
      return;
    }
    setCursor(controlRectAt(position) >= 0 ? Qt::PointingHandCursor
                                           : Qt::ArrowCursor);

    const int control = controlRectAt(position);
    if (control != hoveredControl_) {
      hoveredControl_ = control;
      hoverTip_ = control >= 0 ? controlTip(control) : QString();
      update();
    }
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton)
      dragging_ = false;
    QWidget::mouseReleaseEvent(event);
  }

  // A layer surface can still initiate a Wayland uri-list drag just like a
  // file manager; the six-dot control is the drag handle.
  void beginFileDrag() {
    QMimeData *mime = new QMimeData;
    const QList<QUrl> urls{QUrl::fromLocalFile(path_)};
    mime->setUrls(urls);
    mime->setText(urls.constFirst().toLocalFile());
    QByteArray pngData;
    QBuffer buffer(&pngData);
    buffer.open(QIODevice::WriteOnly);
    if (image_.save(&buffer, "PNG"))
      mime->setData(QStringLiteral("image/png"), pngData);

    QDrag drag(this);
    drag.setMimeData(mime);
    drag.setPixmap(QPixmap::fromImage(image_.scaled(
        256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    drag.exec(Qt::CopyAction | Qt::MoveAction);
  }

  void wheelEvent(QWheelEvent *event) override {
    // Pinned captures deliberately keep a stable 250x200 frame so the
    // controls remain usable and the image area never reflows.
    event->accept();
  }

  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) {
      close();
      return;
    }
    if (event->matches(QKeySequence::Copy)) {
      QString error;
      showToast(copyPngToClipboard(image_, error)
                    ? QStringLiteral("Copied to clipboard")
                    : error);
      return;
    }
    QWidget::keyPressEvent(event);
  }

  void enterEvent(QEnterEvent *) override {
    hovered_ = true;
    hoveredControl_ = -1;
    update();
  }

  void leaveEvent(QEvent *) override {
    hovered_ = false;
    hoveredControl_ = -1;
    hoverTip_.clear();
    setCursor(Qt::ArrowCursor);
    update();
  }

private:
  [[nodiscard]] QString controlTip(int index) const {
    switch (index) {
    case 0:
      return QStringLiteral("Close · Esc or middle-click");
    case 1:
      return QStringLiteral("Copy image to clipboard");
    case 2:
      return QStringLiteral("Copy file path");
    case 3:
      return QStringLiteral("Edit in nirisnap");
    case 4:
      return QStringLiteral("Drag this image out");
    default:
      return {};
    }
  }

  [[nodiscard]] QSize initialSize() const {
    return {kPinWidth, kPinHeight};
  }

  void showToast(QString message) {
    toast_ = std::move(message);
    update();
    QTimer::singleShot(kToastMs, this, [this] {
      toast_.clear();
      update();
    });
  }

  void applyPosition(QPoint position) {
    position_ = position;
    if (QWindow *handle = windowHandle()) {
      if (LayerShellQt::Window *layer = LayerShellQt::Window::get(handle)) {
        const QSize available = availableSize();
        layer->setMargins(
            QMargins(0, 0, available.width() - position.x() - width(),
                     available.height() - position.y() - height()));
      }
    }
  }
  // The wide drag handle stands alone in the top-left; edit, path, copy, and
  // close remain grouped in the top-right.
  [[nodiscard]] QRectF closeButtonRect() const { return controlRect(0); }

  [[nodiscard]] QRectF copyButtonRect() const { return controlRect(1); }

  [[nodiscard]] QRectF pathButtonRect() const { return controlRect(2); }

  [[nodiscard]] QRectF editButtonRect() const { return controlRect(3); }

  [[nodiscard]] QRectF dragButtonRect() const { return controlRect(4); }

  [[nodiscard]] QRectF controlRect(int index) const {
    const qreal right = width() - kCloseButtonSize - kCloseButtonInset;
    if (index < 4) {
      return QRectF(right - index * (kCloseButtonSize + kControlGap),
                    kCloseButtonInset, kCloseButtonSize, kCloseButtonSize);
    }
    return QRectF(kCloseButtonInset, kCloseButtonInset, kDragButtonWidth,
                  kCloseButtonSize);
  }

  [[nodiscard]] int controlRectAt(const QPointF &position) const {
    for (int index = 0; index < 5; ++index) {
      if (controlRect(index).contains(position))
        return index;
    }
    return -1;
  }

  QImage image_;
  QString path_;
  PinSnapshotFile snapshotFile_;
  PinSlotLock slotLock_;
  QPoint position_;
  QPoint dragOffset_;
  bool dragging_ = false;
  QString toast_;
  QString hoverTip_;
  bool hovered_ = false;
  int hoveredControl_ = -1;
};

} // namespace

int runPinnedCapture(const QString &path) {
  QImage image(path);
  if (image.isNull()) {
    qWarning("nirisnap: could not load pinned image %s", qUtf8Printable(path));
    return 1;
  }

  PinWindow window(std::move(image), path);
  if (!window.hasPinLock()) {
    qWarning("nirisnap: could not lock pinned image %s", qUtf8Printable(path));
    return 1;
  }
  static_cast<void>(window.winId());
  QWindow *handle = window.windowHandle();
  LayerShellQt::Window *layer =
      handle ? LayerShellQt::Window::get(handle) : nullptr;
  if (!handle || !layer) {
    qCritical("nirisnap: could not create pinned layer surface");
    return 1;
  }

  layer->setScope(QStringLiteral("nirisnap-pin"));
  LayerShellQt::Window::Anchors anchors;
  anchors.setFlag(LayerShellQt::Window::AnchorBottom);
  anchors.setFlag(LayerShellQt::Window::AnchorRight);
  layer->setAnchors(anchors);
  const QPoint slot = pinSlotPosition(window.availableSize(), window.size(),
                                      window.size(), window.slotIndex(),
                                      kPinGap, kCornerMargin);
  layer->setMargins(QMargins(0, 0,
                             window.availableSize().width() - slot.x() -
                                 window.width(),
                             window.availableSize().height() - slot.y() -
                                 window.height()));
  layer->setExclusiveZone(0);
  layer->setDesiredSize(window.size());
  layer->setKeyboardInteractivity(
      LayerShellQt::Window::KeyboardInteractivityOnDemand);
  layer->setActivateOnShow(false);
  layer->setLayer(LayerShellQt::Window::LayerOverlay);
  window.show();
  return QApplication::exec();
}
