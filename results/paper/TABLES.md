# Regenerated DCC manuscript tables and figure data

Source session: `20260926-112604-bdea7596`. R = reassignment; N = native-policy merge/remap; M = matched-cost merge/remap. All values were calculated from selected/memory records and raw timing batches. No benchmark was rerun.

## Table 1. Whole-file unweighted mean bits per byte

| Coder and model | Calgary (18) | Canterbury (11) | Silesia (12) |
| --- | --- | --- | --- |
| Huffman single | 4.962 | 4.520 | 5.498 |
| Huffman R | 4.029 | 3.795 | 4.207 |
| Huffman N | 3.975 | 3.724 | 4.205 |
| Huffman M | 3.959 | 3.695 | 4.207 |
| rANS single | 4.917 | 4.465 | 5.483 |
| rANS high bits | 4.270 | 3.911 | 4.097 |
| rANS R | 3.994 | 3.747 | 4.148 |
| rANS N | 3.946 | 3.690 | 4.144 |
| rANS M | 3.929 | 3.667 | 4.143 |
| Adaptive order 1 | 3.722 | 3.328 | 3.838 |

Clustered rows use exact selection. Adaptive order 1 is a rate reference. Sizes include codec bodies and exclude ten-byte outer framing and stored fallback.

## Table 2. Estimated versus exact rANS selection

| Method | Different sizes | Added bytes | Largest addition | Encode speedup Cal / Can / Sil |
| --- | --- | --- | --- | --- |
| R | 6 / 761 | 60 | 25 | 3.76 / 5.38 / 9.35 |
| N | 2 / 761 | 11 | 10 | 1.25 / 1.45 / 4.13 |
| M | 0 / 761 | 0 | 0 | 1.09 / 1.15 / 2.52 |

Size comparisons include 41 whole-file entries plus 720 blocks per generator. Speedups here apply to whole-file encoding.

## Table 3. Sampled independent blocks; estimated selection for all generators

| Block KiB | Method | Rate bpb | Encode MB/s | Decode MB/s | Peak heap KiB |
| --- | --- | --- | --- | --- | --- |
| 16 | R | 4.194 | 4.66 | 124.15 | 579.0 |
| 16 | N | 4.106 | 0.25 | 121.19 | 1150.1 |
| 16 | M | 4.077 | 0.09 | 125.69 | 1150.1 |
| 64 | R | 3.978 | 8.69 | 130.05 | 780.3 |
| 64 | N | 3.931 | 0.73 | 128.47 | 1342.1 |
| 64 | M | 3.910 | 0.24 | 132.06 | 1150.1 |
| 256 | R | 3.956 | 16.50 | 126.50 | 1300.3 |
| 256 | N | 3.933 | 2.02 | 124.32 | 1534.1 |
| 256 | M | 3.917 | 0.67 | 128.03 | 1342.1 |

Rates are unweighted means over 240 blocks per size. Throughput is total input bytes / sum of workload median seconds / 1,000,000. Peak heap is maximum additional requested encoder heap per member, not process RAM.

## Table 4. Increase in total codec-body bytes versus exact reassignment

| Block KiB | Huffman init only | rANS init only | Huffman payload only | rANS payload only |
| --- | --- | --- | --- | --- |
| 16 | 5.60% | 8.04% | 19.57% | 27.85% |
| 64 | 5.16% | 6.48% | 4.79% | 5.36% |
| 256 | 3.48% | 4.02% | 0.37% | 0.41% |

These are ratios of total body bytes, not ratios of unweighted mean rates. Ablations change one component at a time.

## Figure 1. Estimated-selection reassignment versus native-policy merging

See `Figure-1.csv` for exact coordinates and `Figure-1.svg` for an inspectable plot. Each point is twenty blocks from one Silesia file at one size. Positive rate differences indicate larger reassignment output. The 36 groups are correlated samples, not independent corpora. SVG formatting differs from the manuscript; its numerical coordinates are the same.

## Verification boundary

This exporter verifies selected/memory identity, static body accounting, source record counts, and all timing medians and type-7 quartiles from the raw measured QPC batches. It does not independently execute codecs, prove all-input correctness, authenticate corpus publishers, or replace the complete candidate-winner audit. Timing is from the original machine and does not predict another machine's absolute speed.
