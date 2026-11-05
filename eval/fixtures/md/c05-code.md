Sentinel start C05.

Call `parse_config()` before `run()`, and set `MAX_WORKERS=8` in the environment.

```python
import json


class Loader:
    def __init__(self, path):
        self.path = path

    def load(self):
        with open(self.path) as fh:
            data = json.load(fh)
            for key, value in data.items():
                if isinstance(value, dict):
                    print(key, len(value))
                else:
                    print(key, value)
        return data
```

```json
{
  "name": "rayopdf",
  "version": "0.0.0",
  "features": ["tables", "lists"],
  "limits": {"pages": 10000, "depth": 64}
}
```

```bash
./configure --prefix=/usr/local --enable-static --disable-shared --with-simd=avx2 --with-tests && make -j8 && make check
```

```
    leading spaces are kept
      and deeper indentation too

    after an inner blank line
	this line starts with a tab
```

The function `sum_values(xs)` returns `0` for an empty list.

Sentinel end C05.
