# DCC005 independent evidence audit for manuscript revision

Audit date: 2026-09-27. This audit reads the frozen DCC005 evidence and recomputes arithmetic; it does not repeat timing experiments, tune a codec, or change evidence files.

## Verified evidence

Source root: `evidence/DCC005-results-20260926-112604-bdea7596/DCC005-20260926-112604-bdea7596`.

`audit_dcc005.py` independently checked:

- 13,107 selected rows against their per-member raw audit CSVs and corresponding instrumented-memory records.
- All non-adaptive selected streams' byte size against count, map, table, padding, payload, and terminal-state accounting. The adaptive stream has no invented component decomposition.
- All 74,016 raw candidate records' recorded correctness/accounting status and size decomposition.
- All 4,626 rANS selections (771 members × 3 generators × exact/estimated) against independently recomputed first-minimum winners from the corresponding 12-candidate pool. Exact selection minimizes emitted body bytes; estimated selection minimizes fixed-point payload plus the byte-aligned header and four-byte state. Chosen map, requested/realized table count, and size agree.
- All 1,356 timing tasks' Q1, median, and Q3 against all 14,916 raw measurement batches, computing per-operation seconds directly from QPC ticks / frequency / iteration count. Every task has 11 measurements.
- Every published corpus/block aggregate against independent aggregation of selected models, timing medians, and memory records.

All checks pass. These checks establish consistency of retained finite evidence; they are not a new correctness proof, malicious-stream audit, or independent execution of the codec. SHA-256 identities of principal inputs are recorded in `audit_results.json`.

## Aggregation definitions that must appear in the manuscript

For member i, input length n_i and complete codec body length c_i:

- Member rate: `8 c_i / n_i` bits per byte.
- Unweighted mean member rate: `(1/m) sum_i 8 c_i/n_i`.
- Byte-weighted rate: `8 sum_i c_i / sum_i n_i`.
- A relative penalty based on mean bpb is `100 × (mean_bpb_R / mean_bpb_control − 1)`. It is **not** the mean of per-member percentage penalties.
- Total-body penalty is `100 × (sum c_R / sum c_control − 1)`; this equals the relative penalty of byte-weighted bpb for matched inputs.
- Aggregate throughput is `sum input bytes / sum workload median seconds / 10^6` decimal MB/s. For block results, one workload is the sequential fresh encoding/decoding of twenty independently reset blocks from one file at one nominal size.
- Aggregate encoder speedup R/control is `sum median_time_control / sum median_time_R`. It is not an arithmetic or geometric mean of per-file speedups.
- Block table means average 240 members per nominal size. Because every file contributes twenty members, this also equals an equal-weight mean of twelve file-group mean bpb values. Final partial blocks have their actual lengths.
- Requested heap and decoder-table memory columns are maxima across individual members, not sums and not total RAM. Encoder output capacity counts toward requested heap. Harness-held inputs/expected streams/output buffers, preallocated decode output, allocator overhead, stack, statics, fragmentation and realloc copy transients do not.

The previously quoted +2.136/+1.195/+0.585% exact R/N block penalties are ratios of unweighted means. Their corresponding total-body penalties are +2.165/+1.190/+0.562%. Both are valid if explicitly labeled; they cannot silently substitute for each other.

## Main fair comparison: same estimated selector for all generators

R = reassignment; N = native-policy merge/remap; M = coder-cost-matched merge/remap. All use the same twelve requested caps `{1,2,3,4,6,8,12,16,24,32,48,64}`, rANS normalization 4096, and the study's serializer. Table cap equality does not imply equal realized table storage or equal search work.

| Nominal size | Mean-bpb penalty R/N | Encoder speedup R/N | Mean-bpb penalty R/M | Encoder speedup R/M |
|---|---:|---:|---:|---:|
| 16 KiB | +2.135961% | 18.2794× | +2.868177% | 52.1283× |
| 64 KiB | +1.195549% | 11.9730× | +1.751703% | 35.4793× |
| 256 KiB | +0.585319% | 8.18964× | +1.005171% | 24.6283× |

These are comparisons of full implemented encode paths on sampled workloads, including fresh counts, clustering, selection, final stream emission, and relevant allocations. They do not prove an algorithm-independent complexity advantage or a global Pareto frontier. N and M are controls derived from Brotli merge/remap policy; **neither is the complete Brotli compressor**. Avoid phrases such as “18× faster than Brotli.”

