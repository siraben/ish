import hashlib
import json
import math
import random

random.seed(12345)
values = [random.random() for _ in range(250000)]
total = 0.0
for i, value in enumerate(values):
    total += math.sin(value + i % 13) * math.sqrt(value + 1.0)

payload = json.dumps({"total": total, "values": values[:1000]}, sort_keys=True).encode()
digest = hashlib.sha256(payload).hexdigest()
print(digest[:16])
