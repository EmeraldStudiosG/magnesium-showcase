enum Status { Ok, Error, Pending }

print(Status<"Ok">)
print(Status<"Error">)
print(Status<"Pending">)

// Use in logic
let state = Status<"Ok">
if state == Status<"Ok"> then
    print("All good!")
end

// Enum as readable state machine
enum Color { Red, Green, Blue }
let c = Color<"Green">
if c == Color<"Red"> then
    print("red")
elseif c == Color<"Green"> then
    print("green")
elseif c == Color<"Blue"> then
    print("blue")
end
