--[[--
Data layer for the Reading quota plugin.

Today's reading time and page count come from the statistics plugin's SQLite
database; the daily goal lives in our own settings file. Every DB access here
degrades gracefully: if statistics was never enabled, or its schema drifted,
we hand back demo values so the page still renders something meaningful.

@module koplugin.ReadingQuota.QuotaData
--]]--

local DataStorage = require("datastorage")
local LuaSettings = require("luasettings")
local lfs = require("libs/libkoreader-lfs")
local logger = require("logger")

local DB_PATH = DataStorage:getSettingsDir() .. "/statistics.sqlite3"
local SETTINGS_PATH = DataStorage:getSettingsDir() .. "/readingquota.lua"

local DEFAULT_GOAL_MINUTES = 30
local DEFAULT_GOAL_PAGES = 20

-- Same shape as statistics' own getTodayBookStats(), but against the raw
-- page_stat_data table so we don't depend on the page_stat view existing.
-- The inner GROUP BY collapses multiple visits to one page into one row, so
-- count(*) is "distinct pages touched today", not "page turns".
local SQL_TODAY = [[
    SELECT count(*),
           sum(sum_duration)
    FROM (
        SELECT sum(duration) AS sum_duration
        FROM   page_stat_data
        WHERE  start_time >= %d
        GROUP  BY id_book, page
    );
]]

local QuotaData = {}

local function startOfToday()
    local now = os.date("*t")
    return os.time() - (now.hour * 3600 + now.min * 60 + now.sec)
end

--- Formats a duration as e.g. "1 小时 5 分钟".
function QuotaData.formatDuration(seconds)
    seconds = math.floor(seconds)
    local hours = math.floor(seconds / 3600)
    local minutes = math.floor(seconds % 3600 / 60)
    if hours > 0 then
        return string.format("%d 小时 %d 分钟", hours, minutes)
    elseif minutes > 0 then
        return string.format("%d 分钟", minutes)
    else
        return string.format("%d 秒", seconds)
    end
end

-- Settings -------------------------------------------------------------------

function QuotaData:getSettings()
    if not self.settings then
        self.settings = LuaSettings:open(SETTINGS_PATH)
    end
    return self.settings
end

--- Returns { mode = "time"|"pages", minutes = n, pages = n }.
function QuotaData:getGoal()
    local settings = self:getSettings()
    return {
        mode = settings:readSetting("goal_mode", "time"),
        minutes = settings:readSetting("goal_minutes", DEFAULT_GOAL_MINUTES),
        pages = settings:readSetting("goal_pages", DEFAULT_GOAL_PAGES),
    }
end

function QuotaData:setGoalMode(mode)
    local settings = self:getSettings()
    settings:saveSetting("goal_mode", mode)
    settings:flush()
end

function QuotaData:setGoalValue(mode, value)
    local settings = self:getSettings()
    settings:saveSetting(mode == "pages" and "goal_pages" or "goal_minutes", value)
    settings:flush()
end

-- Statistics DB --------------------------------------------------------------

--- Returns { pages = n, duration = seconds } or nil plus a reason string.
function QuotaData:queryToday()
    if lfs.attributes(DB_PATH, "mode") ~= "file" then
        return nil, "no_db"
    end

    local SQ3 = require("lua-ljsqlite3/init")
    local conn
    -- On failure the second return value is the error message, not a row.
    local ok, pages_or_err, duration = pcall(function()
        conn = SQ3.open(DB_PATH, "ro")
        return conn:rowexec(string.format(SQL_TODAY, startOfToday()))
    end)
    if conn then
        pcall(function() conn:close() end)
    end
    if not ok then
        logger.warn("ReadingQuota: statistics query failed:", pages_or_err)
        return nil, "query_failed"
    end

    return {
        pages = tonumber(pages_or_err) or 0,
        duration = tonumber(duration) or 0,
    }
end

--- Today's numbers, with a demo fallback. `source` is "db" when real.
function QuotaData:getToday()
    local stats, reason = self:queryToday()
    if stats then
        return { duration = stats.duration, pages = stats.pages, source = "db" }
    end
    -- Fallback so a fresh install still shows a page worth looking at.
    return { duration = 19 * 60, pages = 24, source = reason }
end

--- Everything the page needs, in one call.
function QuotaData:getQuota()
    local today = self:getToday()
    local goal = self:getGoal()

    local done, target
    if goal.mode == "pages" then
        done, target = today.pages, goal.pages
    else
        done, target = today.duration, goal.minutes * 60
    end

    local ratio = target > 0 and done / target or 1
    if ratio > 1 then
        ratio = 1
    end

    return {
        mode = goal.mode,
        done = done,
        target = target,
        remaining = math.max(target - done, 0),
        ratio = ratio,
        today = today,
        goal = goal,
    }
end

--- Renders `done`/`target`/`remaining` in the unit the current mode uses.
function QuotaData.formatAmount(mode, value)
    if mode == "pages" then
        return string.format("%d 页", value)
    end
    return QuotaData.formatDuration(value)
end

return QuotaData
