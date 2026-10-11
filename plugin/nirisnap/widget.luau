local capture = require("./capture.luau")

function update()
    noctalia.setUpdateInterval(60000)
    barWidget.setGlyph("camera")
    barWidget.setText("")
    barWidget.setTooltip(noctalia.tr("widget.tooltip"))
    barWidget.setGlyphColor(noctalia.getConfig("icon_color"))
end

function onClick()
    capture.defaultCapture()
end

function onMiddleClick()
    noctalia.togglePanel("itc-steve/nirisnap:panel")
end

function onRightClick()
    noctalia.openSettings()
end

function onIpc(event, _payload)
    if event == "toggle" then
        capture.defaultCapture()
    elseif event == "region" or event == "window" or event == "fullscreen" then
        capture.capture(event)
    elseif event == "clipboard" then
        capture.clipboard()
    end
end
