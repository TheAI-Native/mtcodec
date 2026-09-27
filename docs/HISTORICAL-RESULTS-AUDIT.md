# Independent audit of earlier methods and historical reproduction

Audit date: 2026-09-27. Sources read locally: DCC003 results and source package, DCC004 results, DCC005 selected-model records and source, and the extracted original manuscript. Numerical results below were independently recomputed from DCC005 `selected-models.csv` with primary-runtime Python. No experimental encoder was rerun, and no manuscript was edited by this audit.

## Critical conclusions

1. Historical size reproduction is now documented. The old manuscript's statement that raw outputs were unavailable is obsolete. DCC003's 58 clustered body sizes and 58 realized counts match the audited historical CSVs. I also independently compared DCC005 to those CSVs: **all 145 body sizes for the five historical static variants and all 87 applicable realized-table counts match exactly** across the 29 Calgary/Canterbury entries. This is reproduction of supplied frozen input bytes, not independent publisher authentication.
2. Use DCC005 numbers for the manuscript's final tables. DCC004 finished its 51-input audits and 41-file timings but its run status is **FAIL** due to a final PowerShell summary conversion exception. Preserve that history. The DCC005 selected records independently match all 765 old DCC004 selected records across 20 relevant fields, including count, byte accounting, stream fingerprint, full map, and status. This validates preservation of old outputs, not retroactive PASS of DCC004's finalizer.
3. Baselines are **Brotli-derived histogram merge/remap procedures**, not complete Brotli compressors. The matched policy changes cost functions, not the common search template. It still does not globally optimize complete serialized size and does not use an equal time or equal realized-memory budget.
4. Header accounting is strongly supported by the sampled small blocks: replacing complete-size selection with payload-only selection increases total body bytes on 16-KiB samples by **19.57% for Huffman and 27.85% for rANS**. These are size-only ablations in DCC005; do not attach new DCC005 timing claims to these variants.
5. The unfavorable whole-file Silesia result must remain: high-bits rANS is smaller than reassignment rANS, and smaller than either merge/remap rANS control in aggregate. High-bits is allowed 256 tables, while clustered methods are capped at 64; this is not matched-memory competition.
6. Reassignment does not guarantee that every iterative search result beats its initialization. In DCC005, initialization-only Huffman is smaller on **two** of the 240 sampled 256-KiB blocks (worst reversal: 352 bytes on `mr`). The aggregate benefit remains positive.

## Baseline method definitions, checked against source

Common model: 256 raw previous-byte contexts; the initial previous byte is zero. Active contexts passed to merging are in increasing byte-value order. Inactive contexts serialize as table zero. Candidate caps are `1,2,3,4,6,8,12,16,24,32,48,64`; caps are not promises of exact realized counts. Empty clusters are removed and IDs made dense. No stored fallback or outer ten-byte file frame enters primary body sizes.

**Reassignment.** Stable descending-frequency order (byte value breaks ties), round-robin initialization, at most eight update/reassignment iterations. Tables remain fixed during a pass; moves require strict cost improvement and ties retain the old assignment. Huffman uses code-length costs, rANS uses 1/256-bit normalized-frequency costs, and a missing symbol incurs the search-only 20-bit penalty. Inner moves omit table-header and map changes, so no monotone complete-size guarantee or global optimum exists. Encoding must rebuild final tables after the final map.

**Native policy.** The literal histogram clustering search from historical Brotli v1.1.0 commit `ed738e842d2fbdf2d6459e39267a633c4a9b2f5d`, retaining its population-cost estimator and map surrogate `0.5 * ClusterCostDiff`. The resulting partitions are coder-independent; only their downstream evaluation differs. This is under the study's raw previous-byte context and study serializer, not Brotli's native literal context function, format, or overall pipeline.

**Matched policy.** The same `cluster_inc.h` search/remap/reindex procedure, pair ordering, bounded pair queue, first-stage batches of 64, and subsequent merging. Population cost is replaced by the relevant own-table cost; `ClusterCostDiff` is set to zero because the study charges an actual fixed-width map outside clustering. Huffman own-table cost = exact RLE length-header bits + count-weighted lengths. rANS own-table cost = exact frequency-header bits + fixed-point normalized-frequency payload estimate. Neither terminal state, global alignment, nor map cost is charged per table. The outer selector accounts for these costs once.

**Provenance qualification.** Small vendor templates were transcribed from official browser-accessible source with retained MIT notices; archive-byte identity was not verified by retrieving a git archive. The packaged native adapter matches all **144** exact expected maps produced by an independently linked Debian libbrotli-dev 1.1.0-2+b7 library (12 fixtures × 12 caps), with all 256 small-log table entries checked. This is tested fixture equivalence, not a proof for all inputs/platforms. Native `log2` for counts >=256 can differ across math libraries on near ties. Keep this qualification in the methods or reproducibility supplement.

## Verified equations and guarantees

