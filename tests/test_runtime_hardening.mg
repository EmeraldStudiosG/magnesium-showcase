let acc = 1.5
let left = 2
let right = 3
acc = acc + left * right
print(acc)

let divided = 1
divided = divided + 5.5 / 2
print(divided)

print(tonumber("12junk"))

let items = [1, 2]
array.extend(items, items)
print(len(items))
print(items[2])
print(items[3])
print(items[math.huge])

let binary_path = "tests/fixtures/runtime_hardening.bin"
fs.write(binary_path, string.char(65, 0, 66))
let binary = fs.read(binary_path)
print(len(binary))
print(string.byte(binary, 1))
print(string.byte(binary, 2))
print(string.byte(binary, 3))
fs.remove(binary_path)

fn deep_capture(n)
    let captured = n
    fn read_captured()
        return captured
    end
    let pad0 = 0
    let pad1 = 1
    let pad2 = 2
    let pad3 = 3
    let pad4 = 4
    let pad5 = 5
    let pad6 = 6
    let pad7 = 7
    let pad8 = 8
    let pad9 = 9
    let pad10 = 10
    let pad11 = 11
    let pad12 = 12
    let pad13 = 13
    let pad14 = 14
    let pad15 = 15
    if n <= 0 then
        return read_captured()
    end
    return deep_capture(n - 1) + read_captured() * 0
end

let stack_growth = coroutine.create(fn()
    print(deep_capture(70))
end)
coroutine.resume(stack_growth)

let rope_error = Err("Kind" + "Rope", "Mes" + "sage", "Use" + " rope")
print(rope_error)
