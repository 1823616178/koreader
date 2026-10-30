-- Page-state and pagination regressions without the KOReader runtime.
local plugin_dir = arg and arg[1] or "readingquota.koplugin"
package.path = plugin_dir .. "/?.lua;" .. package.path

local function widget(kind)
    local class = {}
    function class:new(value)
        return setmetatable(value or {}, { __index = self })
    end
    function class:extend(value)
        return self:new(value)
    end
    function class:free() end
    function class:getSize()
        if self.dimen then return self.dimen end
        local height = 0
        for _, child in ipairs(self) do
            if kind == "HorizontalGroup" then
                height = math.max(height, child:getSize().h)
            else
                height = height + child:getSize().h
            end
        end
        if kind == "ButtonTable" then height = 40 end
        if kind == "TextWidget" then height = 20 end
        if kind == "VerticalSpan" then height = self.width end
        if kind == "HorizontalSpan" then height = 0 end
        return { w = self.width or 0, h = self.height or height + 2 * (self.padding or 0) }
    end
    return class
end

local modules = {
    "buttontable", "container/centercontainer", "container/framecontainer",
    "gesturerange", "horizontalgroup", "horizontalspan", "container/inputcontainer",
    "inputdialog", "infomessage", "container/leftcontainer", "linewidget",
    "container/rightcontainer", "textwidget", "titlebar", "verticalgroup", "verticalspan",
}
local names = {
    buttontable = "ButtonTable", horizontalgroup = "HorizontalGroup",
    textwidget = "TextWidget", verticalspan = "VerticalSpan", horizontalspan = "HorizontalSpan",
}
for _, name in ipairs(modules) do
    local class = widget(names[name] or name)
    local module_name = name == "gesturerange" and "ui/gesturerange" or "ui/widget/" .. name
    package.preload[module_name] = function() return class end
end
package.preload["ui/geometry"] = function() return widget("geometry") end
package.preload["ui/font"] = function() return { getFace = function() return {} end } end
package.preload["ui/size"] = function() return { border = { thin = 1 } } end
package.preload["ffi/blitbuffer"] = function() return {} end
package.preload["quotabar"] = function() return widget("bar") end
package.preload["device"] = function()
    return { screen = { scaleBySize = function(_, n) return n end } }
end
local pending, shown = {}, {}
local manager = {
    setDirty = function() end,
    scheduleIn = function(_, _, callback) table.insert(pending, callback) end,
    show = function(_, item) table.insert(shown, item) end,
    close = function() end,
}
package.preload["ui/uimanager"] = function() return manager end
local data = {
    emptySnapshot = function() return { providers = {} } end,
    getApiUrl = function() return "http://example.test/api/usage" end,
    getApiToken = function() return nil end,
    formatTimestamp = function() return "--" end,
    formatPercent = tostring,
}
package.preload["quotadata"] = function() return data end
local Page = require("quotapage")

local function newPage()
    local page = Page:extend{
        content_width = 568, card_width = 280, card_inner_width = 260,
        card_gap = 8, card_padding = 9, line_height = 27,
        snapshot = { providers = {}, displayMode = "used", alertPercent = 85 },
    }
    page.rebuild = function(self) self.rebuilds = (self.rebuilds or 0) + 1 end
    return page
end

-- Every card remains reachable, controls stay within the available height,
-- and navigating in either direction wraps at the end.
for _, count in ipairs({ 0, 1, 2, 5, 20 }) do
    local page = newPage()
    function page:buildCard(title)
        return widget("card"):new{ title = title, height = 200 }
    end
    function page:buildInfoCard() return widget("info"):new{ height = 200 } end
    for i = 1, count do
        table.insert(page.snapshot.providers, { title = "provider-" .. i, windows = {} })
    end
    local seen = {}
    local first = page:buildContent(650)
    assert(first:getSize().h <= 650, "controls must fit on screen")
    local pages = page.page_count
    for i = 1, pages do
        page.page_index = i
        local content = page:buildContent(650)
        assert(content:getSize().h <= 650, "every page must fit")
        local function visit(item)
            if item.title then seen[item.title] = true end
            for _, child in ipairs(item) do visit(child) end
        end
        visit(content)
    end
    for i = 1, count do assert(seen["provider-" .. i], "provider must remain reachable") end
    page:changePage(1)
    assert(page.page_index == 1, "next page wraps")
    page:changePage(-1)
    assert(page.page_index == pages, "previous page wraps")
end

local page = newPage()
local labels = {}
function page:buildWindow(label)
    table.insert(labels, label)
    return widget("window"):new{ height = 60 }
end
page:buildCard("Grok", { monthly = { used = 20 } })
assert(labels[3] == "月额度", "monthly quota must render")

-- Saving settings during a fetch discards the old account's result and
-- schedules a fetch with the saved settings, including when the new fetch fails.
page = newPage()
page.loading = true
page.snapshot = { providers = { { title = "old-account" } } }
data.fetch = function()
    page:refresh(true)
    return { providers = { { title = "stale-result" } } }
end
page:performFetch()
assert(page.loading and #pending == 1, "settings change queues a new fetch")
assert(#page.snapshot.providers == 0, "old account snapshot is cleared")
data.fetch = function() return nil, "unauthorized" end
table.remove(pending, 1)()
assert(not page.loading and page.error_text == "unauthorized")
assert(#page.snapshot.providers == 0, "failed request must not restore old account")

-- Rejected input leaves the dialog open and does not start another fetch.
page = newPage()
local dialog_class = require("ui/widget/inputdialog")
function dialog_class:onShowKeyboard() end
function dialog_class:getInputText() return "bad token" end
data.setApiToken = function() return false, "invalid token" end
page:editApiToken()
local dialog = shown[#shown]
dialog.buttons[1][2].callback()
assert(shown[#shown].text == "invalid token", "invalid input must show feedback")
assert(#pending == 0 and not page.loading, "invalid input must not refresh")

io.write("all quotapage tests passed\n")
