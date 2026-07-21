// Hello Magnesium Example

let name = "Developer"
let @session_start = 1712500000 // Global score example

fn greet(user)
    let msg = _"Welcome to Magnesium, {user}!"
    print(msg)
end

greet(name)

let items = ["Engine", "Compiler", "VM"]
for item in items
    print(_"Loading {item}...")
end

defer print("Engine shutdown complete.")
