// Postfix ? unwraps fallible lookups and propagates lookup errors.

fn pick_name(info)
    return info<"name">?
end

fn pick_age(info)
    return info<"age">?
end

fn pick_bad_key(info)
    return info<1>?
end

fn second(items)
    return items[1]?
end

fn tenth(items)
    return items[9]?
end

fn char_at(text, index)
    return text[index]?
end

let person = &< name = "Ada" >
let name, name_err = pick_name(person)
print(name)
print(name_err)

let age, age_err = pick_age(person)
print(age)
print(age_err)

let bad_key, bad_key_err = pick_bad_key(person)
print(bad_key)
print(bad_key_err)

let values = [10, 20, 30]
let value, value_err = second(values)
print(value)
print(value_err)

let missing_value, missing_value_err = tenth(values)
print(missing_value)
print(missing_value_err)

let ch, ch_err = char_at("magnesium", 2)
print(ch)
print(ch_err)

let missing_ch, missing_ch_err = char_at("mg", 9)
print(missing_ch)
print(missing_ch_err)
