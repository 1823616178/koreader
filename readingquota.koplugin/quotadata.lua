--[[--
Data access and formatting for the AI credits dashboard.

The endpoint is configurable because KOReader usually runs on another device
than the dashboard server. A failed provider/window is kept as nil so one bad
source never hides the other providers.

@module koplugin.ReadingQuota.QuotaData
--]]--

local DataStorage = require("datastorage")
local LuaSettings = require("luasettings")
local http = require("socket.http")
local rapidjson = require("rapidjson")
local socketutil = require("socketutil")

local SETTINGS_PATH = DataStorage:getSettingsDir() .. "/readingquota.lua"
-- Loopback only: the real server address is a per-device setting, never a
-- hardcoded LAN address in the repo. Existing devices keep their saved
-- usage_api_url; fresh installs must set it once (see README).
local DEFAULT_API_BASE = "http://127.0.0.1:46927"

-- Dynamic provider display: no fixed provider filter is sent, so the
-- server returns everything it collects and the page shows exactly that.
-- Known providers keep a stable order and title; anything else the server
-- returns is shown after them, alphabetically, with a title derived from
-- its id. A provider counts as "returned" when its object is present in
-- the response, even if all its windows are null (that still renders a
-- card saying "暂无数据" rather than silently hiding the provider).
local KNOWN_ORDER = { "grok", "codex", "kimi" }
local KNOWN_TITLES = { grok = "Grok", codex = "Codex", kimi = "Kimi Code" }
local AGY_ORDER = { "gemini", "thirdParty" }
local AGY_TITLES = { gemini = "AGY · Gemini", thirdParty = "AGY · Claude/GPT" }
local RESERVED_KEYS = {
    displayMode = true, alertPercent = true, generatedAt = true,
    providers = true,
}

local QuotaData = {}

