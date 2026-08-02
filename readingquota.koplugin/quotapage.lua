--[[--
The full-screen "today's reading quota" page.

Takes over the whole screen like KOReader's own book map / page browser do,
so it reads as its own app screen rather than a popup over the reader.

@module koplugin.ReadingQuota.QuotaPage
--]]--

local Blitbuffer = require("ffi/blitbuffer")
local ButtonTable = require("ui/widget/buttontable")
local CenterContainer = require("ui/widget/container/centercontainer")
local Device = require("device")
local Font = require("ui/font")
local FrameContainer = require("ui/widget/container/framecontainer")
local Geom = require("ui/geometry")
local GestureRange = require("ui/gesturerange")
local HorizontalGroup = require("ui/widget/horizontalgroup")
local InputContainer = require("ui/widget/container/inputcontainer")
local LeftContainer = require("ui/widget/container/leftcontainer")
local LineWidget = require("ui/widget/linewidget")
local RightContainer = require("ui/widget/container/rightcontainer")
local Size = require("ui/size")
local SpinWidget = require("ui/widget/spinwidget")
local TextWidget = require("ui/widget/textwidget")
local TitleBar = require("ui/widget/titlebar")
local UIManager = require("ui/uimanager")
local VerticalGroup = require("ui/widget/verticalgroup")
local VerticalSpan = require("ui/widget/verticalspan")
local QuotaBar = require("quotabar")
local QuotaData = require("quotadata")
local Screen = Device.screen

local QuotaPage = InputContainer:extend{
    title = "今日阅读额度",
}

function QuotaPage:init()
    self.width = Screen:getWidth()
    self.height = Screen:getHeight()
    self.dimen = Geom:new{ x = 0, y = 0, w = self.width, h = self.height }
    self.covers_fullscreen = true -- hint for UIManager:_repaint()

    self.h_padding = Screen:scaleBySize(24)
    self.content_width = self.width - 2 * self.h_padding
    self.row_height = Screen:scaleBySize(36)

    self.face_big = Font:getFace("cfont", 30)
    self.face_note = Font:getFace("cfont", 18)
    self.face_label = Font:getFace("cfont", 17)
    self.face_value = Font:getFace("cfont", 17)

    self:registerKeyEvents()
    if Device:isTouchDevice() then
        self.ges_events.MultiSwipe = {
            GestureRange:new{ ges = "multiswipe", range = self.dimen },
        }
    end

    self.quota = QuotaData:getQuota()
    self[1] = self:buildLayout()
end

function QuotaPage:registerKeyEvents()
    if Device:hasKeys() then
        self.key_events.Close = { { Device.input.group.Back } }
    end
end

-- Layout ---------------------------------------------------------------------

--- A label on the left, its value flushed right, spanning the content width.
function QuotaPage:buildRow(label, value)
    local half = math.floor(self.content_width / 2)
    return HorizontalGroup:new{
        LeftContainer:new{
            dimen = Geom:new{ w = half, h = self.row_height },
            TextWidget:new{
                text = label,
                face = self.face_label,
                max_width = half - Size.padding.default,
            },
        },
        RightContainer:new{
            dimen = Geom:new{ w = self.content_width - half, h = self.row_height },
            TextWidget:new{
                text = value,
                face = self.face_value,
                max_width = self.content_width - half - Size.padding.default,
            },
        },
    }
end

function QuotaPage:buildHeadline()
    local quota = self.quota
    if quota.ratio >= 1 then
        return "今日目标已达成"
    end
    return "还剩 " .. QuotaData.formatAmount(quota.mode, quota.remaining)
end

function QuotaPage:buildSourceText()
    local source = self.quota.today.source
    if source == "db" then
        return "统计插件"
    elseif source == "no_db" then
        return "示例数据（统计插件未启用）"
    else
        return "示例数据（数据库读取失败）"
    end
end

