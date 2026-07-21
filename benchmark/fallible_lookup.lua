local config = { value = 7 }
local total = 0

for i = 0, 2999999 do
    local value = config["value"]
    if value == nil then error("missing value") end
    total = total + value
end

print(total)
