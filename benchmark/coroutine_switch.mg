let limit = 200000

let co = coroutine.create(fn()
    for i in 0..limit
        coroutine.yield(i)
    end
end)

let total = 0
for i in 0..limit
    total = total + coroutine.resume(co)?
end

print(total)
