-- Unit tests for readingquota.koplugin/quotadata.lua, without KOReader.
--
-- Run with any Lua 5.1+ interpreter (KOReader's luajit, or a stock lua):
--   lua tests/test_quotadata.lua readingquota.koplugin
--
-- KOReader modules are stubbed via package.preload. rapidjson.decode is a
-- stub that returns a preset table, so these tests cover URL normalization,
-- dynamic provider handling, token handling and response normalization --
-- not the real JSON parser (that is exercised by the KOReader smoke run).

local plugin_dir = arg and arg[1] or "readingquota.koplugin"
package.path = plugin_dir .. "/?.lua;" .. package.path

local captured = {}
local decode_result = nil
local store = {}

local function stub_settings()
    return {
        readSetting = function(_, key, default)
            if store[key] ~= nil then return store[key] end
            return default
        end,
        saveSetting = function(_, key, value) store[key] = value end,
        delSetting = function(_, key) store[key] = nil end,
        flush = function() end,
    }
end

package.preload["datastorage"] = function()
    return { getSettingsDir = function() return "/tmp/test-settings" end }
end
package.preload["luasettings"] = function()
    return { open = function() return stub_settings() end }
end
package.preload["socket.http"] = function()
    return {
        request = function(req)
            captured.url = req.url
            captured.headers = req.headers
            if req.sink then
                req.sink("ignored-by-decode-stub")
                req.sink(nil)
            end
            return 1, captured.status or 200, {}, "OK"
        end,
    }
end
package.preload["rapidjson"] = function()
    return { decode = function() return decode_result end }
end
package.preload["socketutil"] = function()
    return {
        set_timeout = function() end,
        reset_timeout = function() end,
        table_sink = function(sink)
            return function(chunk)
                if chunk then table.insert(sink, chunk) end
            end
        end,
    }
end

local QuotaData = require("quotadata")

local function reset(new_store)
    store = new_store or {}
    captured = { status = 200 }
    decode_result = nil
    QuotaData.settings = stub_settings()
end

local function check(cond, msg)
    assert(cond, msg)
    io.write("ok   " .. msg .. "\n")
end

-- 1. Defaults: loopback only, and no provider filter is added.
reset()
check(QuotaData:getApiUrl() == "http://127.0.0.1:46927/api/usage",
    "default URL is loopback with no provider filter")

-- 2. A saved URL keeps its host and its query untouched: a user who wants
-- to narrow the request can still do so by editing the URL, but nothing
-- is filtered by default.
reset({ usage_api_url = "192.168.1.100:46927?providers=claude" })
check(QuotaData:getApiUrl() ==
    "http://192.168.1.100:46927/api/usage?providers=claude",
    "saved URL is normalized with its query preserved")

-- 3. setApiUrl stores the normalized URL.
reset()
check(QuotaData:setApiUrl("  example.test:8123/ ") ==
    "http://example.test:8123/api/usage", "setApiUrl normalizes a bare host")
check(QuotaData:getApiUrl() == "http://example.test:8123/api/usage",
    "getApiUrl returns the normalized URL unchanged")

-- 4. Fetch shows exactly what the response contains: known providers in
-- stable order, AGY expanded into its sub-providers, and an unknown
-- provider after them, alphabetically, with a derived title. A provider
-- whose windows are all nil still gets a card.
reset()
decode_result = {
    displayMode = "used",
    alertPercent = 150, -- clamped to 100
    generatedAt = 1785695952333,
    grok = { five = { used = 0, resetAt = 1785712676000 }, seven = nil },
    codex = { five = { used = 12, resetAt = 1785712676000 }, seven = { used = 34 } },
    kimi = { five = nil, seven = nil },
    agy = {
        gemini = { five = { used = 67.8 }, seven = { used = 13.5 } },
        thirdParty = { five = { used = 91 }, seven = nil },
    },
    cursor = { five = { used = 5 }, seven = nil },
}
local snap, err = QuotaData:fetch()
check(snap ~= nil and err == nil, "fetch succeeds on a valid snapshot")
check(#snap.providers == 6, "six provider cards for six returned providers")
check(snap.providers[1].id == "grok" and snap.providers[1].title == "Grok",
    "first card is Grok")
check(snap.providers[1].windows.five.used == 0,
    "grok used=0 is valid, not treated as missing")
check(snap.providers[1].windows.seven == nil,
    "grok missing seven window stays nil")
check(snap.providers[3].id == "kimi",
    "provider with no windows still gets a card")
check(snap.providers[4].id == "agy.gemini"
    and snap.providers[4].title == "AGY · Gemini",
    "AGY expands to a Gemini card")
check(snap.providers[5].id == "agy.thirdParty"
    and snap.providers[5].windows.seven == nil,
    "AGY third-party card keeps a nil seven window")
check(snap.providers[6].id == "cursor" and snap.providers[6].title == "Cursor",
    "unknown provider is shown last with a derived title")
check(snap.alertPercent == 100, "alertPercent is clamped to 100")
check(captured.headers["Authorization"] == nil,
    "no Authorization header without a token")
check(captured.url == "http://127.0.0.1:46927/api/usage",
    "fetch sends no provider filter")

-- 4b. Fewer providers returned means fewer cards, in the same order.
reset()
decode_result = {
    codex = { five = { used = 12 }, seven = nil },
    agy = { gemini = { five = { used = 1 }, seven = nil } },
}
snap = QuotaData:fetch()
check(#snap.providers == 2
    and snap.providers[1].id == "codex"
    and snap.providers[2].id == "agy.gemini",
    "two returned providers give two cards")

-- 4c. AGY with its own five/seven windows stays a single card.
reset()
decode_result = {
    agy = { five = { used = 10 }, seven = { used = 20 } },
}
snap = QuotaData:fetch()
check(#snap.providers == 1 and snap.providers[1].id == "agy"
    and snap.providers[1].title == "AGY",
    "AGY with direct windows is one card, not expanded")

-- 5. A configured token is sent as Bearer; a token with whitespace is
-- rejected and never sent.
reset({ usage_api_token = "test-token_123" })
decode_result = { codex = {}, kimi = {}, agy = {} }
snap = QuotaData:fetch()
check(captured.headers["Authorization"] == "Bearer test-token_123",
    "configured token is sent as Bearer")
reset()
check(QuotaData:setApiToken("has space") == false, "token with space is rejected")
check(QuotaData:getApiToken() == nil, "rejected token is not stored")
check(QuotaData:setApiToken("ok-token") == true, "valid token is accepted")
check(QuotaData:setApiToken("") == false, "empty token clears the setting")
check(QuotaData:getApiToken() == nil, "cleared token reads back as nil")

-- 6. Non-200 and non-table responses are errors, not snapshots.
reset()
captured.status = 401
decode_result = {}
snap, err = QuotaData:fetch()
check(snap == nil and err ~= nil, "401 is an error, not a snapshot")
reset()
decode_result = "not-a-table"
snap, err = QuotaData:fetch()
check(snap == nil and err ~= nil, "non-table JSON is an error, not a snapshot")

-- 7. Formatting helpers and the empty snapshot (no cards while loading).
reset()
local empty = QuotaData.emptySnapshot()
check(type(empty.providers) == "table" and #empty.providers == 0,
    "empty snapshot has no provider cards")
check(QuotaData.emptySnapshot().providers ~= empty.providers,
    "empty snapshots do not share one provider list")
check(QuotaData.displayValue({ used = 0 }, "used") == 0,
    "displayValue keeps used=0")
check(QuotaData.displayValue({ used = 18 }, "remaining") == 82,
    "displayValue remaining inverts used")
check(QuotaData.formatPercent(12) == "12%", "formatPercent whole number")
check(QuotaData.formatPercent(2.4) == "2.4%", "formatPercent one decimal")
check(QuotaData.formatPercent(nil) == "--", "formatPercent nil is --")

io.write("all quotadata tests passed\n")
