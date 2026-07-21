local text = "magnesium"
local total = 0

for i = 0, 4999999 do
    total = total + string.len(text)
end

print(total)
