!strict

struct Box
    value: number
end

let bad_box = Box { missing = 1 }

let bad_callback: fn(number): number = fn(value: string): string
    return value
end

type Named = &<
    name: string
>

let generic: &<string, string> = &< name = "Ada" >
let bad_shape: Named = generic

let shaped: Named = &< name = "Ada" >
let missing = shaped.missing
