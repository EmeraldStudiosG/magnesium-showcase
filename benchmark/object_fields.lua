local Vec2 = {}
Vec2.__index = Vec2

function Vec2.new(x, y)
    return setmetatable({ x = x, y = y }, Vec2)
end

function Vec2:move(dx, dy)
    self.x = self.x + dx
    self.y = self.y + dy
end

local pos = Vec2.new(0, 0)
for i = 0, 999999 do
    pos:move(1, 2)
end
print(pos.x + pos.y)
