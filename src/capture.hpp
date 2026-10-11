/** @fileoverview Declares screenshot capture, rendering, and output types. */
#pragma once

#include <QPainterPath>
#include <QColor>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <atomic>
#include <optional>

class QFont;
class QPainter;

struct MonitorInfo {
  QString name;
  QRect geometry;
  QSize pixelSize;
  qreal scale = 1.0;
  std::optional<quint64> workspaceId;
};

struct WindowTarget {
  QRect rect;
  QString stableId;
  QString title;
};

struct CaptureData {
  MonitorInfo monitor;
  QImage source;
  /** Logical size the native source image is presented at. */
  QSize previewSize;
  QVector<WindowTarget> windows;
};

enum class BackgroundStyle { None, Aurora, Sunset, Lagoon, Violet };
inline constexpr qreal kCaptureBackgroundMargin = 64.0;
inline constexpr qreal kCaptureBackgroundRadius = 14.0;
enum class QuickOutputMode { None, Copy, Save, Both };
enum class CaptureOutcome { Success, Cancelled, Error };

enum class SpotlightShape { Ellipse, Rectangle, RoundedRectangle };
enum class RedactionStyle { Solid, Pixelate };

struct Annotation {
  enum class Kind {
    Arrow,
    Line,
    Freehand,
    Highlighter,
    Marker,
    Rectangle,
    Text,
    Redaction,
    Spotlight
  };

  Kind kind = Kind::Arrow;
  QPointF start;
  QPointF end;
  QString text;
  QColor color;
  qreal size = 4.0;
  int number = 0;
  QVector<QPointF> points;
  RedactionStyle redactionStyle = RedactionStyle::Pixelate;
  qreal magnification = 2.0;
  SpotlightShape spotlightShape = SpotlightShape::Ellipse;
  quint32 redactionSeed = 0;

  bool operator==(const Annotation &) const = default;
};

enum class AnnotationLayer { Redaction, Default };

[[nodiscard]] constexpr AnnotationLayer annotationLayer(Annotation::Kind kind) {
  return kind == Annotation::Kind::Redaction ? AnnotationLayer::Redaction
                                             : AnnotationLayer::Default;
}

[[nodiscard]] bool loadCaptureFonts();
[[nodiscard]] QFont annotationTextFont(qreal size);
/**
 * Discovers the focused Niri output (name, logical geometry, scale). Safe to
 * call on the main thread before pixel capture runs in the background.
 */
[[nodiscard]] bool probeFocusedMonitor(MonitorInfo &monitor, QString &error);
/** Runs Niri's native picker and returns its selected window id. */
[[nodiscard]] CaptureOutcome
pickWindowTarget(WindowTarget &window, QString &error,
                 const std::atomic_bool *termination = nullptr);
/**
 * Captures the focused monitor's pixels onto the given monitor, and its window
 * list when `includeWindows` is set. Window discovery runs alongside the screen
 * grab, so callers that never show the overlay should skip it. Pure I/O and
 * image work (no GUI objects): safe on any thread.
 */
[[nodiscard]] bool captureMonitorPixels(const MonitorInfo &monitor,
                                        CaptureData &capture,
                                        bool includeWindows, QString &error);
/** Convenience: probes the focused monitor, then captures its pixels. */
[[nodiscard]] bool captureFocusedMonitor(CaptureData &capture,
                                         bool includeWindows, QString &error);
[[nodiscard]] CaptureOutcome
captureWindowSurface(const WindowTarget &window, QImage &image, QString &error,
                     const std::atomic_bool *termination = nullptr);
[[nodiscard]] QImage renderCapture(const CaptureData &capture,
                                   const QRectF &selection,
                                   const QVector<Annotation> &annotations,
                                   BackgroundStyle backgroundStyle);
/** Loads the current Wayland clipboard image. */
[[nodiscard]] bool loadClipboardImage(QImage &image, QString &error);
[[nodiscard]] bool copyPngFileToClipboard(const QString &path, QString &error);
[[nodiscard]] bool quickOutput(const QImage &image, QuickOutputMode mode,
                               QString &error);
[[nodiscard]] bool copyTextToClipboard(const QString &text, QString &error);
void paintAnnotation(QPainter &painter, const Annotation &annotation);
[[nodiscard]] QPainterPath spotlightPath(const Annotation &annotation);
void paintSpotlights(QPainter &painter, const QImage &source,
                     const QRectF &targetBounds, const QRectF &sourceRect,
                     const QVector<Annotation> &annotations);
/**
 * Paints the default annotation layer (spotlights, then vectors) in selection
 * space. Spotlights sample `redacted`, which must already include the
 * redaction layer so a loupe cannot magnify source pixels.
 */
void paintDefaultLayer(QPainter &painter, const QImage &redacted,
                       const QRectF &logicalBounds,
                       const QVector<Annotation> &annotations);
void paintCaptureBackground(QPainter &painter, const QRectF &bounds,
                            BackgroundStyle backgroundStyle);
void paintCaptureShadow(QPainter &painter, const QRectF &imageRect,
                        qreal scaleX, qreal scaleY);
/**
 * Renders the selection region at `targetSize` for the redaction layer. The
 * result carries no annotations; callers overlay redactions with
 * applyRedactionsScaled and cache it while the selection is unchanged.
 */
[[nodiscard]] QImage renderSelectionBase(const CaptureData &capture,
                                         const QRectF &selection,
                                         const QSize &targetSize);
/**
 * Paints redaction annotations over a display-resolution selection image. The
 * source image MUST be the exact selection region scaled to `targetSize`;
 * annotations are selection-relative, spanning 0..`selection` size.
 */
QImage applyRedactionsScaled(QImage image, const QVector<Annotation> &redactions,
                             const QRectF &selection, const QSizeF &targetSize);
/** Creates or repairs a private directory owned by the current user. */
[[nodiscard]] bool ensurePrivateDirectory(const QString &path);
/** Returns Nirisnap's private runtime directory, or empty on failure. */
[[nodiscard]] QString secureRuntimeDirectory();
[[nodiscard]] QString moveSnapshotToScreenshots(const QString &sourcePath,
                                                QString &error);
[[nodiscard]] QString temporarySnapshotPath();
[[nodiscard]] QString pinnedSnapshotPath(int index);
void prunePinnedSnapshots();
/**
 * Writes `image` into the private runtime directory. `quality` is the Qt PNG
 * quality knob, which maps inversely onto zlib levels: -1 keeps the default
 * level, higher values compress less and encode faster.
 */
[[nodiscard]] bool saveTemporarySnapshot(const QImage &image, QString path,
                                         QString &error, int quality = -1);
[[nodiscard]] QString recognizeText(const QImage &image, QString &error);
void sendCaptureNotification(const QString &message,
                             const QString &imagePath = {});
