local limit = 200000

local co = coroutine.create(function()
    for i = 0, limit - 1 do
        coroutine.yield(i)
    end
end)

local total = 0
for _ = 0, limit - 1 do
    local ok, value = coroutine.resume(co)
    if not ok then error(value) end
    total = total + value
end

print(total)
