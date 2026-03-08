#include "tensor2cnf.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum EncodeMode {
  MODE_AUTO = 0,
  MODE_BLOCK_ZEROS = 1,
  MODE_SELECTORS_ONES = 2,
};

struct Limits {
  int64_t max_points;
  int64_t max_clauses;
  int64_t max_literals;
  int64_t max_new_vars;
  int64_t max_runtime_ms;
};

struct Estimate {
  int64_t vars;
  int64_t clauses;
  int64_t literals;
};

bool check_runtime(const std::chrono::steady_clock::time_point &start,
                   int64_t max_runtime_ms) {
  if (max_runtime_ms <= 0) {
    return true;
  }
  auto now = std::chrono::steady_clock::now();
  int64_t elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
  return elapsed <= max_runtime_ms;
}

bool mul_overflow(int64_t a, int64_t b, int64_t *out) {
  if (a < 0 || b < 0) {
    return true;
  }
  if (a == 0 || b == 0) {
    *out = 0;
    return false;
  }
  if (a > std::numeric_limits<int64_t>::max() / b) {
    return true;
  }
  *out = a * b;
  return false;
}

char *dup_cstr(const std::string &s) {
  char *p = static_cast<char *>(std::malloc(s.size() + 1));
  if (p == NULL) {
    return NULL;
  }
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

void set_error(ssat_cnf_result *out, const std::string &msg) {
  out->cnf = NULL;
  out->error = dup_cstr(msg);
}

bool validate_dims(int ndim, const int64_t *shape, int64_t *total,
                   std::string *error) {
  if (ndim <= 0 || ndim >= 63) {
    *error = "ndim must be in [1, 62]";
    return false;
  }
  int64_t t = 1;
  for (int i = 0; i < ndim; ++i) {
    if (shape[i] != 2) {
      *error = "all tensor axes must have size 2";
      return false;
    }
    if (mul_overflow(t, 2, &t)) {
      *error = "tensor is too large";
      return false;
    }
  }
  *total = t;
  return true;
}

Estimate estimate_block_zeros(int ndim, int64_t zeros) {
  Estimate e;
  e.vars = ndim;
  e.clauses = zeros;
  e.literals = zeros * static_cast<int64_t>(ndim);
  return e;
}

Estimate estimate_selectors_ones(int ndim, int64_t ones) {
  Estimate e;
  e.vars = ndim + ones;
  e.clauses = ones * static_cast<int64_t>(ndim) + 1;
  e.literals = ones * static_cast<int64_t>(2 * ndim + 1);
  return e;
}

bool within_limits(const Estimate &e, const Limits &lim, int ndim) {
  if (e.clauses > lim.max_clauses || e.literals > lim.max_literals) {
    return false;
  }
  int64_t new_vars = e.vars - ndim;
  return new_vars <= lim.max_new_vars;
}

int lit_for_assignment_bit(int var_index_zero_based, int bit) {
  int v = var_index_zero_based + 1;
  return bit ? v : -v;
}

int bit_at(int64_t idx, int var_pos, int ndim) {
  int shift = ndim - 1 - var_pos;
  return static_cast<int>((idx >> shift) & 1LL);
}

int choose_mode(int requested_mode, int ndim, int64_t ones, int64_t zeros,
                const Limits &lim, int *mode, Estimate *used,
                std::string *error) {
  Estimate block = estimate_block_zeros(ndim, zeros);
  Estimate sel = estimate_selectors_ones(ndim, ones);
  bool block_ok = within_limits(block, lim, ndim) && zeros <= lim.max_points;
  bool sel_ok = within_limits(sel, lim, ndim) && ones <= lim.max_points;

  if (requested_mode == MODE_BLOCK_ZEROS) {
    if (!block_ok) {
      *error = "block_zeros encoding exceeds limits";
      return 0;
    }
    *mode = MODE_BLOCK_ZEROS;
    *used = block;
    return 1;
  }
  if (requested_mode == MODE_SELECTORS_ONES) {
    if (!sel_ok) {
      *error = "selectors_ones encoding exceeds limits";
      return 0;
    }
    *mode = MODE_SELECTORS_ONES;
    *used = sel;
    return 1;
  }

  if (block_ok && sel_ok) {
    if (block.literals <= sel.literals) {
      *mode = MODE_BLOCK_ZEROS;
      *used = block;
    } else {
      *mode = MODE_SELECTORS_ONES;
      *used = sel;
    }
    return 1;
  }
  if (block_ok) {
    *mode = MODE_BLOCK_ZEROS;
    *used = block;
    return 1;
  }
  if (sel_ok) {
    *mode = MODE_SELECTORS_ONES;
    *used = sel;
    return 1;
  }

  *error = "both encoding modes exceed limits; increase limits or pre-compress";
  return 0;
}

std::string encode_from_indices(const std::vector<int64_t> &indices, int ndim,
                                int mode, const Estimate &used) {
  std::ostringstream out;
  out << "p cnf " << used.vars << " " << used.clauses << "\n";

  if (mode == MODE_BLOCK_ZEROS) {
    for (size_t p = 0; p < indices.size(); ++p) {
      int64_t idx = indices[p];
      for (int var = 0; var < ndim; ++var) {
        int bit = bit_at(idx, var, ndim);
        int lit = bit ? -(var + 1) : (var + 1);
        out << lit << ' ';
      }
      out << "0\n";
    }
  } else {
    for (size_t p = 0; p < indices.size(); ++p) {
      int64_t idx = indices[p];
      int selector = ndim + static_cast<int>(p) + 1;
      for (int var = 0; var < ndim; ++var) {
        int bit = bit_at(idx, var, ndim);
        out << -selector << ' ' << lit_for_assignment_bit(var, bit) << " 0\n";
      }
    }
    for (size_t p = 0; p < indices.size(); ++p) {
      int selector = ndim + static_cast<int>(p) + 1;
      out << selector << ' ';
    }
    out << "0\n";
  }
  return out.str();
}

bool to_limits(const ssat_encode_options *opts, Limits *out) {
  out->max_points = (opts && opts->max_points > 0) ? opts->max_points : 200000;
  out->max_clauses =
      (opts && opts->max_clauses > 0) ? opts->max_clauses : 5000000;
  out->max_literals =
      (opts && opts->max_literals > 0) ? opts->max_literals : 40000000;
  out->max_new_vars =
      (opts && opts->max_new_vars > 0) ? opts->max_new_vars : 2000000;
  out->max_runtime_ms =
      (opts && opts->max_runtime_ms > 0) ? opts->max_runtime_ms : 120000;
  return true;
}

int encode_dense_internal(const uint8_t *data, int64_t total, int ndim,
                          const ssat_encode_options *opts,
                          ssat_cnf_result *out) {
  Limits lim;
  to_limits(opts, &lim);
  int req_mode = opts ? opts->mode : MODE_AUTO;
  auto start = std::chrono::steady_clock::now();

  int64_t ones = 0;
  for (int64_t i = 0; i < total; ++i) {
    uint8_t v = data[i];
    if (v > 1) {
      set_error(out, "tensor values must be 0/1");
      return 0;
    }
    ones += (v == 1);
    if ((i & ((1 << 20) - 1)) == 0 && !check_runtime(start, lim.max_runtime_ms)) {
      set_error(out, "runtime limit exceeded while counting tensor entries");
      return 0;
    }
  }
  int64_t zeros = total - ones;

  Estimate used;
  int mode = MODE_AUTO;
  std::string choose_error;
  if (!choose_mode(req_mode, ndim, ones, zeros, lim, &mode, &used,
                   &choose_error)) {
    set_error(out, choose_error);
    return 0;
  }

  uint8_t target = (mode == MODE_BLOCK_ZEROS) ? 0 : 1;
  int64_t points = (target == 1) ? ones : zeros;
  std::vector<int64_t> indices;
  indices.reserve(static_cast<size_t>(points));

  for (int64_t i = 0; i < total; ++i) {
    if (data[i] == target) {
      indices.push_back(i);
    }
    if ((i & ((1 << 20) - 1)) == 0 && !check_runtime(start, lim.max_runtime_ms)) {
      set_error(out, "runtime limit exceeded while collecting sparse entries");
      return 0;
    }
  }

  std::string cnf = encode_from_indices(indices, ndim, mode, used);
  out->cnf = dup_cstr(cnf);
  if (out->cnf == NULL) {
    set_error(out, "out of memory while building CNF string");
    return 0;
  }
  out->error = NULL;
  out->mode_used = mode;
  out->vars = used.vars;
  out->clauses = used.clauses;
  out->literals = used.literals;
  return 1;
}

}  // namespace

