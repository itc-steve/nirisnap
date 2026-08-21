import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import qs.Commons
import qs.Widgets

Item {
  id: root
  property var pluginApi: null

  readonly property var geometryPlaceholder: panelContainer
  readonly property bool allowAttach: true
  property real contentPreferredWidth: 380 * Style.uiScaleRatio
  property real contentPreferredHeight: contentCol.implicitHeight + Style.marginL * 2

  readonly property var main: pluginApi?.mainInstance
  readonly property string outputLabel: pluginApi?.tr("output." + (main?.output ?? "edit")) || "Edit"

  anchors.fill: parent

  Rectangle {
    id: panelContainer
    anchors.fill: parent
    color: "transparent"

    ColumnLayout {
      id: contentCol
      anchors.fill: parent
      anchors.margins: Style.marginL
      spacing: Style.marginM

      NHeader {
        label: pluginApi?.tr("panel.title") || "Nirisnap"
        description: pluginApi?.tr("panel.description") || "Capture and annotate"
      }

      RowLayout {
        Layout.fillWidth: true
        spacing: Style.marginS

        NButton {
          Layout.fillWidth: true
          text: pluginApi?.tr("panel.region") || "Region"
          icon: "crop-square"
          onClicked: main?.capture("region")
        }

        NButton {
          Layout.fillWidth: true
          text: pluginApi?.tr("panel.window") || "Window"
          icon: "window"
          onClicked: main?.capture("window")
        }

        NButton {
          Layout.fillWidth: true
          text: pluginApi?.tr("panel.fullscreen") || "Full monitor"
          icon: "monitor"
          onClicked: main?.capture("fullscreen")
        }
      }

      NButton {
        Layout.fillWidth: true
        text: pluginApi?.tr("panel.clipboard") || "Edit clipboard image"
        icon: "clipboard"
        outlined: true
        onClicked: main?.clipboard()
      }

      NText {
        Layout.fillWidth: true
        text: (pluginApi?.tr("panel.output") || "Output") + ": " + root.outputLabel
        color: Color.mOnSurfaceVariant
        pointSize: Style.fontSizeXS
      }
    }
  }
}
