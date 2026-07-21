let total = 0

fn make_pair(seed)
    let base = seed * 10

    fn first()
        total = total + base + 1
    end

    fn second()
        total = total + base + 2
    end

    return first, second
end

for i in 0..20
    let first, second = make_pair(i)
    task.run(first)
    task.run(second)
end

task.run(fn()
    print(total)
end)
