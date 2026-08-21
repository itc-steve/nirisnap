import QtQuick
import QtQuick.Layouts
import qs.Commons
import qs.Widgets

ColumnLayout {
  id: root
  property var pluginApi: null

  property var cfg: pluginApi?.pluginSettings || ({})
  property var defaults: pluginApi?.manifest?.metadata?.defaultSettings || ({})

  // Edit copies — never write pluginSettings from bindings.
  property string editDefaultMode: String(cfg.defaultMode ?? defaults.defaultMode ?? "region")
  property string editOutputMode: String(cfg.outputMode ?? defaults.outputMode ?? "edit")
  property string editIconColor: String(cfg.iconColor ?? defaults.iconColor ?? "none")

  spacing: Style.marginL

  NComboBox {
    Layout.fillWidth: true
    label: pluginApi?.tr("settings.default_mode_label")
    description: pluginApi?.tr("settings.default_mode_desc")
    model: [
      { "key": "region", "name": pluginApi?.tr("mode.region") || "Region" },
      { "key": "window", "name": pluginApi?.tr("mode.window") || "Window" },
      { "key": "fullscreen", "name": pluginApi?.tr("mode.fullscreen") || "Full monitor" }
    ]
    currentKey: root.editDefaultMode
    onSelected: key => root.editDefaultMode = key
  }

  NComboBox {
    Layout.fillWidth: true
    label: pluginApi?.tr("settings.output_label")
    description: pluginApi?.tr("settings.output_desc")
    model: [
      { "key": "edit", "name": pluginApi?.tr("output.edit") || "Edit" },
      { "key": "copy", "name": pluginApi?.tr("output.copy") || "Copy" },
      { "key": "save", "name": pluginApi?.tr("output.save") || "Save" },
      { "key": "copy-save", "name": pluginApi?.tr("output.copy-save") || "Copy + save" }
    ]
    currentKey: root.editOutputMode
    onSelected: key => root.editOutputMode = key
  }

  NColorChoice {
    Layout.fillWidth: true
    label: pluginApi?.tr("settings.icon_color_label")
    description: pluginApi?.tr("settings.icon_color_desc")
    currentKey: root.editIconColor
    onSelected: key => root.editIconColor = key
  }

  function saveSettings() {
    if (!pluginApi)
      return;
    pluginApi.pluginSettings.defaultMode = root.editDefaultMode;
    pluginApi.pluginSettings.outputMode = root.editOutputMode;
    pluginApi.pluginSettings.iconColor = root.editIconColor;
    pluginApi.saveSettings();
    Logger.i("nirisnap", "Settings saved");
  }
}
