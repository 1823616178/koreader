--[[--
AI credits: a compact full-screen dashboard for Codex, Kimi and AGY.

Reachable from the file manager as well as from a book, so it can be used as a
standalone screen rather than only as a reader accessory.

@module koplugin.ReadingQuota
--]]--

local Dispatcher = require("dispatcher")
local UIManager = require("ui/uimanager")
local WidgetContainer = require("ui/widget/container/widgetcontainer")
local QuotaPage = require("quotapage")
local _ = require("gettext")

local ReadingQuota = WidgetContainer:extend{
    name = "readingquota",
    is_doc_only = false,
}

function ReadingQuota:onDispatcherRegisterActions()
    Dispatcher:registerAction("readingquota_show", {
        category = "none",
        event = "ShowReadingQuota",
        title = _("AI credits"),
        general = true,
    })
end

function ReadingQuota:init()
    self:onDispatcherRegisterActions()
    self.ui.menu:registerToMainMenu(self)
end

function ReadingQuota:addToMainMenu(menu_items)
    menu_items.reading_quota = {
        text = "AI 额度",
        sorting_hint = "more_tools",
        callback = function()
            self:showQuotaPage()
        end,
    }
end

function ReadingQuota:showQuotaPage()
    UIManager:show(QuotaPage:new{}, "full")
end

--- Fired by the dispatcher, so the page can be bound to a gesture or a key.
function ReadingQuota:onShowReadingQuota()
    self:showQuotaPage()
    return true
end

return ReadingQuota
