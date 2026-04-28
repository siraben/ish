import hashlib
import os

path = "/tmp/bench-hot-paths/python-file-io.bin"
chunk = bytes((i * 19 + i // 3) & 0xFF for i in range(4096))
rounds = 1024

with open(path, "wb", buffering=0) as f:
    for _ in range(rounds):
        f.write(chunk)

h = hashlib.sha256()
with open(path, "rb", buffering=0) as f:
    while True:
        data = f.read(4096)
        if not data:
            break
        h.update(data)

os.unlink(path)
print(h.hexdigest()[:16])
