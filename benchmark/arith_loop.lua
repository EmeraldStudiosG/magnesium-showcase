local total = 0
for i = 0, 4999999 do
    total = total + i * 3 - i / 2 + i % 7
end
print(total)
