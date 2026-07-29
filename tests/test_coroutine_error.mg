fn coroutine_failure()
    error("coroutine boom")
end

let failed = coroutine.create(coroutine_failure)
let value, resume_err = coroutine.resume(failed)
print(value)
print(resume_err.kind)
print(resume_err.message)
print(coroutine.status(failed))
