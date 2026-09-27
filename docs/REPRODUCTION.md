**Commands and verification levels**

Use Python 3.9 or newer. Publication scripts use only the standard library; no pip installation is required. New output directories are required. Every reproduction command first checks the 44 frozen scientific file hashes. The original study, corpus files and source tree are read-only inputs.

| Phase | Dependencies | Work and expected result |
| --- | --- | --- |
| Saved-results export | Python and extracted public evidence archive | Four table CSVs, figure CSV/SVG, TABLES.md, verification.json; no benchmark |
| `inputs` | Python and corpus files | 41 complete input hashes and 771 member/range checks; no compiler |
| `build` | Windows x64 MSVC, PowerShell | Three newly built executable modes; no benchmark |
| `smoke` | Windows x64 MSVC, PowerShell | Build plus 52 unit checks, 144 oracle cases, five expected failures |
| `audit` | Windows toolchain and all inputs | Build, smoke, 87 correctness/size and 87 requested-memory jobs; potentially long |
| `full` | Same, stable idle machine | Audit plus 77 timed workloads; potentially many hours |

Common invocation:

```powershell
py -3 .\scripts\Reproduce-Study.py --repo-root . --output-dir 'NEW-OUTPUT-DIRECTORY' --phase smoke
```

Input-only invocation:

```powershell
py -3 .\scripts\Reproduce-Study.py --repo-root . --output-dir 'NEW-OUTPUT-DIRECTORY' --phase inputs --data-root 'DIRECTORY-CONTAINING-THREE-CORPUS-FOLDERS'
```

Explicit full experiment:

```powershell
py -3 .\scripts\Reproduce-Study.py --repo-root . --output-dir 'NEW-OUTPUT-DIRECTORY' --phase full --data-root 'DIRECTORY-CONTAINING-THREE-CORPUS-FOLDERS' --allow-long-run
```

Open the x64 Native Tools Command Prompt for VS 2022 and then enter `powershell -NoLogo -NoProfile`. `cl.exe` must resolve to `Hostx64\x64`; `CL` and `_CL_` must be empty. Use ASCII paths; spaces are supported. The wrapper loads preserved PowerShell validation code in memory and does not change execution policy.

The original build uses `/O2 /W4 /std:c17 /TC /utf-8 /fp:precise`, with x64 linking. It builds oracle, production and memory-instrumented modes, preserving policy IDs 0/1/2. Full reproduction retains the one-based ordinal from all 87 workload definitions, including synthetic entries, rather than renumbering only timed workloads. This preserves the shuffle seed inputs.

Full timing is machine-specific. The original run used Windows 10 Pro 10.0.19045, MSVC 19.44.35228.0/toolset 14.44.35207, i7-7700K, approximately 32 GiB RAM, and Balanced power. The C runner uses QPC and pins its main thread to the lowest allowed affinity bit. Inputs are resident; decode reconstructs tables each call. Power settings are recorded and checked around timing. Close CPU-heavy applications and prevent manual sleep/restart during a full run. The wrapper requests prevention of idle system sleep for long phases and restores that request on exit.

A new full run writes its own raw records and environment into its new output directory. The fixed-reference exporter reproduces the paper from the archived DCC005 records; it does not silently replace the paper with new timing results. New results should be compared and reported separately. This publication wrapper deliberately does not resume incomplete jobs; retain interrupted outputs and choose a new output directory if a rerun is justified.

Validation boundary: source/flag checks, guarded path behavior, input verification and saved-result numerical reproduction have been exercised during publication preparation. Native Windows execution of the new wrapper must be established by the owner's smoke test. Audit/full phases remain separately unexecuted unless their own new run reports PASS. A passing smoke test does not prove a completed full public rerun.