Let `m(k)=0` when k=1 and otherwise `256 ceil(log2 k)` map bits. Let `a_t` be the number of nonzero Huffman lengths and `z_t` the number of maximal zero runs. The exact Huffman body is

`C_H = ceil((8 + m(k) + sum_t(6 a_t + 14 z_t) + sum_cs N_cs l_{g(c),s}) / 8)` bytes.

The source ranks unpadded body bits and emits only the selected model. Upward byte rounding preserves a minimum; the first candidate with the minimum bit rank wins. Distinct bit ranks can tie in final bytes.

For normalized frequencies `f_ts` summing to 4096, own-table rANS header bits are

`h_t = 1 + min(256*13, sum_s[2 floor(log2(f_ts+1))+1])`.

With `R` actually emitted renormalization bytes, exact body size is

`C_R = ceil((8 + m(k) + sum_t h_t)/8) + 4 + R`.

Exact rANS selection emits all twelve generated candidates and retains the first minimum actual body size. The estimated-total selector ranks

`[8 ceil((8 + m(k) + sum_t h_t)/8) + 32] * 256 + sum_cs N_cs q(f_{g(c),s})`,

where `q(f)=12*256-log2_fixed(f)` is the existing integer approximation. It emits only the selected candidate. The estimated selector can choose different bytes and has **no guaranteed one-table bound or maximum regret**. Empirical small regret is not exactness.

Exact complete-size selection, successful encoding, and inclusion of cap one guarantee no larger body than the corresponding one-table candidate. This holds for each generator's exact selector and does not imply superiority over high-bits, another generator, or all possible partitions. Inner reassignment and estimated ranking do not inherit this exact guarantee.

## DCC005 whole-file sizes (unweighted mean bpb; exact counts)

| Variant | Calgary bytes | Calgary mean bpb | Canterbury bytes | Canterbury mean bpb | Silesia bytes | Silesia mean bpb |
|---|---:|---:|---:|---:|---:|---:|
| H single | 1,828,846 | 4.962498 | 1,313,588 | 4.520120 | 138,199,934 | 5.497753 |
| H reassignment | 1,458,705 | 4.028641 | 1,071,654 | 3.794755 | 105,437,394 | 4.206851 |
| H native | 1,450,352 | 3.974954 | 1,068,704 | 3.724152 | 105,464,225 | 4.205186 |
| H matched | 1,447,994 | 3.959257 | 1,067,793 | 3.694680 | 105,510,521 | 4.206597 |
| R single | 1,792,181 | 4.916934 | 1,279,011 | 4.464551 | 137,752,695 | 5.482833 |
| R high-bits | 1,469,222 | 4.269721 | 1,029,363 | 3.911389 | 102,425,189 | 4.097499 |
| R reassignment | 1,407,298 | 3.993724 | 985,893 | 3.746811 | 103,834,816 | 4.147887 |
| R native | 1,398,662 | 3.945746 | 984,099 | 3.690303 | 103,754,729 | 4.143619 |
| R matched | 1,396,109 | 3.929295 | 982,605 | 3.667232 | 103,729,972 | 4.143001 |
| Adaptive reference | 1,360,544 | 3.721721 | 925,881 | 3.328165 | 94,890,154 | 3.838324 |

Note aggregate bytes and unweighted rates can rank differently. On whole Silesia, reassignment Huffman has the smallest total bytes among the three H clustering generators, but native has the smallest unweighted mean bpb. Do not say matched cost always produces the best size.

Both native and matched controls produce smaller sizes than reassignment on **every one of the 29 Calgary/Canterbury entries in both coders**. On Silesia, reassignment is smaller than native on 2/12 Huffman files and 1/12 rANS files, and smaller than matched on 2/12 Huffman files and 1/12 rANS files. High-bits beats reassignment on 7/12 Silesia files. These counts treat conventional entries separately; `pic` and `ptt5` duplicate the same bytes.

## Compact ablation table suitable for manuscript

Entries are percentage increase in **total codec-body bytes**, relative to exact reassignment, computed as `100*(ablation_total/exact_total-1)`. All entries below are from DCC005 size records, not from pooled DCC004 data.

| Input group | H init-only | R init-only | H payload-only | R payload-only |
|---|---:|---:|---:|---:|
| Calgary whole | 1.6591% | 1.9598% | 0.9390% | 1.5162% |
| Canterbury whole | 0.7001% | 1.3760% | 0.3973% | 1.2822% |
| Silesia whole | 3.4714% | 3.6384% | 0.000249% | 0% |
| Silesia 16-KiB samples | 5.5975% | 8.0424% | 19.5745% | 27.8508% |
| Silesia 64-KiB samples | 5.1593% | 6.4760% | 4.7899% | 5.3595% |
| Silesia 256-KiB samples | 3.4838% | 4.0233% | 0.3710% | 0.4145% |

