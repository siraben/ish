import hashlib

size = 1024 * 1024
rounds = 24
src = bytearray((i * 29 + i // 11) & 0xFF for i in range(size))
dst = bytearray(size)
view_src = memoryview(src)
view_dst = memoryview(dst)

checksum = 0
for i in range(rounds):
    view_dst[:] = view_src
    dst[i::4096] = bytes(((b + i) & 0xFF) for b in dst[i::4096])
    checksum += sum(dst[::8192])

digest = hashlib.sha256(dst).hexdigest()
print(f"{digest[:16]} {checksum}")
