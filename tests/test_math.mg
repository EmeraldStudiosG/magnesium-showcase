// Basic arithmetic test
print(10 + 5)
print((10 + 5) * 2)
print(100 / 4)
print(10 - 3)
print(17 % 5)

let a = 9
let b = 4
print(a * b)
print(a - b)
print((a + b) % 5)
print(-a)

let acc = 10
let extra = 3
acc = acc + extra * 2 - extra
print(acc)

let doubled = 2
doubled = doubled + doubled
print(doubled)

let div_seen = 0
for i in 9..10
    div_seen = i / 3
end
print(div_seen)

let negative_mod_value = -17
let positive_mod_value = 17
let large_negative_mod_value = -2147483647
print(negative_mod_value % 3)
print(negative_mod_value % -3)
print(positive_mod_value % -3)
print(large_negative_mod_value % 16)
print(positive_mod_value % 1)
print(positive_mod_value % 17)
