print("Main thread: Spawning task...")

fn background_work()
    // some loop
    let sum = 0
    for i in 0..10000
        sum = sum + i
    end
    print("Background task finished with sum:")
    print(sum)
end

task.run(background_work)
print("Main thread: Task spawned! Doing my own work...")

// task.run() queues cooperative work that runs after the current script returns.
for i in 0..100000
    let temp = i * 2
end
print("Main thread: Exiting.")
