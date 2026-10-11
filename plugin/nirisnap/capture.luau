local M = {}
local flags = {
    region = "--capture-region",
    window = "--capture-window",
    fullscreen = "--capture-fullscreen",
}

function M.buildArgs(mode, output)
    local args = { flags[mode] or flags.region }
    if output == "copy" or output == "copy-save" then
        table.insert(args, "--copy")
    end
    if output == "save" or output == "copy-save" then
        table.insert(args, "--save")
    end
    return args
end

function M.launch(args)
    if not noctalia.commandExists("nirisnap") then
        noctalia.notifyError("Nirisnap", noctalia.tr("error.missing"))
        return
    end
    -- Detached helper delays capture until the closing panel has unmapped.
    local argv = { "/bin/sh", noctalia.pluginDir() .. "/launch.sh" }
    for _, value in ipairs(args) do
        table.insert(argv, value)
    end
    if not noctalia.runAsync(argv) then
        noctalia.notifyError("Nirisnap", noctalia.tr("error.launch"))
    end
end

function M.capture(mode)
    M.launch(M.buildArgs(mode, noctalia.getConfig("output_mode")))
end

function M.defaultCapture()
    M.capture(noctalia.getConfig("default_mode"))
end

function M.clipboard()
    M.launch({ "--clipboard" })
end

return M
