struct Box
    value
end

let box = Box { value = 10 }
let box_calls = 0

fn current_box()
    box_calls += 1
    return box
end

current_box().value += 2
print(box_calls)
print(box.value)

let numbers = [10]
let array_calls = 0
let index_calls = 0

fn current_array()
    array_calls += 1
    return numbers
end

fn current_index()
    index_calls += 1
    return 0
end

current_array()[current_index()] += 5
print(array_calls)
print(index_calls)
print(numbers[0])

fn make_capture()
    fn capture_values()
        return null, 42
    end
    let capture_functions = [capture_values]
    let ignored, captured = capture_functions[0]()
    let escaped = null
    if true then
        fn read_capture()
            return captured
        end
        escaped = read_capture
    end
    let overwrite_closed_slot = 99
    return escaped
end

let read_capture = make_capture()
print(read_capture())

fn return_with_defer()
    defer print("outer return defer")
    if true then
        defer print("inner return defer")
        return 7
    end
    return 0
end

print(return_with_defer())

for i in 0..3
    defer print(_"loop defer {i}")
    if i == 0 then
        continue
    end
    if i == 1 then
        break
    end
end

for i in 0..2
    defer print(_"normal loop defer {i}")
end

for item in ["a", "b"]
    defer print(_"iterator defer {item}")
end

for i in 0..0
    defer print("zero-trip defer must not run")
end

fn propagate_with_defer()
    defer print("lookup defer")
    let missing = &< value = 1 ><"missing">?
    print("unreachable")
    return missing
end

let missing_value, missing_error = propagate_with_defer()
print(missing_value)

try
    defer print("catch defer")
    &< value = 1 ><"missing">?
catch caught_error
    print("caught")
end

if 1 < 2 and 3 > 2 then
    print("comparison chain")
end

let callbacks = &< next = fn(value) return value + 1 end >
print(callbacks<"next">(6))
let nested_values = &< values = [9] >
print(nested_values<"values">[0])

print(_"{true}:{false}:{null}")
print(_"{Box { value = "nested" }.value}")
print(_"{&< value = "}" ><"value">}")

fn ten_results()
    return 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
end

let r1, r2, r3, r4, r5, r6, r7, r8, r9, r10 = ten_results()
print(r1 + r10)
