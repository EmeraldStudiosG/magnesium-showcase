print("Hello from spawned VM!")
let sum = 0
for i in 0..=10
    sum += i
end
print("Spawned sum 1..10 = " + tostring(sum))
