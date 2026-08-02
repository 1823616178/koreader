--[[--
A segmented progress bar, in the spirit of a terminal's `████████░░░░░░`.

Solid blocks instead of a continuous fill: on e-ink the hard edges stay
readable at any refresh mode, and a partial redraw only ever has to flip whole
blocks rather than a moving fill edge.

@module koplugin.ReadingQuota.QuotaBar
--]]--

local Blitbuffer = require("ffi/blitbuffer")
local Geom = require("ui/geometry")
local Widget = require("ui/widget/widget")
local Screen = require("device").screen

local QuotaBar = Widget:extend{
    width = nil,
    height = Screen:scaleBySize(30),
    segments = 20,
    gap = Screen:scaleBySize(4),
    percentage = 0, -- 0..1, share of the goal already done
    fillcolor = Blitbuffer.COLOR_BLACK,
    emptycolor = Blitbuffer.COLOR_LIGHT_GRAY,
}

function QuotaBar:getSize()
    return Geom:new{ w = self.width, h = self.height }
end

function QuotaBar:setPercentage(percentage)
    self.percentage = percentage
end

--- How many blocks to fill. Any progress at all lights up at least one block,
--- and only a completed goal lights up the last one.
function QuotaBar:getFilledCount()
    if self.percentage <= 0 then
        return 0
    elseif self.percentage >= 1 then
        return self.segments
    end
    local filled = math.floor(self.percentage * self.segments + 0.5)
    return math.min(math.max(filled, 1), self.segments - 1)
end

function QuotaBar:paintTo(bb, x, y)
    self.dimen = Geom:new{ x = x, y = y, w = self.width, h = self.height }

    local filled = self:getFilledCount()
    -- Each block owns `pitch` pixels and gives up `gap` of them to its right
    -- neighbour. Deriving both edges from the same rounding keeps the blocks
    -- evenly sized and makes the last one land exactly on x + width.
    local pitch = (self.width + self.gap) / self.segments
    for i = 0, self.segments - 1 do
        local left = math.floor(i * pitch + 0.5)
        local right = math.floor((i + 1) * pitch + 0.5) - self.gap
        local block_w = math.max(right - left, 1)
        bb:paintRect(x + left, y, block_w, self.height,
            i < filled and self.fillcolor or self.emptycolor)
    end
end

return QuotaBar
