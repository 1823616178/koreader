--[[--
Compact full-screen AI credits dashboard.

One tile per provider the API actually returned, in a 2-column grid. The
connection-status tile fills the last slot when the provider count is odd,
and takes a full-width row of its own when it is even (including zero
providers, e.g. while loading or when the first fetch failed). The Kindle
client packs its cards the same way from the response, within its known
provider set. Tall grids use pages so the controls remain on screen.

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
local HorizontalSpan = require("ui/widget/horizontalspan")
local InputContainer = require("ui/widget/container/inputcontainer")
local InputDialog = require("ui/widget/inputdialog")
local InfoMessage = require("ui/widget/infomessage")
local LeftContainer = require("ui/widget/container/leftcontainer")
local LineWidget = require("ui/widget/linewidget")
local RightContainer = require("ui/widget/container/rightcontainer")
local Size = require("ui/size")
local TextWidget = require("ui/widget/textwidget")
local TitleBar = require("ui/widget/titlebar")
local UIManager = require("ui/uimanager")
local VerticalGroup = require("ui/widget/verticalgroup")
local VerticalSpan = require("ui/widget/verticalspan")
local QuotaBar = require("quotabar")
local QuotaData = require("quotadata")
local Screen = Device.screen

local QuotaPage = InputContainer:extend{
    title = "AI 额度",
}

function QuotaPage:init()
    self.width = Screen:getWidth()
    self.height = Screen:getHeight()
    self.dimen = Geom:new{ x = 0, y = 0, w = self.width, h = self.height }
    self.covers_fullscreen = true

    self.h_padding = Screen:scaleBySize(16)
    self.card_gap = Screen:scaleBySize(8)
    self.card_padding = Screen:scaleBySize(9)
    self.content_width = self.width - 2 * self.h_padding
    self.card_width = math.floor((self.content_width - self.card_gap) / 2)
    self.card_inner_width = self.card_width - 2 * (self.card_padding + Size.border.thin)
    self.line_height = Screen:scaleBySize(27)

    self.face_card_title = Font:getFace("cfont", 19)
    self.face_label = Font:getFace("cfont", 14)
    self.face_percent = Font:getFace("cfont", 16)
    self.face_detail = Font:getFace("cfont", 12)

    self.snapshot = QuotaData.emptySnapshot()
    self.loading = true
    self.error_text = nil
    self.request_started = false
    self.closed = false

    self:registerKeyEvents()
    if Device:isTouchDevice() then
        self.ges_events.MultiSwipe = {
            GestureRange:new{ ges = "multiswipe", range = self.dimen },
        }
    end

    self[1] = self:buildLayout()
end

function QuotaPage:registerKeyEvents()
    if Device:hasKeys() then
        self.key_events.Close = { { Device.input.group.Back } }
    end
end

local function textWidget(text, face, max_width)
    return TextWidget:new{
        text = text,
        face = face,
        max_width = max_width,
    }
end

function QuotaPage:buildValueLine(label, value, inner_width)
    inner_width = inner_width or self.card_inner_width
    local label_width = math.floor(inner_width * 0.42)
    local value_width = inner_width - label_width
    return HorizontalGroup:new{
        LeftContainer:new{
            dimen = Geom:new{ w = label_width, h = self.line_height },
            textWidget(label, self.face_label, label_width),
        },
        RightContainer:new{
            dimen = Geom:new{ w = value_width, h = self.line_height },
            textWidget(value, self.face_percent, value_width),
        },
    }
end

function QuotaPage:buildWindow(label, window)
    local mode = self.snapshot.displayMode
    local value = QuotaData.displayValue(window, mode)
    local value_text = value and QuotaData.formatPercent(value) or "--"
    local detail = window and QuotaData.formatReset(window.resetAt) or "暂无数据"
    if window and window.used >= self.snapshot.alertPercent then
        value_text = value_text .. " !"
    end

    return VerticalGroup:new{
        align = "left",
        self:buildValueLine(label, value_text),
        QuotaBar:new{
            width = self.card_inner_width,
            height = Screen:scaleBySize(8),
            segments = 12,
            gap = Screen:scaleBySize(2),
            percentage = value and value / 100 or 0,
            emptycolor = window and Blitbuffer.COLOR_LIGHT_GRAY or Blitbuffer.COLOR_GRAY,
        },
        VerticalSpan:new{ width = Screen:scaleBySize(3) },
        RightContainer:new{
            dimen = Geom:new{ w = self.card_inner_width, h = self.line_height },
            textWidget(detail, self.face_detail, self.card_inner_width),
        },
    }
end

function QuotaPage:buildCard(title, windows)
    local mode_label = self.snapshot.displayMode == "remaining" and "剩余" or "已用"
    local contents = {
        align = "left",
        HorizontalGroup:new{
            LeftContainer:new{
                dimen = Geom:new{ w = math.floor(self.card_inner_width * 0.72), h = self.line_height },
                textWidget(title, self.face_card_title, math.floor(self.card_inner_width * 0.72)),
            },
            RightContainer:new{
                dimen = Geom:new{ w = math.ceil(self.card_inner_width * 0.28), h = self.line_height },
                textWidget(mode_label, self.face_detail, math.ceil(self.card_inner_width * 0.28)),
            },
        },
        VerticalSpan:new{ width = Screen:scaleBySize(5) },
        self:buildWindow("5 小时", windows.five),
        VerticalSpan:new{ width = Screen:scaleBySize(5) },
        self:buildWindow("周额度", windows.seven),
    }
    if windows.monthly then
        table.insert(contents, VerticalSpan:new{ width = Screen:scaleBySize(5) })
        table.insert(contents, self:buildWindow("月额度", windows.monthly))
    end
    return FrameContainer:new{
        width = self.card_width,
        bordersize = Size.border.thin,
        padding = self.card_padding,
        margin = 0,
        background = Blitbuffer.COLOR_WHITE,
        VerticalGroup:new(contents),
    }
end

function QuotaPage:buildInfoBlock(label, value, detail, inner_width)
    inner_width = inner_width or self.card_inner_width
    return VerticalGroup:new{
        align = "left",
        self:buildValueLine(label, value, inner_width),
        LineWidget:new{
            dimen = Geom:new{ w = inner_width, h = Screen:scaleBySize(8) },
            background = Blitbuffer.COLOR_LIGHT_GRAY,
        },
        VerticalSpan:new{ width = Screen:scaleBySize(3) },
        LeftContainer:new{
            dimen = Geom:new{ w = inner_width, h = self.line_height },
            textWidget(detail, self.face_detail, inner_width),
        },
    }
end

function QuotaPage:buildInfoCard(width)
    width = width or self.card_width
    local inner_width = width - 2 * (self.card_padding + Size.border.thin)
    local api_url = QuotaData:getApiUrl()
    local host = api_url:match("^https?://([^/]+)") or "未配置"
    local state, detail
    if self.loading then
        state, detail = "连接中", api_url
    elseif self.error_text then
        state, detail = "失败", self.error_text
    else
        state, detail = "已连接", api_url
    end
    -- In the narrow grid slot a full URL would clip; the title bar already
    -- shows the host, and errors are always shown in full.
    if width < self.content_width and not self.error_text then
        detail = host
    end
    local mode = self.snapshot.displayMode == "remaining" and "剩余" or "已用"
    local generated = "快照 " .. QuotaData.formatTimestamp(self.snapshot.generatedAt)
    if QuotaData:getApiToken() then
        generated = generated .. " · 令牌已配置"
    end

    return FrameContainer:new{
        width = width,
        bordersize = Size.border.thin,
        padding = self.card_padding,
        margin = 0,
        background = Blitbuffer.COLOR_WHITE,
        VerticalGroup:new{
            align = "left",
            LeftContainer:new{
                dimen = Geom:new{ w = inner_width, h = self.line_height },
                textWidget("数据接口", self.face_card_title, inner_width),
            },
            VerticalSpan:new{ width = Screen:scaleBySize(5) },
            self:buildInfoBlock("状态", state, detail, inner_width),
            VerticalSpan:new{ width = Screen:scaleBySize(5) },
            self:buildInfoBlock("显示", mode, generated .. " · 告警 " .. QuotaData.formatPercent(self.snapshot.alertPercent), inner_width),
        },
    }
end

function QuotaPage:buildCardRow(left, right)
    local row_height = math.max(left:getSize().h, right:getSize().h)
    return HorizontalGroup:new{
        CenterContainer:new{
            dimen = Geom:new{ w = self.card_width, h = row_height },
            left,
        },
        HorizontalSpan:new{ width = self.card_gap },
        CenterContainer:new{
            dimen = Geom:new{ w = self.card_width, h = row_height },
            right,
        },
    }
end

function QuotaPage:buildButtons()
    return ButtonTable:new{
        width = self.content_width,
        show_parent = self,
        buttons = {
            {
                { text = self.loading and "加载中…" or "刷新", enabled = not self.loading,
                    callback = function() self:refresh() end },
                { text = "接口地址", callback = function() self:editApiUrl() end },
                { text = "令牌", callback = function() self:editApiToken() end },
                { text = "关闭", callback = function() self:onClose() end },
            },
        },
    }
end

function QuotaPage:buildContent(body_height)
    local providers = self.snapshot.providers or {}
    local cards = {}
    for _, provider in ipairs(providers) do
        table.insert(cards, self:buildCard(provider.title, provider.windows))
    end

    -- Pair the cards two per row. A fresh gap per row: reusing one
    -- VerticalSpan instance in several slots is not safe to assume.
    local function newGap()
        return VerticalSpan:new{ width = self.card_gap }
    end
    local rows = {}
    local i = 1
    while i <= #cards do
        if i == #cards then
            -- Odd one out shares its row with the narrow info tile.
            table.insert(rows, self:buildCardRow(
                cards[i], self:buildInfoCard(self.card_width)))
        else
            table.insert(rows, self:buildCardRow(cards[i], cards[i + 1]))
        end
        i = i + 2
    end
    if #cards % 2 == 0 then
        -- Even count (including zero): the info tile gets its own row.
        table.insert(rows, self:buildInfoCard(self.content_width))
    end
    local buttons = self:buildButtons()
    local available = body_height - buttons:getSize().h - self.card_gap
    local total = 0
    for _, row in ipairs(rows) do
        total = total + row:getSize().h + self.card_gap
    end
    local pages = { {} }
    local nav
    if total > available then
        -- Reserve navigation and controls before packing rows, so every
        -- provider stays reachable on a small screen or a large response.
        nav = ButtonTable:new{
            width = self.content_width,
            show_parent = self,
            buttons = { {
                { text = "上一页", callback = function() self:changePage(-1) end },
                { text = "下一页", callback = function() self:changePage(1) end },
            } },
        }
        available = available - nav:getSize().h - self.card_gap
    end
    local used = 0
    for _, row in ipairs(rows) do
        local height = row:getSize().h + self.card_gap
        if used > 0 and used + height > available then
            table.insert(pages, {})
            used = 0
        end
        table.insert(pages[#pages], row)
        used = used + height
    end
    self.page_count = #pages
    self.page_index = math.max(1, math.min(self.page_index or 1, #pages))
    local children = { align = "center" }
    for page_index, page_rows in ipairs(pages) do
        for _, row in ipairs(page_rows) do
            if page_index == self.page_index then
                table.insert(children, row)
                table.insert(children, newGap())
            else
                row:free()
            end
        end
    end
    if nav then
        table.insert(children, nav)
        table.insert(children, newGap())
    end
    table.insert(children, buttons)
    return VerticalGroup:new(children)
end

function QuotaPage:changePage(delta)
    self.page_index = ((self.page_index or 1) - 1 + delta) % self.page_count + 1
    self:rebuild()
end

function QuotaPage:buildLayout()
    local host = QuotaData:getApiUrl():match("^https?://([^/]+)") or "未配置"
    self.title_bar = TitleBar:new{
        width = self.width,
        fullscreen = true,
        align = "center",
        title = self.title,
        subtitle = host,
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
                self:buildContent(body_height),
            },
        },
    }
end

-- Fetch after the loading skeleton has had one paint cycle.
function QuotaPage:onShow()
    UIManager:setDirty(self, "ui")
    if not self.request_started then
        self.request_started = true
        UIManager:scheduleIn(0.1, function() self:performFetch() end)
    end
end

function QuotaPage:rebuild()
    self[1]:free()
    self[1] = self:buildLayout()
    UIManager:setDirty(self, "ui")
end

function QuotaPage:performFetch()
    if self.closed then
        return
    end
    local snapshot, err = QuotaData:fetch()
    if self.closed then
        return
    end
    if self.refresh_pending then
        self.refresh_pending = nil
        self.loading = false
        self:refresh()
        return
    end
    if snapshot then
        self.snapshot = snapshot
        self.error_text = nil
    else
        self.error_text = err
    end
    self.loading = false
    self:rebuild()
end

function QuotaPage:refresh(settings_changed)
    if settings_changed then
        self.snapshot = QuotaData.emptySnapshot()
        self.page_index = 1
    end
    if self.loading then
        if settings_changed then
            self.refresh_pending = true
        end
        return
    end
    self.loading = true
    self.error_text = nil
    self:rebuild()
    UIManager:scheduleIn(0.1, function() self:performFetch() end)
end

function QuotaPage:editApiUrl()
    local dialog
    dialog = InputDialog:new{
        title = "Usage API 地址",
        description = "可输入主机地址或完整的 /api/usage 地址",
        input = QuotaData:getApiUrl(),
        input_hint = "http://127.0.0.1:46927",
        buttons = {
            {
                {
                    text = "取消",
                    id = "close",
                    callback = function() UIManager:close(dialog) end,
                },
                {
                    text = "保存并刷新",
                    is_enter_default = true,
                    callback = function()
                        QuotaData:setApiUrl(dialog:getInputText())
                        UIManager:close(dialog)
                        self:refresh(true)
                    end,
                },
            },
        },
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

function QuotaPage:editApiToken()
    local dialog
    dialog = InputDialog:new{
        title = "Usage API 令牌",
        description = "可选。服务端要求 Bearer 认证时填写；留空并保存则清除令牌。当前令牌不会回显。",
        input = "",
        input_hint = "Bearer 令牌（可选）",
        buttons = {
            {
                {
                    text = "取消",
                    id = "close",
                    callback = function() UIManager:close(dialog) end,
                },
                {
                    text = "保存并刷新",
                    is_enter_default = true,
                    callback = function()
                        local ok, err = QuotaData:setApiToken(dialog:getInputText())
                        if not ok then
                            UIManager:show(InfoMessage:new{ text = err })
                            return
                        end
                        UIManager:close(dialog)
                        self:refresh(true)
                    end,
                },
            },
        },
    }
    UIManager:show(dialog)
    dialog:onShowKeyboard()
end

function QuotaPage:onClose()
    self.closed = true
    UIManager:close(self)
    return true
end


function QuotaPage:onMultiSwipe()
    return self:onClose()
end

function QuotaPage:onCloseWidget()
    self.closed = true
    UIManager:setDirty(nil, "full")
end

return QuotaPage