void ssat_default_options(ssat_encode_options *opts) {
  if (!opts) {
    return;
  }
  opts->max_points = 200000;
  opts->max_clauses = 5000000;
  opts->max_literals = 40000000;
  opts->max_new_vars = 2000000;
  opts->max_runtime_ms = 120000;
  opts->mode = MODE_AUTO;
}

int ssat_tensor_to_cnf_u8(const uint8_t *data, int64_t data_len, int ndim,
                          const int64_t *shape,
                          const ssat_encode_options *opts,
                          ssat_cnf_result *out) {
  if (!out) {
    return 0;
  }
  out->cnf = NULL;
  out->error = NULL;
  out->mode_used = 0;
  out->vars = 0;
  out->clauses = 0;
  out->literals = 0;

  if (!data || !shape) {
    set_error(out, "null data or shape");
    return 0;
  }

  int64_t total = 0;
  std::string err;
  if (!validate_dims(ndim, shape, &total, &err)) {
    set_error(out, err);
    return 0;
  }
  if (data_len < total) {
    set_error(out, "data_len smaller than product(shape)");
    return 0;
  }

  return encode_dense_internal(data, total, ndim, opts, out);
}

int ssat_points_to_cnf_u8(const uint8_t *points, int64_t num_points, int ndim,
                          int points_are_ones,
                          const ssat_encode_options *opts,
                          ssat_cnf_result *out) {
  if (!out) {
    return 0;
  }
  out->cnf = NULL;
  out->error = NULL;
  out->mode_used = 0;
  out->vars = 0;
  out->clauses = 0;
  out->literals = 0;

  if (ndim <= 0 || ndim >= 63) {
    set_error(out, "ndim must be in [1, 62]");
    return 0;
  }
  if (num_points < 0) {
    set_error(out, "num_points must be non-negative");
    return 0;
  }
  if (num_points > 0 && !points) {
    set_error(out, "points is null");
    return 0;
  }

  int64_t total = (1LL << ndim);

  Limits lim;
  to_limits(opts, &lim);
  int req_mode = opts ? opts->mode : MODE_AUTO;

  Estimate used;
  int mode = MODE_AUTO;
  std::string choose_error;

  std::vector<int64_t> indices;
  indices.reserve(static_cast<size_t>(num_points));
  for (int64_t i = 0; i < num_points; ++i) {
    int64_t idx = 0;
    for (int j = 0; j < ndim; ++j) {
      uint8_t b = points[i * ndim + j];
      if (b > 1) {
        set_error(out, "points must contain only 0/1 bits");
        return 0;
      }
      idx = (idx << 1) | static_cast<int64_t>(b);
    }
    indices.push_back(idx);
  }

  std::sort(indices.begin(), indices.end());
  indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

  int64_t unique_points = static_cast<int64_t>(indices.size());
  int64_t ones = points_are_ones ? unique_points : (total - unique_points);
  int64_t zeros = total - ones;

  if (!choose_mode(req_mode, ndim, ones, zeros, lim, &mode, &used,
                   &choose_error)) {
    set_error(out, choose_error);
    return 0;
  }

  bool direct_use = (mode == MODE_SELECTORS_ONES && points_are_ones) ||
                    (mode == MODE_BLOCK_ZEROS && !points_are_ones);
  if (!direct_use) {
    set_error(out,
              "mode selection requires opposite sparse side; provide dense tensor for this case");
    return 0;
  }

  std::string cnf = encode_from_indices(indices, ndim, mode, used);
  out->cnf = dup_cstr(cnf);
  if (out->cnf == NULL) {
    set_error(out, "out of memory while building CNF string");
    return 0;
  }
  out->error = NULL;
  out->mode_used = mode;
  out->vars = used.vars;
  out->clauses = used.clauses;
  out->literals = used.literals;
  return 1;
}

void ssat_free_result(ssat_cnf_result *out) {
  if (!out) {
    return;
  }
  if (out->cnf) {
    std::free(out->cnf);
    out->cnf = NULL;
  }
  if (out->error) {
    std::free(out->error);
    out->error = NULL;
  }
}
