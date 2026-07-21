// Defer scoping test

// Top-level defer fires at script exit
defer print("script exit")

print("start")

// Block-scoped defer
if true then
    defer print("leaving if block")
    print("inside if")
end

print("after if")

// LIFO order
fn test_lifo()
    defer print("defer 3")
    defer print("defer 2")
    defer print("defer 1")
    print("in function")
end

test_lifo()
print("done")