local items = {}
for i = 0, 2999999 do
    items[i + 1] = i
end

local total = 0
for i = 0, 2999999 do
    total = total + items[i + 1]
end
print(total)
