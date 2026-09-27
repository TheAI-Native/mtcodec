The input directory layout is `calgary18/`, `canterbury11/`, `silesia/`, containing 18, 11 and 12 files respectively. Inputs are not committed to Git.

Use `scripts/Acquire-Inputs.py` with `protocol/audited-corpus-manifest.csv`. The helper downloads the publisher archives into a new directory, verifies their pinned SHA-256 values, extracts only manifest-listed files, and verifies all 41 per-file hashes and lengths. A changed archive stops the operation even if a server might have merely repackaged it; investigate before updating provenance.

For local reproduction, `scripts/Reproduce-Study.py --phase inputs --data-root ...` reads a directory directly containing the three corpus folders. `--study-root ...` instead reads its `data` child. Synthetic fixtures come from the repository's `fixtures/` and are mapped without changing the old manifest's paths.

Verified source URLs:

- https://corpus.canterbury.ac.nz/resources/calgary.tar.gz
- https://corpus.canterbury.ac.nz/resources/cantrbry.tar.gz
- https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip

All 41 inputs were matched against these sources on September 27, 2026. The archived acquisition report records the exact archive hashes and per-file checks. The Calgary description page discusses 14 entries; its downloaded archive contains all 18 tested here, including paper3–paper6. Calgary `pic` and Canterbury `ptt5` are duplicate content. Preserve both named experimental entries when reproducing the original aggregation.
