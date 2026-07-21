struct Vec2
    x
    y
end

fn Vec2.move(self, dx, dy)
    self.x = self.x + dx
    self.y = self.y + dy
end

let pos = Vec2 { x = 0, y = 0 }
for i in 0..1000000
    pos.move(1, 2)
end
print(pos.x + pos.y)
