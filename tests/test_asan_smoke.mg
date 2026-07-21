import "tests/modules/math" as math

let arr = []
push(arr, 10)
push(arr, 20)
push(arr, 30)

let total = math.square(4)

fn make_task(seed)
    let base = seed
    fn task()
        total = total + base + len(arr)
    end
    return task
end

task.run(make_task(1))
task.run(make_task(2))
task.run(fn()
    print(total)
end)

print("asan smoke")
