# Passed Tree Decomposition Usage

This document traces how the `sspp::TreeDecomposition` object passed into the solver is used.

## 1) Construction and passing into the solver

In `main`, a tree decomposition is computed from the preprocessed primal graph and then passed to `Solver::solve(...)` as `tdecomp`.

- Unweighted path: `tdecomp` is built with `sspp::decomp::Treedecomp(...)` and passed to `theSolver.solve(ins, tdecomp)`.  
- Weighted path (double): same pattern.  
- Weighted path (arbitrary precision): same pattern.

So `solve(...)` receives the decomposition by const reference in all solve modes.

## 2) Solver entry point: decomposition consumed once during initialization

`Solver::solve(const sspp::Instance&, const sspp::TreeDecomposition&)` uses the passed decomposition only in one place:

- `Instance<T_num>::PrepareTWScore(tdec, config_.decomp_weight, config_.weight_mode);`

This happens after preprocessing/loading and before search (`countSAT()`), so the decomposition is used to initialize decision heuristic scores.

## 3) How `PrepareTWScore` transforms decomposition into heuristic values

`Instance::PrepareTWScore(...)` does the following with the passed decomposition:

1. Gets decomposition width via `tdec.Width()`.
2. Gets an ordering via `tdec.GetOrd()`.
3. Converts ordering to normalized per-variable scores in `[0, 1]` and stores them in `extra_score`.
4. Scales these scores by a coefficient derived from:
   - decomposition width,
   - number of variables,
   - configured `decomp_weight`,
   - configured `weight_mode`.
5. Writes scaled values back into `extra_score`.

Net effect: the tree decomposition influences only `extra_score`.

## 4) Where those decomposition-derived scores are used in search

During branching, the solver computes each variable's score with:

- optional VSADS frequency term,
- optional activity terms,
- `extra_score[v]` (decomposition-derived term).

`decideLiteral()` picks the variable with maximum combined score, so decomposition information influences variable selection order, not correctness logic.

## 5) Source of `GetOrd()` used above

`TreeDecomposition::GetOrd()` is computed from the decomposition tree structure by:

1. finding a centroid bag (`Centroid()`),
2. DFS traversal from the centroid (`OdDes(...)`),
3. assigning first-introduction depth-like order values per variable.

That ordering is exactly what `PrepareTWScore` uses to build `extra_score`.

## 6) Configuration knobs controlling decomposition impact

- `-decow` sets `config_.decomp_weight`.
- `-wemod` sets `config_.weight_mode`.

These parameters directly control the coefficient used to scale decomposition-based heuristic scores.


## 7) Why both `bags` and `vertex_bags` exist

The decomposition stores bag membership in two complementary layouts:

- `bags[b]` (bag -> vertices): canonical representation of each bag contents.
  - Used when code naturally iterates over a bag, e.g.
    - coverage checks in `Verify` (`for (v in bag) for (u in bag)`),
    - order construction (`Centroid`/`OdDes`/`GetOrd`),
    - chordal reconstruction (`Chordal`).
- `vertex_bags[v]` (vertex -> bags): an index to avoid scanning all bags for a vertex.
  - Used in `Verify` to check running-intersection connectivity only on bags that actually contain `v`.

Why duplication is intentional: without `vertex_bags`, `Verify` had to scan all `bs` bags for each vertex (`n`), which is expensive on large decompositions. With `vertex_bags`, this step is proportional to actual memberships instead of `n * bs`.

## Summary

The passed tree decomposition is not used for clause learning, propagation, or component caching directly. It is used to derive a variable-priority signal (`extra_score`) that is added into branching scores and therefore affects decision ordering.