`main_block_estimated_table_9rows.csv` gives bpb, encode/decode decimal MB/s, maximum additional requested encoder/decode heap in KiB, and maximum decoder-table KiB for all nine size/generator combinations. Decode throughput is close: R 124.15/130.05/126.50 MB/s, N 121.19/128.47/124.32, M 125.69/132.06/128.03. This is principally an encoder trade-off, not a decoder-speed improvement claim.

At 16 KiB, maximum decoder-table storage is R 160.125 KiB versus N 120.094 and M 115.090; at 64 KiB it is R 320.25 versus N 240.188 and M 215.168. At 256 KiB all reach 320.25 KiB. Reassignment's lower encoder requested-heap peaks do not mean uniformly lower decoder table memory.

With exact emission-based selection for all three generators, R/N encoder speedups are 11.6917/6.85320/3.81124× and R/M are 32.9078/19.2827/10.1867×. This supports that the generator comparison does not arise solely by giving R a cheaper outer selector.

## File-group scatter

`scatter_estimated_vs_native_36_points.csv` has twelve file groups at each of three block sizes. A point consists of twenty independent block models from one file, not an individual block timing. Use `relative_unweighted_mean_bpb_penalty_percent` on one axis and `encode_speedup` on the other; encode speedup uses the corresponding one workload's median encode times.

- R is faster in all 36 observed file-size groups: 4.71079–22.8776×.
- Mean-bpb differences range from −0.525701% to +4.259802%.
- R has smaller mean rate in three groups: `mr` at 64 KiB (−0.049521%), `mr` at 256 KiB (−0.525701%), and `x-ray` at 256 KiB (−0.204266%). Therefore use “mean penalty overall,” not “every workload sacrifices size.”
- Per size, speedup ranges are 8.9565–22.8776× (16), 8.6203–14.9766× (64), 4.7108–12.9806× (256).
- Exact-selector companion data are in `scatter_exact_vs_native_36_points.csv`; all six comparison/mode combinations are in `per_file_generator_comparisons.csv`.

## Estimated versus exact selection within each generator

The denominator is **761 non-synthetic members**: 41 whole-corpus entries plus 720 blocks. This is not 761 independent source files. Calgary `pic` and Canterbury `ptt5` are byte-identical. Across block sizes, sampled coverage overlaps.

| Generator | Size-different members | Total added bytes across 761 entries | Maximum individual added bytes | Maximum individual percentage |
|---|---:|---:|---:|---:|
| R | 6 | 60 | 25 | 0.0442635% |
| N | 2 | 11 | 10 | 0.0149566% |
| M | 0 | 0 | 0 | 0% |

For R the 25-byte loss occurs twice in the table because identical `pic` and `ptt5` occur in separate corpus entries; the other four losses are 1, 7, 1, and 1 bytes on sampled blocks. N losses are 10 bytes on `mr`/256 KiB and 1 byte on `samba`/16 KiB. The 10-byte N case has a smaller relative percentage than its 1-byte case.

Observed maximum losses are descriptive, not a regret bound. Exactness refers only to the generated finite candidate set. Matching compressed size does not by itself imply identical chosen models or streams; those can be examined separately in `exact_estimated_all_members.csv`.

| Whole corpus | R estimated/exact encoder speedup | N estimated/exact | M estimated/exact |
|---|---:|---:|---:|
| Calgary-18 | 3.76237× | 1.25279× | 1.09025× |
| Canterbury-11 | 5.38398× | 1.45285× | 1.14874× |
| Silesia | 9.34785× | 4.12761× | 2.52364× |

On 16/64/256 KiB samples, the same within-generator speedups are R 1.6058/1.8778/2.5508×, N 1.0271/1.0748/1.1871×, M 1.0137/1.0206/1.0550×. The contribution of outer repeated emission therefore differs sharply by generator; do not present a speedup of estimated selection as if universally 9×.

## Whole-file results and unfavorable controls

Estimated R/N/M whole mean bpb respectively:

| Corpus | R | N | M |
|---|---:|---:|---:|
| Calgary-18 | 3.993745 | 3.945746 | 3.929295 |
| Canterbury-11 | 3.746846 | 3.690303 | 3.667232 |
| Silesia | 4.147887 | 4.143619 | 4.143001 |

