-- Headless-ish smoke test for the AI credits page.
--
-- Run from the KOReader install dir:
--   KO_MULTIUSER=1 ./luajit smoke.lua <plugin_dir> <out_dir>

require("setupkoenv")

G_defaults = require("luadefaults"):open()
local DataStorage = require("datastorage")
G_reader_settings = require("luasettings"):open(DataStorage:getDataDir() .. "/settings.reader.lua")
require("gettext")

local Device = require("device")
require("document/canvascontext"):init(Device)
local Screen = Device.screen

local plugin_dir = assert(arg[1], "usage: smoke.lua <plugin_dir> <out_dir>")
local out_dir = arg[2] or "/tmp"
package.path = plugin_dir .. "/?.lua;" .. package.path

local QuotaData = require("quotadata")
local QuotaPage = require("quotapage")
local http = require("socket.http")
local rapidjson = require("rapidjson")

local function window(used, reset_hours)
    return {
        used = used,
        resetAt = (os.time() + reset_hours * 3600) * 1000,
    }
end

local sample = {
    displayMode = "used",
    alertPercent = 85,
    generatedAt = os.time() * 1000,
    codex = { five = window(12, 3), seven = window(34, 120) },
    kimi = { five = window(2.4, 2), seven = window(25, 72) },
    agy = {
        gemini = { five = window(67.8, 4), seven = window(13.5, 48) },
        thirdParty = { five = window(91, 1), seven = nil },
    },
}

-- Exercise URL normalization, provider filtering, the actual HTTP sink, JSON
-- decoder and response normalizer without depending on the LAN service.
QuotaData.settings = {
    readSetting = function()
        return "http://example.test/base/?foo=1&providers=claude"
    end,
}
local real_http_request = http.request
http.request = function(request)
    assert(request.url == "http://example.test/base/api/usage?foo=1&providers=codex,kimi,agy")
    assert(not request.url:find("opencode", 1, true))
    request.sink(rapidjson.encode(sample))
    request.sink(nil)
    return 1, 200, { ["content-type"] = "application/json" }, "OK"
end
local parsed = assert(QuotaData:fetch())
http.request = real_http_request
assert(parsed.codex.five.used == 12)
assert(parsed.agy.thirdParty.seven == nil)
assert(QuotaData.displayValue({ used = 0 }, "used") == 0)
assert(QuotaData.displayValue({ used = 18 }, "remaining") == 82)

local function render(name, configure)
    local page = QuotaPage:new{}
    if configure then
        configure(page)
        page:rebuild()
    end
    page:paintTo(Screen.bb, 0, 0)
    local path = out_dir .. "/" .. name .. ".png"
    Screen:shot(path)
    io.stderr:write("rendered " .. path .. "\n")
end

render("preview-live")

render("preview-partial", function(page)
    page.snapshot = sample
    page.loading = false
end)

render("preview-reached", function(page)
    sample.displayMode = "remaining"
    page.snapshot = sample
    page.loading = false
end)

render("preview-pages", function(page)
    page.loading = false
    page.error_text = "接口不可用：connection refused"
end)

-- Exercise the real refresh completion path without touching the network.
QuotaData.fetch = function() return sample end
local page = QuotaPage:new{}
page:performFetch()
page:paintTo(Screen.bb, 0, 0)
io.stderr:write("fetch/rebuild path ok\n")
io.stderr:write("all states rendered\n")
