class Vec2:
    __slots__ = ("x", "y")

    def __init__(self, x, y):
        self.x = x
        self.y = y

    def move(self, dx, dy):
        self.x = self.x + dx
        self.y = self.y + dy

pos = Vec2(0, 0)
for i in range(1000000):
    pos.move(1, 2)
print(pos.x + pos.y)
