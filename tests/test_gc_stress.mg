let sum = 0

fn make_worker(seed)
    fn work()
        let arr = []
        for j in 0..200
            push(arr, seed + j)
        end
        sum = sum + len(arr) + arr[0] + arr[len(arr) - 1]
    end
    return work
end

for i in 0..50
    let seed = i
    task.run(make_worker(seed))
end

task.run(fn()
    print("drain complete")
    print(sum)
end)

print("scheduled")
