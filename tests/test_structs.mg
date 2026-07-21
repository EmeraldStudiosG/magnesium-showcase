// Struct test
struct Player
    x
    y
    name
end

fn Player.move(self, nx, ny)
    self.x = nx
    self.y = ny
end

fn Player.greet(self)
    print(_"I am {self.name} at ({self.x}, {self.y})")
end

let p = Player { x = 0, y = 0, name = "Alice" }
p.greet()

p.move(10, 20)
p.greet()