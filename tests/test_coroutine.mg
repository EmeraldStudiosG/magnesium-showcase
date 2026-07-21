let co = coroutine.create(fn()
    print("inside")
    coroutine.yield("pause")
    print("after")
    coroutine.yield(99)
end)

print(type(co))
print(coroutine.status(co))
print(coroutine.resume(co)?)
print(coroutine.status(co))
print(coroutine.resume(co)?)
print(coroutine.status(co))
print(coroutine.resume(co)?)
print(coroutine.status(co))

let again, again_err = coroutine.resume(co)
print(again)
print(again_err.kind)

let n = 40
let captured = coroutine.create(fn()
    n = n + 1
    coroutine.yield(n)
    n = n + 1
    coroutine.yield(n)
end)

print(coroutine.resume(captured)?)
print(coroutine.resume(captured)?)
print(n)

let outside, outside_err = coroutine.yield("bad")
print(outside)
print(outside_err.kind)

let multi = coroutine.create(fn()
    coroutine.yield("a", "b", 3)
    return "done", "tail"
end)

let a, b, c = coroutine.resume(multi)
print(a)
print(b)
print(c)

let done, tail = coroutine.resume(multi)
print(done)
print(tail)

fn first_value_from_multi()
    let local_co = coroutine.create(fn()
        coroutine.yield("first", "second")
    end)
    return coroutine.resume(local_co)?
end

let first, first_err = first_value_from_multi()
print(first)
print(first_err)
