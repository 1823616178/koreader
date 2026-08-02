-- Headless-ish smoke test: boot enough of KOReader to construct the QuotaPage
-- and paint it, then dump the framebuffer to a PNG. Catches the runtime errors
-- a syntax check can't -- bad widget fields, nil sizes, missing requires.
--
-- Renders the real data once, then a few synthetic states so the branches that
-- today's numbers don't happen to hit still get exercised.
--
-- Run from the KOReader install dir:
--   cd /usr/lib/koreader && KO_MULTIUSER=1 ./luajit smoke.lua <plugin_dir> <out_dir>

require("setupkoenv")

G_defaults = require("luadefaults"):open()
local DataStorage = require("datastorage")
G_reader_settings = require("luasettings"):open(DataStorage:getDataDir() .. "/settings.reader.lua")
require("gettext")

local Device = require("device")
-- Must happen before anything pulls in the font list, same as reader.lua does.
require("document/canvascontext"):init(Device)
local Screen = Device.screen

-- PluginLoader does this for real plugins; we're loading ours by hand.
local plugin_dir = assert(arg[1], "usage: smoke.lua <plugin_dir> <out_dir>")
local out_dir = arg[2] or "/tmp"
package.path = plugin_dir .. "/?.lua;" .. package.path

local QuotaData = require("quotadata")
local QuotaPage = require("quotapage")

local function render(name)
    local page = QuotaPage:new{}
    page:paintTo(Screen.bb, 0, 0)
    local path = out_dir .. "/" .. name .. ".png"
    Screen:shot(path)
    -- stderr, so the caller can drop KOReader's chatty ffi logging on stdout.
    io.stderr:write("rendered " .. path .. "\n")
end

-- 1. Whatever the statistics DB actually says right now.
render("preview-live")

-- 2. The rebuild path behind the 刷新 / 切换计法 buttons, which frees the whole
--    layout and builds a fresh one.
local page = QuotaPage:new{}
page:paintTo(Screen.bb, 0, 0)
page:refresh()
page:paintTo(Screen.bb, 0, 0)
io.stderr:write("refresh path ok\n")

-- 3. Synthetic states. Stubbing the two leaf getters keeps getQuota() itself
--    under test.
local function stub(today, goal)
    QuotaData.getToday = function() return today end
    QuotaData.getGoal = function() return goal end
end

stub({ duration = 19 * 60, pages = 24, source = "db" },
     { mode = "time", minutes = 30, pages = 20 })
render("preview-partial")

stub({ duration = 47 * 60, pages = 61, source = "db" },
     { mode = "time", minutes = 30, pages = 20 })
render("preview-reached")

stub({ duration = 12 * 60, pages = 13, source = "no_db" },
     { mode = "pages", minutes = 30, pages = 20 })
render("preview-pages")

io.stderr:write("all states rendered\n")
