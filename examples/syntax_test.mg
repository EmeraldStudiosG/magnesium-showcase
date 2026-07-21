// Comments
// Line comment
//[[
    Block comment
    Multiple lines
]]

// Variables
let x = 42
let pi = 3.14
const max = 100
print("Variables: " + tostring(x) + ", " + tostring(pi) + ", " + tostring(max))

// Globals
let @greeting = "hello"
print("Global: " + @greeting)

// Strings
let s1 = "hello"
let s2 = " " + "world"
print("Concat: " + s1 + s2)

let score = 99
print(_"Interp: score={score}")

// Booleans and null
let alive = true
let dead = false
let nothing = null
print("Bool/null: " + tostring(alive) + ", " + tostring(dead) + ", " + tostring(nothing))

// Arithmetic
let a = 10 + 5
let b = 10 - 5
let c = 10 * 5
let d = 10 / 5
let e = 17 % 5
print("Arith: " + tostring(a) + " " + tostring(b) + " " + tostring(c) + " " + tostring(d) + " " + tostring(e))

// Assignment updates
let counter = 0
counter += 10
counter -= 3
counter *= 2
counter /= 2
counter %= 5
print("Updates: " + tostring(counter))

// Comparison
print("Compare: " + tostring(5 >= 3) + " " + tostring("abc" == "abc") + " " + tostring(1 != 2))

// Logical
print("Logic: " + tostring(true and false) + " " + tostring(true or false) + " " + tostring(not true))

// If/elseif/else
let val = 75
if val >= 90 then
    print("Grade: A")
elseif val >= 70 then
    print("Grade: B")
else
    print("Grade: C")
end

// Loop
let i = 0
loop
    i += 1
    if i > 3 then break end
end
print("Loop: " + tostring(i))

// Range loops
let range_result = ""
for i in 1..4
    range_result = range_result + tostring(i) + " "
end
print("Range(..): " + range_result)

let incl_result = ""
for i in 1..=4
    incl_result = incl_result + tostring(i) + " "
end
print("Range(..=): " + incl_result)

// Collection loops
let items = ["apple", "banana", "cherry"]
let item_str = ""
for item in items
    item_str = item_str + item + " "
end
print("Array loop: " + item_str)

let config = &< host = "localhost", port = 8080 >
let cfg_str = ""
for key, value in config
    cfg_str = cfg_str + key + "=" + tostring(value) + " "
end
print("Dict loop: " + cfg_str)

// Functions
fn add(a, b)
    return a + b
end
print("fn: " + tostring(add(7, 8)))

// Multiple returns
fn bounds()
    return 0, 100
end
let lo, hi = bounds()
print("Multi-ret: " + tostring(lo) + ", " + tostring(hi))

// First-class functions
let double = fn(x)
    return x * 2
end
print("Lambda: " + tostring(double(21)))

// Arrays
let arr = [10, 20, 30]
push(arr, 40)
print("Array: " + tostring(arr[0]) + " " + tostring(arr[3]) + " len=" + tostring(len(arr)))
print("Out of range: " + tostring(arr[99]))

// Dicts
let player = &< name = "Ada", hp = 100 >
print("Dict: " + player<"name"> + " hp=" + tostring(player<"hp">))
player<"hp"> = 90
print("Dict set: hp=" + tostring(player<"hp">))

// Structs
struct Vec2
    x
    y
end

let v = Vec2 { x = 3, y = 4 }
print("Struct: " + tostring(v.x) + ", " + tostring(v.y))

fn Vec2.magnitude(self)
    return math.sqrt(self.x * self.x + self.y * self.y)
end
print("Method: " + tostring(v.magnitude()))

// Enums
enum Color { Red, Green, Blue }
print("Enum: " + tostring(Color<"Red">))

// Defer
fn defer_test()
    defer print("deferred!")
    print("in defer_test")
end
defer_test()

// Try/catch catches Err values propagated by `?`.
fn lookup(d, key)
    return d<key>?
end

try
    let name = lookup(&< age = 30 >, "name")?
    print("got name: " + name)
catch err
    print("Caught: " + err.kind + " - " + err.message)
end

// Const violation
const immut = 42
// immut = 99  // Would be compile error

// Coroutines
let co = coroutine.create(fn()
    print("co: first yield")
    coroutine.yield()
    print("co: second yield")
    coroutine.yield()
    print("co: done")
end)

coroutine.resume(co)
coroutine.resume(co)
coroutine.resume(co)
print("Coroutine state: " + tostring(coroutine.status(co)))

// vm.spawn
let task = vm.spawn("examples/spawn_child.mg")
let ok = vm.join(task)
if ok then
    print("Spawned VM finished with OK")
else
    print("Spawned VM finished with error")
end

print("ALL SYNTAX TESTS PASSED")
