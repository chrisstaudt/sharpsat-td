from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

import numpy as np

try:
    import torch  # type: ignore
except ImportError:  # pragma: no cover
    torch = None


class _EncodeOptions(ctypes.Structure):
    _fields_ = [
        ("max_points", ctypes.c_int64),
        ("max_clauses", ctypes.c_int64),
        ("max_literals", ctypes.c_int64),
        ("max_new_vars", ctypes.c_int64),
        ("max_runtime_ms", ctypes.c_int64),
        ("mode", ctypes.c_int),
    ]


class _Result(ctypes.Structure):
    _fields_ = [
        ("cnf", ctypes.c_char_p),
        ("error", ctypes.c_char_p),
        ("mode_used", ctypes.c_int),
        ("vars", ctypes.c_int64),
        ("clauses", ctypes.c_int64),
        ("literals", ctypes.c_int64),
    ]


@dataclass
class EncodingOptions:
    max_points: int = 200_000
    max_clauses: int = 5_000_000
    max_literals: int = 40_000_000
    max_new_vars: int = 2_000_000
    max_runtime_ms: int = 120_000
    mode: str = "auto"  # auto | block_zeros | selectors_ones

    def to_c(self) -> _EncodeOptions:
        mode_map = {"auto": 0, "block_zeros": 1, "selectors_ones": 2}
        if self.mode not in mode_map:
            raise ValueError(f"unknown mode: {self.mode}")
        return _EncodeOptions(
            self.max_points,
            self.max_clauses,
            self.max_literals,
            self.max_new_vars,
            self.max_runtime_ms,
            mode_map[self.mode],
        )


class TensorToCNFEncoder:
    def __init__(self, lib_path: Optional[str] = None):
        self._lib = ctypes.CDLL(str(self._resolve_lib(lib_path)))

        self._lib.ssat_points_to_cnf_u8.argtypes = [
            ctypes.POINTER(ctypes.c_uint8),
            ctypes.c_int64,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(_EncodeOptions),
            ctypes.POINTER(_Result),
        ]
        self._lib.ssat_points_to_cnf_u8.restype = ctypes.c_int

        self._lib.ssat_free_result.argtypes = [ctypes.POINTER(_Result)]
        self._lib.ssat_free_result.restype = None

    @staticmethod
    def _resolve_lib(lib_path: Optional[str]) -> Path:
        if lib_path:
            p = Path(lib_path)
            if not p.exists():
                raise FileNotFoundError(f"library not found: {p}")
            return p

        env = os.environ.get("SHARPSAT_TD_TENSOR_LIB")
        if env:
            p = Path(env)
            if p.exists():
                return p

        candidates = [
            Path("build/libsharpsat_td_tensor2cnf.so"),
            Path("build/src/libsharpsat_td_tensor2cnf.so"),
            Path("libsharpsat_td_tensor2cnf.so"),
        ]
        for c in candidates:
            if c.exists():
                return c
        raise FileNotFoundError(
            "Could not locate libsharpsat_td_tensor2cnf.so. Build with CMake or set SHARPSAT_TD_TENSOR_LIB."
        )

    def encode(
        self,
        tensor: Any,
        options: Optional[EncodingOptions] = None,
    ) -> Tuple[str, Dict[str, int]]:
        arr = _to_numpy_bool(tensor)
        if arr.ndim <= 0:
            raise ValueError("tensor must have at least one axis")
        if not np.all(np.array(arr.shape) == 2):
            raise ValueError("expected shape (2,)*n; every axis must be size 2")

        opts = options or EncodingOptions()
        c_opts = opts.to_c()

        ones = int(arr.sum())
        total = arr.size
        zeros = total - ones

        target_ones = True
        if opts.mode == "block_zeros":
            target_ones = False
        elif opts.mode == "selectors_ones":
            target_ones = True
        else:
            target_ones = ones <= zeros

        if target_ones:
            points = np.argwhere(arr)
            points_are_ones = 1
        else:
            points = np.argwhere(~arr)
            points_are_ones = 0

        points_u8 = np.ascontiguousarray(points.astype(np.uint8, copy=False))
        num_points = int(points_u8.shape[0])

        c_points = points_u8.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))
        out = _Result()
        ok = self._lib.ssat_points_to_cnf_u8(
            c_points,
            num_points,
            int(arr.ndim),
            points_are_ones,
            ctypes.byref(c_opts),
            ctypes.byref(out),
        )
        try:
            if not ok:
                msg = out.error.decode("utf-8") if out.error else "unknown encoder error"
                raise RuntimeError(msg)
            cnf = out.cnf.decode("utf-8") if out.cnf else ""
            stats = {
                "vars": int(out.vars),
                "clauses": int(out.clauses),
                "literals": int(out.literals),
                "mode_used": int(out.mode_used),
                "sparse_points": num_points,
            }
            return cnf, stats
        finally:
            self._lib.ssat_free_result(ctypes.byref(out))


def _to_numpy_bool(tensor: Any) -> np.ndarray:
    if isinstance(tensor, np.ndarray):
        return np.ascontiguousarray(tensor.astype(np.bool_, copy=False))

    if torch is not None and isinstance(tensor, torch.Tensor):
        return np.ascontiguousarray(tensor.detach().to("cpu").bool().numpy())

    raise TypeError("tensor must be a numpy.ndarray or torch.Tensor")


__all__ = ["TensorToCNFEncoder", "EncodingOptions"]