Whole Canterbury rANS payload-only increases unweighted mean bpb by **25.4990%**, but total bytes by only **1.2822%**. If emphasizing small-file penalties, state explicitly which metric is used. A weighted/unweighted mismatch would be misleading.

Payload-only preserves exactly the original candidate maps and changes only ranking. Huffman ranks actual payload bits; rANS ranks actual renormalization bytes, excluding the constant four-byte state. Thus its effect isolates header-inclusive selection, not a different candidate family. Initialization-only removes update/reassignment but still rebuilds pooled tables and uses complete-size selection.

## Experimental environment and limitations

The actual captured environment is **Microsoft Windows 10 Pro, 10.0.19045**, not Windows 11 from remembered conversation context. CPU: Intel Core i7-7700K @4.20 GHz, four cores/eight logical processors. Physical memory recorded: 34,317,426,688 bytes. MSVC executable version: **19.44.35228.0**, toolset path **14.44.35207**, x64; flags include `/O2 /std:c17 /fp:precise`; no `/GL` or `/LTCG`. Active power plan was Balanced. DCC005 timing agent should verify matching captured environment for the final paper.

Timings are warm-memory, full production encode calls: statistics, model search, permitted candidate evaluation, emission, ordinary allocations. Input reads, verification, hashes, CSV writes, and memory instrumentation are outside the interval. Decode includes header parsing, table allocation/reconstruction and internal free, with output allocation/first touch before timing. QPC and lowest allowed logical-processor affinity; calibration >=0.25 s followed by three warmups and eleven batches with deterministic shuffled order; all variation retained. Decimal MB/s is total bytes divided by sum of file/workload median seconds, not arithmetic average of MB/s. Do not carry over the manuscript's preliminary GCC figures or call the measurements universal algorithm bounds.

Requested-heap accounting is **not total process memory**. It excludes input, preallocated decode output, static storage, stack, allocator metadata/fragmentation, bookkeeping, and transient internal realloc copies; encoder output capacity counts. Decoder table storage includes metadata, not just each principal 4-KiB lookup: actual struct sizes are H 5,384 bytes/table and R 5,124 bytes/table. At 64 tables, maxima are 344,576 and 327,936 bytes, respectively; high-bits permits 256 R tables =1,311,744 bytes. These are compiler-build-specific `sizeof` measurements.

All 12 Silesia files use publisher legacy-MD5 checks inherited from prior audit. Calgary/Canterbury are frozen known bytes but lack independent archive-byte publisher authentication. 41 corpus entries represent 40 distinct inputs due to `pic`/`ptt5`. Blocks are sampled from Silesia already viewed in DCC004, not a new holdout; cross-size sets overlap and cover different byte ranges, so no isolated causal block-size claim is warranted.

## Original-manuscript claims to replace or remove

- Abstract/introduction/conclusion: replace the claim that stronger clustering, timing provenance, and block evidence remain wholly absent. They are now central results, with limits above.
- Section 2.3: remove “Neither reference clustering algorithm is included as a matched baseline.” Describe the two Brotli-derived controls exactly; JPEG XL remains related work only.
- Section 4.1: remove “raw corpus outputs and complete timing metadata were not supplied.” State freeze/reproduction and final DCC005 evidence.
- Replace old large per-file Tables 1/2 with compact whole-corpus and sampled-block results; retain per-file data in supplementary artifacts.
- Remove preliminary GCC Table 4 and old numerical claims 149.3/110.4/26.8 MB/s. Do not pool old or DCC004 timings with DCC005.
- Remove unsupported gzip/xz numerical reference rows unless separately audited invocation/version/output evidence exists. Neither DCC004 nor DCC005 supplies a rerun.
- Replace redundant Figures 5/6 with speed/size comparisons or exact-versus-estimated selection and block/header evidence.
- Generalize neither reassignment-over-high-bits dominance nor matched-cost superiority: Silesia and individual blocks give counterexamples.
- The old statement that the implementation is only whole-file is obsolete for the independent-block experiment, but do not imply an implemented interoperable container, streaming deployment, LZ integration, or per-block latency measurement.
- Future work should now emphasize unseen workloads, matched realized-memory/time budgets, stronger optimized production baselines, alternative headers, malformed-stream testing and integration. The executed baseline/ablation/block experiments are no longer proposed future work.
- The original source can be CC0 while the new experimental package retains Brotli MIT dependencies; do not call the complete new package wholly public domain.

## Files written

- `size_summary.csv` and `.json`: all 17 variants, all three whole-corpus groups and three sampled-block sizes; exact bytes, unweighted and weighted bpb, maximum realized tables/storage.
- `ablation_comparison.csv` and `.json`: exact comparisons against reassignment for init/payload/native/matched/single/high-bits; both aggregate-byte and unweighted-rate differences.
- `pairwise_counts.json`: per-entry wins/ties/losses and largest byte differences.
- `reproduction_checks.json`: verified historical counts, raw candidate status counts, and source hashes.

