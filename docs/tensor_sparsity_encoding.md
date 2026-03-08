# Encoding sparse Boolean tensors back to CNF for SharpSAT-TD

This note explains how to encode a sparse `n`-dimensional Boolean tensor (axis size 2, one axis per SAT variable) into a CNF / WCNF instance that `sharpSAT-TD` can count.

## 1) Basic setup

Let original variables be `x_1, ..., x_n`.
A tensor entry is indexed by a full assignment `a in {0,1}^n`.

- `T(a) = 1` means assignment `a` is **allowed** (support encoding).
- `T(a) = 0` means assignment `a` is **forbidden** (conflict encoding).

In DIMACS CNF, each literal corresponds to one axis value:

- `x_i = 1`  -> literal `x_i`
- `x_i = 0`  -> literal `¬x_i`

For a full assignment `a`, define the cube

`cube(a) = (l_1 ∧ ... ∧ l_n)`

where `l_i = x_i` if `a_i=1`, else `l_i = ¬x_i`.

## 2) Direct sparse encodings

### A) Sparse zeros (few forbidden cells): one clause per forbidden assignment

For each `a` with `T(a)=0`, add one blocking clause

`(¬l_1 ∨ ... ∨ ¬l_n)`.

This is exact and often best when zeros are very sparse.

### B) Sparse ones (few allowed cells): introduce selector variables

If only a few assignments are allowed, direct DNF

`F = OR_{a in S} cube(a)`

is compact conceptually but not CNF. Use Tseitin variables:

- For each allowed assignment `a in S`, create selector `s_a` meaning `s_a -> cube(a)`.
- Encode implications with binary clauses:
  - if `a_i=1`: `(¬s_a ∨ x_i)`
  - if `a_i=0`: `(¬s_a ∨ ¬x_i)`
- Require at least one selector true: `(s_{a1} ∨ ... ∨ s_{ak})`.

This gives `k*n + 1` clauses (plus `k` new vars).

Use when `k = |S|` is small.

## 3) Compress before CNF (important for efficiency)

Naively using one clause/cube per nonzero often explodes. First compress sparse patterns into larger subcubes.

### A) Prime implicants / implicates (cube merging)

Merge assignments that differ only on some coordinates into patterns with don't-cares.
Example:

