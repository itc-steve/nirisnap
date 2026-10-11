local dir = arg[1]
local config = { default_mode = "region", output_mode = "edit", icon_color = "primary" }
local calls, notices, order = {}, {}, {}
noctalia = {
    getConfig = function(key) return config[key] end,
    pluginDir = function() return dir end,
    commandExists = function() return true end,
    runAsync = function(argv)
        table.insert(calls, argv)
        table.insert(order, "launch")
        return true
    end,
    notifyError = function(title, body) table.insert(notices, {title, body}) end,
    tr = function(key) return key end,
    togglePanel = function(id) assert(id == "itc-steve/nirisnap:panel") end,
    openSettings = function() table.insert(order, "settings") end,
    setUpdateInterval = function() end,
}
local capture = dofile(dir .. "/capture.luau")
for _, mode in ipairs({"region", "window", "fullscreen"}) do
    for _, output in ipairs({"edit", "copy", "save", "copy-save"}) do
        local argv = capture.buildArgs(mode, output)
        assert(argv[1] == "--capture-" .. mode)
        local expected = {argv[1]}
        if output == "copy" or output == "copy-save" then table.insert(expected, "--copy") end
        if output == "save" or output == "copy-save" then table.insert(expected, "--save") end
        assert(table.concat(argv, "|") == table.concat(expected, "|"))
    end
end
for _, value in ipairs({"; touch /tmp/no", "$(id)", "--file", false, 3}) do
    assert(table.concat(capture.buildArgs(value, value), "|") == "--capture-region")
end
assert(table.concat(capture.buildArgs(nil, nil), "|") == "--capture-region")
-- Resolve relative modules as the host does, without executing any process.
require = function(path) assert(path == "./capture.luau"); return capture end
barWidget = {
    setGlyph = function() end, setText = function() end, setTooltip = function() end,
    setGlyphColor = function(color) assert(color == "primary") end,
}
dofile(dir .. "/widget.luau")
update()
onClick()
assert(calls[#calls][3] == "--capture-region")
config.output_mode = "copy-save"
onIpc("window", "$(id)")
assert(table.concat(calls[#calls], "|") == "/bin/sh|" .. dir .. "/launch.sh|--capture-window|--copy|--save")
local count = #calls
onIpc("$(id)", "--file")
assert(#calls == count)
onMiddleClick()
onRightClick()
local tree
ui = {}
for _, kind in ipairs({"column", "row", "label", "button"}) do
    ui[kind] = function(props, children) return {props = props, children = children} end
end
panel = {
    render = function(value) tree = value end,
    close = function() table.insert(order, "close") end,
}
dofile(dir .. "/panel.luau")
onOpen("")
assert(tree ~= nil)
-- Click every capture button; panel must close before scheduling delayed launch.
local function visit(node)
    if type(node.props.onClick) == "function" then
        order = {}
        node.props.onClick()
        assert(order[1] == "close" and order[2] == "launch")
    end
    for _, child in ipairs(node.children or {}) do visit(child) end
end
visit(tree)
noctalia.commandExists = function() return false end
count = #calls
capture.launch(capture.buildArgs("region", "edit"))
assert(#calls == count and #notices == 1)
noctalia.commandExists = function() return true end
noctalia.runAsync = function() return false end
capture.launch(capture.buildArgs("region", "edit"))
assert(#notices == 2)
print("plugin regressions passed")
