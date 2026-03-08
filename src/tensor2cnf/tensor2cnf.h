#ifndef SHARPSAT_TD_TENSOR2CNF_H
#define SHARPSAT_TD_TENSOR2CNF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssat_encode_options {
  int64_t max_points;
  int64_t max_clauses;
  int64_t max_literals;
  int64_t max_new_vars;
  int64_t max_runtime_ms;
  int mode; /* 0=auto, 1=block_zeros, 2=selectors_ones */
} ssat_encode_options;

typedef struct ssat_cnf_result {
  char *cnf;
  char *error;
  int mode_used; /* 1=block_zeros, 2=selectors_ones */
  int64_t vars;
  int64_t clauses;
  int64_t literals;
} ssat_cnf_result;

void ssat_default_options(ssat_encode_options *opts);

int ssat_tensor_to_cnf_u8(const uint8_t *data, int64_t data_len, int ndim,
                          const int64_t *shape,
                          const ssat_encode_options *opts,
                          ssat_cnf_result *out);

int ssat_points_to_cnf_u8(const uint8_t *points, int64_t num_points, int ndim,
                          int points_are_ones,
                          const ssat_encode_options *opts,
                          ssat_cnf_result *out);

void ssat_free_result(ssat_cnf_result *out);

#ifdef __cplusplus
}
#endif

#endif
