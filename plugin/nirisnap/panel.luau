local capture = require("./capture.luau")

function onOpen(_context)
    panel.render(ui.column({ gap = 12 }, {
        ui.label({ text = "Nirisnap", fontSize = 18, fontWeight = "bold" }),
        ui.button({ text = noctalia.tr("mode.region"), onClick = function()
            panel.close()
            capture.capture("region")
        end }),
        ui.button({ text = noctalia.tr("mode.window"), onClick = function()
            panel.close()
            capture.capture("window")
        end }),
        ui.button({ text = noctalia.tr("mode.fullscreen"), onClick = function()
            panel.close()
            capture.capture("fullscreen")
        end }),
        ui.button({ text = noctalia.tr("clipboard"), onClick = function()
            panel.close()
            capture.clipboard()
        end }),
        ui.row({ gap = 8 }, {
            ui.button({ text = noctalia.tr("settings"), onClick = "openSettings" }),
            ui.button({ text = noctalia.tr("close"), onClick = "closePanel" }),
        }),
    }))
end

function openSettings()
    noctalia.openSettings()
end

function closePanel()
    panel.close()
end
