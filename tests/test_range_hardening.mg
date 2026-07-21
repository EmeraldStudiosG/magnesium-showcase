let sum = 0
for i in 0..5
    sum = sum + i
end
print(sum)

let odds = 0
for i in 0..10
    if i % 2 == 0 then
        continue
    end
    odds = odds + i
end
print(odds)

fn add_one(n)
    return n + 1
end

let called = 0
for i in 0..5
    called = called + add_one(i)
end
print(called)

let flags = []
for i in 0..=10
    push(flags, true)
end

for i in 2..=10
    if flags[i] then
        let j = i * 2
        loop
            if j > 10 then
                break
            end
            flags[j] = false
            j = j + i
        end
    end
end

print(flags[2])
print(flags[4])
print(flags[9])

let max_seen = 0
for i in 2147483647..=2147483647
    max_seen = i
end
print(max_seen)