local function trim(value)
    return (value:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function clamp(value)
    return math.max(0, math.min(100, value))
end

local function validWindow(value)
    if type(value) ~= "table" or type(value.used) ~= "number" then
        return nil
    end
    return {
        used = clamp(value.used),
        resetAt = type(value.resetAt) == "number" and value.resetAt or nil,
    }
end

local function providerWindows(provider)
    provider = type(provider) == "table" and provider or {}
    return {
        five = validWindow(provider.five),
        seven = validWindow(provider.seven),
        monthly = validWindow(provider.monthly),
    }
end

local function normalizeUrl(value)
    value = trim(type(value) == "string" and value or "")
    if value == "" then
        value = DEFAULT_API_BASE
    end
    if not value:match("^https?://") then
        value = "http://" .. value
    end
    local query = ""
    local query_start = value:find("?", 1, true)
    if query_start then
        query = value:sub(query_start)
        value = value:sub(1, query_start - 1)
    end
    value = value:gsub("/+$", "")
    if not value:match("/api/usage$") then
        value = value .. "/api/usage"
    end
    return value .. query
end

function QuotaData:getSettings()
    if not self.settings then
        self.settings = LuaSettings:open(SETTINGS_PATH)
    end
    return self.settings
end

function QuotaData:getApiUrl()
    -- No provider filter: any ?providers= in a saved URL is passed through
    -- untouched, so a user who does want to narrow the request can still
    -- do so by editing the URL, but by default the server decides.
    return normalizeUrl(
        self:getSettings():readSetting("usage_api_url", DEFAULT_API_BASE))
end

function QuotaData:setApiUrl(value)
    local url = normalizeUrl(value)
    local settings = self:getSettings()
    settings:saveSetting("usage_api_url", url)
    settings:flush()
    return url
end

-- Optional Bearer token, stored alongside the URL in KOReader's settings
-- file (same plaintext sensitivity as any other KOReader setting). The
-- server side must validate it; an unset token means "no Authorization
-- header", preserving the old unauthenticated behaviour. Tokens containing
-- anything outside printable ASCII are never sent.
local function validToken(value)
    if type(value) ~= "string" or value == "" or #value > 200
        or value:find("[^\033-\126]") then
        return nil
    end
    return value
end

function QuotaData:getApiToken()
    return validToken(self:getSettings():readSetting("usage_api_token", ""))
end

function QuotaData:setApiToken(value)
    if type(value) ~= "string" then
        return false, "令牌须为不含空格的可打印 ASCII 字符，最多 200 字符"
    end
    local token = validToken(value)
    if value ~= "" and not token then
        return false, "令牌须为不含空格的可打印 ASCII 字符，最多 200 字符"
    end
    local settings = self:getSettings()
    if token then
        settings:saveSetting("usage_api_token", token)
    elseif settings.delSetting then
        settings:delSetting("usage_api_token")
    else
        -- Older luasettings without delSetting: an empty value reads back
        -- as unset through validToken, so this is equivalent.
        settings:saveSetting("usage_api_token", "")
    end
    settings:flush()
    return true
end

local function providerTitle(id)
    return KNOWN_TITLES[id] or (id:sub(1, 1):upper() .. id:sub(2))
end

local function sortedKeys(value)
    local keys = {}
    for key in pairs(value) do
        if type(key) == "string" then
            table.insert(keys, key)
        end
    end
    table.sort(keys)
    return keys
end

--- Flatten one response into an ordered provider list. Each entry is
-- { id, title, windows = { five, seven, monthly } }, ready for the page to render
-- as one card. AGY is a container, not a provider with windows of its
-- own: it expands into its sub-providers, unless a future response gives
-- AGY five/seven windows directly, in which case it stays a single card.
local function buildProviders(decoded)
    local providers = {}
    local function add(id, title, raw)
        if type(raw) == "table" then
            table.insert(providers, {
                id = id, title = title, windows = providerWindows(raw),
            })
        end
    end

    for _, id in ipairs(KNOWN_ORDER) do
        add(id, KNOWN_TITLES[id], decoded[id])
    end

    local agy = decoded.agy
    if type(agy) == "table" then
        if agy.five ~= nil or agy.seven ~= nil or agy.monthly ~= nil then
            add("agy", "AGY", agy)
        else
            for _, sub in ipairs(AGY_ORDER) do
                add("agy." .. sub, AGY_TITLES[sub], agy[sub])
            end
            for _, sub in ipairs(sortedKeys(agy)) do
                if sub ~= "gemini" and sub ~= "thirdParty" then
                    add("agy." .. sub, "AGY · " .. providerTitle(sub), agy[sub])
                end
            end
        end
    end

    for _, id in ipairs(sortedKeys(decoded)) do
        if not RESERVED_KEYS[id] and id ~= "agy" and not KNOWN_TITLES[id] then
            add(id, providerTitle(id), decoded[id])
        end
    end
    return providers
end

--- Fetch and validate one aggregate usage snapshot.
-- Returns normalized data, or nil plus a concise user-facing error.
function QuotaData:fetch()
    local sink = {}
    local headers = {
        ["Accept"] = "application/json",
        ["Cache-Control"] = "no-cache",
    }
    local token = self:getApiToken()
    if token then
        headers["Authorization"] = "Bearer " .. token
    end
    socketutil:set_timeout(45, 50)
    local ok, request_ok, code, _, status = pcall(function()
        return http.request{
            url = self:getApiUrl(),
            method = "GET",
            headers = headers,
            sink = socketutil.table_sink(sink),
        }
    end)
    socketutil:reset_timeout()

    if not ok then
        return nil, "请求失败：" .. tostring(request_ok)
    end
    if not request_ok or tonumber(code) ~= 200 then
        return nil, "接口不可用：" .. tostring(status or code or "网络错误")
    end

    local decode_ok, decoded, decode_err = pcall(rapidjson.decode, table.concat(sink))
    if not decode_ok then
        return nil, "响应不是有效 JSON：" .. tostring(decoded)
    end
    if type(decoded) ~= "table" then
        return nil, "响应不是有效 JSON：" .. tostring(decode_err or "未知错误")
    end
    local mt = getmetatable(decoded)
    if type(mt) == "table" and mt.__jsontype == "array" then
        return nil, "响应必须是用量对象，不能是数组"
    end
    for key in pairs(decoded) do
        if type(key) ~= "string" then
            return nil, "响应必须是用量对象，不能是数组"
        end
    end

    return {
        displayMode = decoded.displayMode == "remaining" and "remaining" or "used",
        alertPercent = type(decoded.alertPercent) == "number" and clamp(decoded.alertPercent) or 85,
        generatedAt = type(decoded.generatedAt) == "number" and decoded.generatedAt or nil,
        providers = buildProviders(decoded),
    }
end

function QuotaData.emptySnapshot()
    -- No provider cards yet: the loading state shows only the info tile
    -- (which says "连接中"), not placeholder tiles for providers that may
    -- never arrive. A fresh list each call, so callers cannot mutate a
    -- shared snapshot.
    return {
        displayMode = "used",
        alertPercent = 85,
        generatedAt = nil,
        providers = {},
    }
end

function QuotaData.displayValue(window, mode)
    if not window then
        return nil
    end
    if mode == "remaining" then
        return clamp(100 - window.used)
    end
    return window.used
end

function QuotaData.formatPercent(value)
    if type(value) ~= "number" then
        return "--"
    end
    if math.abs(value - math.floor(value + 0.5)) < 0.05 then
        return string.format("%d%%", math.floor(value + 0.5))
    end
    return string.format("%.1f%%", value)
end

function QuotaData.formatReset(reset_at)
    if type(reset_at) ~= "number" then
        return "重置时间未知"
    end
    local seconds = math.floor(reset_at / 1000 - os.time())
    if seconds <= 0 then
        return "即将重置"
    elseif seconds < 3600 then
        return string.format("%d 分钟后", math.max(1, math.ceil(seconds / 60)))
    elseif seconds < 86400 then
        local hours = math.floor(seconds / 3600)
        local minutes = math.floor(seconds % 3600 / 60)
        return string.format("%d 小时 %d 分后", hours, minutes)
    elseif seconds < 7 * 86400 then
        return string.format("%d 天 %d 小时后", math.floor(seconds / 86400), math.floor(seconds % 86400 / 3600))
    end
    return os.date("%m-%d %H:%M", math.floor(reset_at / 1000))
end

function QuotaData.formatTimestamp(timestamp)
    if type(timestamp) ~= "number" then
        return "--"
    end
    return os.date("%m-%d %H:%M:%S", math.floor(timestamp / 1000))
end

return QuotaData