function QuotaPage:buildContent()
    local quota = self.quota
    local span = VerticalSpan:new{ width = Screen:scaleBySize(12) }
    local big_span = VerticalSpan:new{ width = Screen:scaleBySize(24) }

    self.bar = QuotaBar:new{
        width = self.content_width,
        percentage = quota.ratio,
    }

    return VerticalGroup:new{
        align = "center",
        TextWidget:new{
            text = self:buildHeadline(),
            face = self.face_big,
            max_width = self.content_width,
        },
        big_span,
        self.bar,
        span,
        HorizontalGroup:new{
            LeftContainer:new{
                dimen = Geom:new{ w = math.floor(self.content_width / 2), h = self.row_height },
                TextWidget:new{
                    text = "已读 " .. QuotaData.formatAmount(quota.mode, quota.done),
                    face = self.face_note,
                },
            },
            RightContainer:new{
                dimen = Geom:new{ w = math.ceil(self.content_width / 2), h = self.row_height },
                TextWidget:new{
                    text = "目标 " .. QuotaData.formatAmount(quota.mode, quota.target),
                    face = self.face_note,
                },
            },
        },
        big_span,
        LineWidget:new{
            dimen = Geom:new{ w = self.content_width, h = Size.line.medium },
            background = Blitbuffer.COLOR_GRAY,
        },
        span,
        self:buildRow("今日阅读时长", QuotaData.formatDuration(quota.today.duration)),
        self:buildRow("今日阅读页数", string.format("%d 页", quota.today.pages)),
        self:buildRow("完成度", string.format("%d%%", math.floor(quota.ratio * 100 + 0.5))),
        self:buildRow("数据来源", self:buildSourceText()),
        big_span,
        self:buildButtons(),
    }
end

function QuotaPage:buildButtons()
    local mode_text = self.quota.mode == "pages" and "按页数计" or "按时长计"
    return ButtonTable:new{
        width = self.content_width,
        show_parent = self,
        buttons = {
            {
                { text = mode_text, callback = function() self:toggleMode() end },
                { text = "设定目标", callback = function() self:editGoal() end },
            },
            {
                { text = "刷新", callback = function() self:refresh() end },
                { text = "关闭", callback = function() self:onClose() end },
            },
        },
    }
end

function QuotaPage:buildLayout()
    self.title_bar = TitleBar:new{
        width = self.width,
        fullscreen = true,
        align = "center",
        title = self.title,
        subtitle = os.date("%Y-%m-%d"),
        with_bottom_line = true,
        close_callback = function() self:onClose() end,
        show_parent = self,
    }

    local body_height = self.height - self.title_bar:getSize().h

    return FrameContainer:new{
        width = self.width,
        height = self.height,
        background = Blitbuffer.COLOR_WHITE,
        bordersize = 0,
        padding = 0,
        margin = 0,
        VerticalGroup:new{
            align = "center",
            self.title_bar,
            CenterContainer:new{
                dimen = Geom:new{ w = self.width, h = body_height },
                self:buildContent(),
            },
        },
    }
end

-- Actions --------------------------------------------------------------------

--- Re-reads the data and rebuilds the layout in place.
function QuotaPage:refresh()
    self.quota = QuotaData:getQuota()
    self[1]:free()
    self[1] = self:buildLayout()
    UIManager:setDirty(self, "ui")
end

function QuotaPage:toggleMode()
    QuotaData:setGoalMode(self.quota.mode == "pages" and "time" or "pages")
    self:refresh()
end

function QuotaPage:editGoal()
    local goal = self.quota.goal
    local is_pages = goal.mode == "pages"
    UIManager:show(SpinWidget:new{
        title_text = is_pages and "每日目标页数" or "每日目标时长",
        value = is_pages and goal.pages or goal.minutes,
        value_min = 1,
        value_max = is_pages and 500 or 600,
        value_step = 5,
        value_hold_step = is_pages and 25 or 30,
        unit = is_pages and "页" or "分钟",
        ok_text = "保存",
        cancel_text = "取消",
        callback = function(spin)
            QuotaData:setGoalValue(goal.mode, spin.value)
            self:refresh()
        end,
    })
end

function QuotaPage:onClose()
    UIManager:close(self)
    return true
end

function QuotaPage:onMultiSwipe()
    self:onClose()
    return true
end

function QuotaPage:onCloseWidget()
    -- A full-screen page leaves ghosting behind on e-ink, so flush it on the way out.
    UIManager:setDirty(nil, "full")
end

return QuotaPage
