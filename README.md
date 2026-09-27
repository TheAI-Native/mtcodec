**mtcodec — context-clustered Huffman and rANS coding**

Research artifact for Richard Leinecker's manuscript, *Compression and Encoding Cost in Context Clustered Huffman and rANS Coding*, University of Central Florida. The manuscript is being prepared for submission; no acceptance is claimed.

The study compares deterministic context reassignment with native-policy and matched-cost Brotli-derived merge/remap controls under common static serializers. Stronger merging usually compresses better. On the sampled 16/64/256 KiB blocks, estimated-selection reassignment rANS encodes 18.28/11.97/8.19 times faster than native-policy merging, with mean-rate penalties about 2.14/1.20/0.59%. These are implementation-specific measurements, not a comparison against complete Brotli or a universally optimal clustering claim.

**Start with saved-results reproduction.** Download the release asset `DCC005-public-evidence-20260926-112604-bdea7596.zip`, extract it outside this repository, and run with Python 3.9 or newer:

```powershell
py -3 .\scripts\Export-Paper-Results.py --run-dir 'PATH-TO-EXTRACTED\DCC005-20260926-112604-bdea7596' --output-dir 'NEW-OUTPUT-DIRECTORY'
```

This needs no compiler, corpus download, or benchmark rerun. It writes Tables 1–4 as CSV, Figure 1 coordinates and SVG, a readable `TABLES.md`, and a verification report. It recomputes the 1,356 timing summaries from 14,916 raw measurement batches and checks 13,107 selected/memory pairs. Published expected outputs are in `results/paper/`. The SVG reproduces the data; its typography differs from the manuscript's plotted figure.

**Inputs.** The three source collections can be independently downloaded and verified:

```powershell
py -3 .\scripts\Acquire-Inputs.py --manifest .\protocol\audited-corpus-manifest.csv --output 'NEW-INPUT-DIRECTORY'
```

This creates `NEW-INPUT-DIRECTORY\data\calgary18`, `canterbury11`, and `silesia`, verifies all 41 frozen SHA-256 values, and records provenance. No corpus bytes are included in this source repository. On September 27, 2026, all 41 inputs were independently reacquired from publisher URLs and matched the frozen inputs; see `provenance/corpus-public-acquisition-verification-20260927.json`. That later verification supplements the preserved historical manifest. The Calgary archive contains all 18 tested entries even though its description page lists 14.

**Build and smoke verification.** On Windows, open the **x64 Native Tools Command Prompt for VS 2022**, then enter `powershell -NoLogo -NoProfile`. From this repository, run:

```powershell
py -3 .\scripts\Reproduce-Study.py --repo-root . --output-dir 'NEW-OUTPUT-DIRECTORY' --phase smoke
```

This builds the three executable modes from unchanged scientific source and checks 52 unit predicates, 144 native-oracle comparisons, and five expected failure controls. Use ASCII paths; spaces are supported. Compiler/PowerShell requirements and all phases are in `docs/REPRODUCTION.md`. New output directories are required and should be outside the repository and corpus directories.

**Validation status.** The original DCC005 run completed successfully and its results were independently audited. On September 27, 2026, the new publication wrapper passed Windows build and smoke verification: 52 unit checks, 144 native-oracle comparisons, and five expected-failure controls. Publisher reacquisition of all 41 corpus entries, verification of 771 members across 87 workloads, and regeneration of the paper outputs also passed on Windows. provenance/publication-smoke-validation.json records the passing wrapper hash. The new audit and full timing phases remain unexecuted; the paper retains the original DCC005 measurements.

**Files.** Keep `source/`, `tests/`, and `vendor/` as siblings: their include paths rely on that layout. Internal DCC003 and DCC004 filenames are intentional inherited implementation names. `provenance/frozen-files.csv` identifies the 44 unchanged scientific files. Root `package-files.csv` is the historical 88-file FIX1 package manifest and also lists old delivery documents absent from this curated tree; it is not an inventory of the new public repository.

**Evidence.** The public evidence asset retains every original CSV and all scientific/protocol files byte-for-byte. Build products are omitted. Operational records replace the author's local study path and machine name; the asset's `PUBLICATION-MANIFEST.csv` records original and public hashes for every file. Historical checkpoint hashes refer to the original unredacted evidence. Historical launchers in the evidence archive document the old run and must not be used to launch this new package.

**Scope.** The 720 blocks are correlated samples from 12 Silesia files. The 41 corpus entries include a byte-identical Calgary `pic` / Canterbury `ptt5` pair. Rates are usually unweighted means; speedups use aggregate throughput. Original timing was warm-memory, single-threaded on Windows 10 Pro 10.0.19045, i7-7700K, MSVC 19.44.35228.0, Balanced power plan. Runtime and native floating-point tie behavior can change on other machines. New measurements must be reported separately. See `docs/STUDY-HISTORY.md` and the manuscript for qualifications.

**Licensing and citation.** Preserve LICENSE.txt and vendor/brotli-v1.1.0/LICENSE; this research package contains mixed CC0/MIT material described in THIRD_PARTY_NOTICES.md. CITATION.cff and RELEASE-CITATION.txt identify version v1.0.0-dcc-submission and DOI 10.5281/zenodo.23001172. Zenodo registers this DOI when the archive is published.