Fair estimated encoder speedups R/N are 10.7517/9.89017/2.66989× across these corpora; R/M are 30.1235/28.2539/5.51587×. All DCC005 timing data are one session. Do not combine with DCC004 timing samples.

On whole Silesia, high-bits rANS mean bpb 4.097499 beats all three clustered methods. Its byte-weighted rate is 3.866222 versus R 3.919430. High-bits can use 256 tables; maximum actual decoder-table storage is 1,311,744 bytes versus the clustered maximum 327,936 bytes, four times larger. This is not an equal-memory control. Whole-file high-bits is not timed by DCC005, so this package cannot supply a whole-file timing comparison against it.

Adaptive order-1 mean bpb 3.721721/3.328165/3.838324 across Calgary/Canterbury/Silesia is also lower than R/N/M. Its model storage is 132,096 bytes, and DCC005 does not time it. Preserve it as a different-model reference rather than imply the clustered codec is universally compression-optimal.

## Environment and timing qualifications

The actual DCC005 capture says **Microsoft Windows 10 Pro 10.0.19045, build 19045**, not Windows 11. CPU: Intel Core i7-7700K @ 4.20 GHz, four cores/eight logical processors. Physical RAM: 34,317,426,688 reported bytes. Compiler executable version 19.44.35228.0, toolset path `MSVC/14.44.35207/bin/HostX64/x64/cl.exe`; Visual Studio 2022 Community. Compile flags include `/O2 /std:c17 /TC /fp:precise` and x64 linking. No compiler-version claim should silently substitute the directory version for executable version.

The single session `20260926-112604-bdea7596` began 2026-09-26 15:26:04 UTC and completed 2026-09-27 02:35:07 UTC. It records PASS, 87 completed workloads, 77 timed workloads, protected originals unchanged. Session identity, complete job checkpoint records and raw data are retained.

QPC frequency is 10,000,000 Hz, main thread selected affinity bit 1 (process mask 255). Balanced power-plan GUID is recorded. Plan/settings are checked around timing jobs but no continuous CPU frequency, thermal state or background-load monitoring is provided. Warm input buffers, 0.25-second calibration target, fixed calibrated iteration count, three warm-up batches, eleven measured batches, deterministic shuffled task order. Final outputs are verified outside the timing interval. Encode includes fresh statistics, model search, output allocation/serialization; decode includes fresh parsing, table rebuilding/allocation with already allocated output buffers.

Across all 1,356 tasks, median `(Q3−Q1)/median` is 1.32079%, maximum 21.0767%, and 38 tasks exceed 10%. None exceeds 25%. 489 of 14,916 measurement batches are below calibration target and are retained. Raw ranges sometimes exceed the IQR substantially: keep all batches. Repeated timings do not create independent datasets or establish across-machine significance.

The 720 ranges sample 20 blocks per file per size from twelve frozen Silesia files. Different sizes cover different byte sets and overlap across sizes. Changes across sizes are therefore descriptive sample results, not a controlled causal experiment isolating block length. The corpus was already visible in DCC004, not a new unseen holdout. All block members reset previous-byte context to zero and independently pay all codec-internal header, table, padding and state costs. No cross-block carry, framing/container, stored fallback, or index is included. Ten hypothetical outer bytes per member exist only as a separately labeled arithmetic column.

## Output files

- `audit_results.json`: all aggregates and comparisons, environment, counts, input identities.
- `main_block_estimated_table_9rows.csv`: manuscript-ready principal estimated-selection table.
- `aggregate_all_variants.csv`: every non-synthetic group/size/variant rate and memory measure; unmeasured timing fields are blank.
- `generator_comparisons.csv`: both rate definitions, exact/estimated/Huffman parity comparisons, aggregate encode/decode speedups.
- `per_file_generator_comparisons.csv`, `scatter_estimated_vs_native_36_points.csv`, `scatter_exact_vs_native_36_points.csv`: file-group figure inputs.
- `selection_effect_by_corpus_and_block.csv`, `exact_estimated_all_members.csv`, `exact_estimated_nonzero_sizes.csv`: selector comparison evidence.
- `timing_variability.csv`: every task's quartiles and variability, sorted descending by relative IQR.
- `audit_dcc005.py`: reproducible independent arithmetic and consistency checks.

No DCC006 experiment is warranted by the arithmetic checks performed here. The manuscript should retain the explicitly bounded implementation-level claim, qualifications and unfavorable controls above.
