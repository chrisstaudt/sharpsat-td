# Python interface for tensor -> CNF encoding

This repository now includes a C++ shared library and a thin Python wrapper for encoding Boolean tensors of shape `(2,)*n` into CNF.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

This builds `libsharpsat_td_tensor2cnf.so`.

## Python usage (NumPy / Torch)

```python
import numpy as np
from sharpsat_td_tensor import TensorToCNFEncoder, EncodingOptions

T = np.zeros((2, 2, 2), dtype=np.uint8)
T[0, 1, 1] = 1
T[1, 0, 1] = 1

enc = TensorToCNFEncoder(lib_path="build/libsharpsat_td_tensor2cnf.so")
cnf, stats = enc.encode(
    T,
    EncodingOptions(
        max_points=100000,
        max_clauses=2000000,
        max_literals=12000000,
        max_runtime_ms=120000,
        mode="auto",  # auto | block_zeros | selectors_ones
    ),
)

with open("tensor.cnf", "w", encoding="utf-8") as f:
    f.write(cnf)
print(stats)
```

For `torch.Tensor`, pass the tensor directly; the wrapper moves it to CPU and converts to bool internally.

## Efficiency and safety controls

The C++ encoder enforces hard limits to avoid oversized CNFs and long runs:

- `max_points`: max sparse entries used for direct encoding.
- `max_clauses`, `max_literals`: hard CNF size caps.
- `max_new_vars`: cap for auxiliary selector vars.
- `max_runtime_ms`: runtime cap in encoder.

If limits are exceeded, encoding fails fast with an explicit error.

## Encoding strategy

- `block_zeros`: one blocking clause per forbidden assignment (good when zeros are sparse).
- `selectors_ones`: Tseitin selector encoding for allowed assignments (good when ones are sparse).
- `auto`: picks the feasible mode with smaller estimated literal count.

This keeps formulas compact while preserving exact sparsity semantics.