- `x1=0,x2=0,x3=0` and `x1=0,x2=0,x3=1`
- merge to `¬x1 ∧ ¬x2` (x3 is don't-care).

Then encode the merged representation instead of each point.

### B) Decision diagram route (recommended)

Build a reduced ordered BDD (or d-DNNF if available) for each tensor's support.
Then Tseitin-encode each internal node:

`v <-> ((¬x_i ∧ lo) ∨ (x_i ∧ hi))`.

This is usually dramatically smaller than pointwise encoding when there is structure.

Practical note: BDD size is dominated by variable order. For many instances, the difference between a good and bad order is exponential.

## 4) Multiple tensors

If your preprocessor outputs constraints `T_1,...,T_m`, encode each as a CNF block and conjoin all blocks.

If tensors share many variables, prefer sharing intermediate BDD/Tseitin nodes where possible to avoid duplicate subgraphs.

## 5) Weighted values

If tensors are strictly Boolean support constraints, plain CNF is enough.

If entries carry multiplicative weights, map them to WMC by introducing indicator variables and literal weights. `sharpSAT-TD` supports weighted parsing via comment lines in MC competition format (`c p weight ...`). Keep hard constraints in CNF and push multiplicative factors to weights when possible.

## 6) Practical heuristic

For each tensor:

1. Compare `#zeros` and `#ones`.
2. If very sparse on one side, use direct encoding from Section 2.
3. Otherwise build a ROBDD and Tseitin-encode.
4. Run a SAT pre-simplifier (subsumption, variable elimination, blocked clause elimination) before counting.

This hybrid strategy is usually best for model counting workloads.

## 7) Compatibility with this repository

`sharpSAT-TD` reads DIMACS CNF (`p cnf ...`) and, in weighted mode, MC competition style weight comments (`c p weight lit weight 0`). So the above encodings are directly usable after serialization.

## 8) How to construct a ROBDD from sparse tensor data

Assume your tensor is represented by its support set `S` (allowed assignments) or conflict set `Z` (forbidden assignments). Build a BDD for whichever side is sparser, then complement if needed.

1. Pick a variable order `π = (x_{π1},...,x_{πn})`.
2. Define terminal nodes `0` and `1`.
3. Recursively build nodes with memoization:
   - state is `(level, restricted_subfunction)`.
   - branch on current variable `x_{π(level)}` into low/high cofactors.
   - recursively build children `lo` and `hi`.
   - reduction rules:
     - if `lo == hi`, return that child (eliminate redundant test);
     - else unique-table lookup `(var, lo, hi)` to share isomorphic subgraphs.
4. Cache every subproblem (computed table) to avoid recomputation.

For sparse point sets, a trie-style top-down constructor is efficient:

- Insert points in sorted variable order.
- Merge identical suffix subtries aggressively using hashing.
- Convert merged trie to ROBDD by applying the same reduction rules.

This gives the canonical ROBDD for the chosen order.

## 9) How to keep the ROBDD small in practice

### A) Exploit decomposition first

If a tensor factorizes over variable blocks, encode each factor separately:

- detect connected components in the variable-interaction graph (variables are adjacent if they co-occur in any non-trivial pattern/constraint),
- build one BDD per component,
- conjoin in CNF after Tseitin.

This avoids one giant BDD and usually helps model counting.

### B) Normalize and compress support before BDD

The goal is to replace many isolated assignments by fewer *cubes* before BDD construction.
A cube is a partial assignment over variables where some coordinates are fixed and others are free.

- Example cube: `(x1=0, x2=1, x3=*, x4=*)`.
- `*` means **don't-care**: both 0 and 1 are allowed there.
- This one cube represents 4 full assignments (all combinations of `x3,x4`).

So compression means: represent your support/conflict set as a union of large cubes instead of individual points.

#### What are don't-cares, and how do I find them?

A variable is a don't-care in a local pattern if flipping that variable does not change membership in the set (support or conflict), while the other fixed coordinates stay the same.

For sparse point data, a practical detection method is iterative pair-merge:

1. Start from cubes with no don't-cares (one cube per point).
2. Group cubes by all coordinates except one coordinate `i`.
3. If two cubes are identical except that coordinate `i` has values 0 and 1, merge them by setting coordinate `i` to `*`.
4. Repeat until no merge applies.

Data-structure tip:
- Store each cube as two bitmasks per variable index: `care_mask` and `value_mask`.
  - `care_mask[i]=0` means don't-care.
  - `care_mask[i]=1` and `value_mask[i]=0/1` means fixed to 0/1.
- Then “differ in exactly one fixed bit” checks are fast bit operations.

This is analogous to Quine–McCluskey style cube combining, but you can run it greedily/locally for scalability.

#### How does cube compression work in practice?

A robust pipeline:

1. **Seed cubes** from sparse entries (`1`s or `0`s, whichever is smaller).
2. **Bucket by Hamming weight / signature** to find likely merge partners quickly.
3. **Iterative merging** to introduce don't-cares (as above).
4. **Subsumption cleanup** (remove redundant cubes; see dominated cubes below).
5. Optional: **re-expand selected coordinates** if needed for downstream constraints.

Why this helps BDDs:
- Larger cubes induce repeated cofactors and early terminal hits.
- BDD unique-table sharing increases because many branches become identical.
- Effective function complexity (from the builder's perspective) drops.

#### How do I find dominated cubes?

Given two cubes `A` and `B`, `A` **dominates** `B` if every assignment covered by `B` is also covered by `A`.
Then `B` is redundant and can be removed.

Bitmask test (with `care/value` representation):

- `A` dominates `B` iff
  1. every variable fixed by `A` is also fixed by `B` with same value, and
  2. `B` may fix additional variables, but cannot contradict `A`.

Equivalent intuition: `A` is more general (more `*`) and consistent with `B`.

Example:
- `A: (x1=0, x2=*, x3=1)`
- `B: (x1=0, x2=1, x3=1)`
`A` dominates `B`, so keep `A`, drop `B`.

Implementation strategy:
- Index cubes by fixed-literal signatures to prune candidate comparisons.
- Run dominance checks within buckets likely to overlap.
- Remove duplicates first; then dominated cubes.

#### How do I detect symmetry, and why does it help?

Two variables are (approximately) symmetric if swapping them leaves the represented set unchanged (or nearly unchanged in sampled data).

Exact checks (if feasible):
- Build a provisional symbolic form and test `f(...,xi,...,xj,...) == f(...,xj,...,xi,...)`.
- In cube representation, compare coverage under variable-swap permutation.

Scalable heuristics:
- Compare variable fingerprints:
  - frequency of appearing fixed to 0/1,
  - co-occurrence profiles with other variables,
  - impact on membership in sampled conditioning tests.
- Cluster near-identical fingerprints as candidate symmetry groups.

How symmetry helps BDD size:
- Symmetric variables should be placed adjacent and treated as a block in ordering/reordering.
- This increases isomorphic subgraphs after swapping-equivalent decisions.
- Dynamic reordering with group constraints (group sifting) preserves these gains.

Even partial symmetry detection is useful: it narrows the ordering search space and often reduces node count substantially.

Cleaner input to the builder often shrinks the BDD substantially.

### C) Use dynamic variable reordering

Use a BDD package (e.g., CUDD, Sylvan, BuDDy) and enable dynamic reordering:

- sifting (best default),
- window permutation,
- group sifting for related variables.

Reordering can reduce node count by orders of magnitude. Trigger it when node count passes a threshold.

### D) Cap blow-ups and fall back

Set limits (max nodes / max memory / max time). If exceeded:

- fall back to direct sparse encoding for the remaining part,
- or split by conditioning on a separator variable and encode branches separately.

## 10) Choosing a good variable order

There is no universally optimal order, but these heuristics are robust for sparse tensor constraints:

1. **Locality first:** put strongly interacting variables close together.
   - Build a weighted interaction graph from your tensor constraints.
   - Order by min-fill/min-degree elimination or spectral/partition-based ordering.
2. **Early discrimination:** variables that quickly separate support from non-support should appear early.
   - score variable `x` by entropy/information gain on remaining points.
3. **Keep correlated blocks contiguous:** if variables come from the same original factor/subtensor, keep that block together.
4. **Try both polarity views:** building from sparse `1`s vs sparse `0`s can favor different orders; benchmark both briefly.
5. **Portfolio short-run selection:**
   - generate several candidate orders (graph-based, random perturbations, domain-specific),
   - build BDDs under a small node/time budget,
   - continue with the best current order.

For tensor-derived constraints, a strong baseline is:

- decompose into components,
- inside each component use min-fill order,
- enable sifting after initial build.

## 11) Minimal CNF extraction from ROBDD (recipe)

After you have root node `r`:

1. Create Tseitin variable `v_u` for each non-terminal BDD node `u`.
2. For node `u` labeled by `x`, with children `lo, hi`, encode
   `v_u <-> ((¬x ∧ val(lo)) ∨ (x ∧ val(hi)))`,
   where `val(terminal0)=0`, `val(terminal1)=1`, else `val(child)=v_child`.
3. Assert root truth with unit clause `(v_r)`.

This gives a linear-size CNF in the number of BDD nodes/arcs and is typically far smaller than assignment-wise encodings.
