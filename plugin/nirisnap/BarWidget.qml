import QtQuick
import Quickshell
import qs.Commons
import qs.Services.UI
import qs.Widgets

Item {
  id: root

  property var pluginApi: null
  property ShellScreen screen
  property string widgetId: ""
  property string section: ""
  property int sectionWidgetIndex: -1
  property int sectionWidgetsCount: 0

  readonly property var main: pluginApi?.mainInstance
  readonly property var cfg: pluginApi?.pluginSettings || ({})
  readonly property var defaults: pluginApi?.manifest?.metadata?.defaultSettings || ({})

  readonly property string mode: main?.mode ?? "region"
  readonly property string output: main?.output ?? "edit"
  readonly property string iconColorKey: cfg.iconColor ?? defaults.iconColor ?? "none"
  readonly property real capsuleHeight: Style.getCapsuleHeightForScreen(screen?.name)

  readonly property string tooltip: pluginApi?.tr("widget.tooltip", {
    "mode": pluginApi?.tr("mode." + root.mode) || root.mode,
    "output": pluginApi?.tr("output." + root.output) || root.output
  }) || "Nirisnap — " + root.mode + " · " + root.output

  implicitWidth: capsuleHeight
  implicitHeight: capsuleHeight

  NIconButton {
    anchors.centerIn: parent
    baseSize: root.capsuleHeight
    applyUiScale: false
    icon: "camera"
    tooltipText: root.tooltip
    tooltipDirection: BarService.getTooltipDirection(root.screen?.name)
    customRadius: Style.radiusL
    colorBg: Style.capsuleColor
    colorFg: Color.resolveColorKey(root.iconColorKey)
    colorBgHover: Color.mHover
    colorFgHover: Color.mOnHover
    colorBorder: Style.capsuleBorderColor
    colorBorderHover: Style.capsuleBorderColor

    onClicked: root.main?.captureDefault()

    onMiddleClicked: {
      if (pluginApi)
        pluginApi.openPanel(root.screen, root);
    }

    onRightClicked: {
      if (pluginApi)
        BarService.openPluginSettings(root.screen, pluginApi.manifest);
    }
  }
}
