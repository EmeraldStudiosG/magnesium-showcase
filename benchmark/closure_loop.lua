local function make_accumulator(seed)
    local total = seed
    return function(n)
        total = total + n
        return total
    end
end

local add = make_accumulator(0)
local check = 0
for i = 0, 4999999 do
    check = add(i)
end
print(check)
