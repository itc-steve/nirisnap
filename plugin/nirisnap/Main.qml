import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons

Item {
  id: root

  property var pluginApi: null

  readonly property var cfg: pluginApi?.pluginSettings || ({})
  readonly property var defaults: pluginApi?.manifest?.metadata?.defaultSettings || ({})

  // Settings files are user-editable, so every value is validated against the
  // fixed allowlists before it can reach a launch.
  readonly property string mode: root.validMode(cfg.defaultMode)
  readonly property string output: root.validOutput(cfg.outputMode)

  function validMode(value) {
    var m = String(value ?? "region");
    return ["region", "window", "fullscreen"].indexOf(m) !== -1 ? m : "region";
  }

  function validOutput(value) {
    var o = String(value ?? "edit");
    return ["edit", "copy", "save", "copy-save"].indexOf(o) !== -1 ? o : "edit";
  }

  // Pure argv builder (tested in tests/tst_plugin.qml). Always an argv array,
  // never shell-interpolated.
  function buildArgs(mode, output) {
    var args = ["nirisnap", "--capture-" + root.validMode(mode)];
    var o = root.validOutput(output);
    if (o === "copy" || o === "copy-save")
      args.push("--copy");
    if (o === "save" || o === "copy-save")
      args.push("--save");
    return args;
  }

  function launch(args) {
    // Ignore extra launches while a panel close is pending; upstream's
    // single-instance behavior takes over once the process is running.
    if (panelCloseTimer.running)
      return;
    if (pluginApi?.panelOpenScreen) {
      // Close first so the Noctalia panel does not end up in the capture.
      pluginApi.closePanel(pluginApi.panelOpenScreen);
      panelCloseTimer.pendingArgs = args;
      panelCloseTimer.start();
      return;
    }
    Quickshell.execDetached(args);
  }

  function capture(mode) {
    root.launch(root.buildArgs(mode, root.output));
  }

  function captureDefault() {
    root.capture(root.mode);
  }

  function region() {
    root.capture("region");
  }

  function windowCapture() {
    root.capture("window");
  }

  function fullscreen() {
    root.capture("fullscreen");
  }

  function clipboard() {
    root.launch(["nirisnap", "--clipboard"]);
  }

  Timer {
    id: panelCloseTimer
    interval: 200
    repeat: false
    property var pendingArgs: []
    onTriggered: {
      var args = root.panelCloseTimer.pendingArgs;
      root.panelCloseTimer.pendingArgs = [];
      if (args.length > 0)
        Quickshell.execDetached(args);
    }
  }

  IpcHandler {
    target: "plugin:nirisnap"
    function toggle() { root.captureDefault(); }
    function region() { root.region(); }
    function window() { root.windowCapture(); }
    function fullscreen() { root.fullscreen(); }
    function clipboard() { root.clipboard(); }
  }

  Component.onCompleted: Logger.i("nirisnap", "Main loaded")
}
