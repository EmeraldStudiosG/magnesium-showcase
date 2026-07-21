local function mix(a, b, c)
    return a + b * 2 - c
end

local total = 0
for i = 0, 9999999 do
    total = total + mix(i, 3, 1)
end
print(total)
