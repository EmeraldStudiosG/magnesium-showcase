let secret = "outer"

import "tests/modules/private" as private

print(secret)
print(private.value)
print(private.reveal())
