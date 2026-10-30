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
local DEFAULT_API_BASE = "http://123.161.179.32:46927"
local PROVIDER_QUERY = "providers=codex,kimi,agy"

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

local function withProviderFilter(url)
    local base, query = url:match("^([^?]+)%?(.*)$")
    if not base then
        return url .. "?" .. PROVIDER_QUERY
    end
    local parts = {}
    for item in (query .. "&"):gmatch("(.-)&") do
        local key = item:match("^([^=]+)")
        if item ~= "" and key ~= "providers" and key ~= "tools"
            and key ~= "tool" and key ~= "provider" then
            table.insert(parts, item)
        end
    end
    table.insert(parts, PROVIDER_QUERY)
    return base .. "?" .. table.concat(parts, "&")
end

function QuotaData:getSettings()
    if not self.settings then
        self.settings = LuaSettings:open(SETTINGS_PATH)
    end
    return self.settings
end

function QuotaData:getApiUrl()
    return withProviderFilter(normalizeUrl(
        self:getSettings():readSetting("usage_api_url", DEFAULT_API_BASE)))
end

function QuotaData:setApiUrl(value)
    local url = normalizeUrl(value)
    local settings = self:getSettings()
    settings:saveSetting("usage_api_url", url)
    settings:flush()
    return url
end

--- Fetch and validate one aggregate usage snapshot.
-- Returns normalized data, or nil plus a concise user-facing error.
function QuotaData:fetch()
    local sink = {}
    socketutil:set_timeout(45, 50)
    local ok, request_ok, code, _, status = pcall(function()
        return http.request{
            url = self:getApiUrl(),
            method = "GET",
            headers = {
                ["Accept"] = "application/json",
                ["Cache-Control"] = "no-cache",
            },
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

    local agy = type(decoded.agy) == "table" and decoded.agy or {}
    return {
        displayMode = decoded.displayMode == "remaining" and "remaining" or "used",
        alertPercent = type(decoded.alertPercent) == "number" and clamp(decoded.alertPercent) or 85,
        generatedAt = type(decoded.generatedAt) == "number" and decoded.generatedAt or nil,
        codex = providerWindows(decoded.codex),
        kimi = providerWindows(decoded.kimi),
        agy = {
            gemini = providerWindows(agy.gemini),
            thirdParty = providerWindows(agy.thirdParty),
        },
    }
end

function QuotaData.emptySnapshot()
    local empty = { five = nil, seven = nil }
    return {
        displayMode = "used",
        alertPercent = 85,
        generatedAt = nil,
        codex = empty,
        kimi = empty,
        agy = {
            gemini = { five = nil, seven = nil },
            thirdParty = { five = nil, seven = nil },
        },
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
