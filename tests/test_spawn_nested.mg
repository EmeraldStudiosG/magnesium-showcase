fn first()
    print("task 1")
    task.run(fn()
        print("task 3")
    end)
end

fn second()
    print("task 2")
end

task.run(first)
task.run(second)
print("main")
