#!/usr/bin/env python3
"""Regenerate the revised DCC paper's four tables and 36-point figure data.

Python standard library only. Does not compile a codec or run a benchmark.
Reads the frozen DCC005 selected/memory records and recomputes timing medians
from raw QPC measurement batches; it does not use assistant-derived CSVs.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import html
import json
import math
from pathlib import Path
import statistics
import sys
from collections import defaultdict

SESSION = "20260926-112604-bdea7596"
FAMILIES = {
    "R": ("rans_reassignment", "rans_estimated_total"),
    "N": ("rans_native", "rans_native_estimated"),
    "M": ("rans_matched", "rans_matched_estimated"),
}
WHOLE = ("calgary18", "canterbury11", "silesia")
SIZES = (16384, 65536, 262144)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def close(a, b):
    return math.isclose(float(a), float(b), rel_tol=1e-11, abs_tol=1e-11)


def quantile(values, p):
    values = sorted(values)
    location = (len(values) - 1) * p
    index = int(location)
    if index == len(values) - 1:
        return values[index]
    return values[index] + (location - index) * (values[index + 1] - values[index])


def write_csv(path, rows):
    require(bool(rows), "Cannot export an empty table")
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def md_table(headers, rows):
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(["---"] * len(headers)) + " |"]
    lines += ["| " + " | ".join(str(value) for value in row) + " |" for row in rows]
    return "\n".join(lines)


def svg_scatter(points):
    """Readable standalone data figure; paper's precise typesetting is separate."""
    width, height = 900, 550
    left, top, plot_width, plot_height = 90, 35, 775, 430
    def px(x):
        return left + x / 25 * plot_width
    def py(y):
        return top + (5 - y) / 6 * plot_height
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
        '<title id="title">Estimated-selection rANS: reassignment versus native merging</title>',
        '<desc id="desc">Thirty-six Silesia file and block-size groups. Positive rate differences mean larger reassignment output. Both methods use estimated selection.</desc>',
        '<rect width="100%" height="100%" fill="white"/>',
        '<g font-family="serif" font-size="18" fill="#222">',
    ]
    for x in range(0, 26, 5):
        parts += [f'<line x1="{px(x)}" y1="{top}" x2="{px(x)}" y2="{top+plot_height}" stroke="#ddd"/>',
                  f'<text x="{px(x)}" y="{top+plot_height+27}" text-anchor="middle">{x}</text>']
    for y in range(-1, 6):
        parts += [f'<line x1="{left}" y1="{py(y)}" x2="{left+plot_width}" y2="{py(y)}" stroke="{"#777" if y == 0 else "#ddd"}"/>',
                  f'<text x="{left-15}" y="{py(y)+6}" text-anchor="end">{y}</text>']
    parts += [f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_height}" stroke="#222"/>',
              f'<line x1="{left}" y1="{top+plot_height}" x2="{left+plot_width}" y2="{top+plot_height}" stroke="#222"/>',
              f'<text x="{left+plot_width/2}" y="{height-30}" text-anchor="middle">Reassignment / native encoding speed</text>',
              f'<text transform="translate(26,{top+plot_height/2}) rotate(-90)" text-anchor="middle">Mean rate difference (%)</text>']
    colors = {16: "#333333", 64: "#27658C", 256: "#AF5A14"}
    def mark(x, y, size, title=""):
        color = colors[size]
        label = f"<title>{html.escape(title)}</title>" if title else ""
        if size == 16:
            return f'<circle cx="{x:.3f}" cy="{y:.3f}" r="6" fill="{color}" stroke="white">{label}</circle>'
        if size == 64:
            return f'<rect x="{x-6:.3f}" y="{y-6:.3f}" width="12" height="12" fill="{color}" stroke="white">{label}</rect>'
        return f'<polygon points="{x:.3f},{y-7:.3f} {x-7:.3f},{y+6:.3f} {x+7:.3f},{y+6:.3f}" fill="{color}" stroke="white">{label}</polygon>'
    for point in points:
        x, y = point["encode_speedup"], point["relative_unweighted_mean_bpb_penalty_percent"]
        require(0 <= x <= 25 and -1 <= y <= 5, "Scatter point outside the frozen paper axes")
        label = f"{point['file']}, {point['block_size_KiB']} KiB: speedup {x:.9g}, rate difference {y:.9g}%"
        parts.append(mark(px(x), py(y), point["block_size_KiB"], label))
    for index, size in enumerate((16, 64, 256)):
        y = top + 25 + index * 29
        parts += [mark(left+24, y, size), f'<text x="{left+42}" y="{y+6}">{size} KiB</text>']
    parts += ["</g></svg>"]
    return "\n".join(parts) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True, help="Extracted DCC005-20260926-112604-bdea7596 folder")
    parser.add_argument("--output-dir", type=Path, required=True, help="New or empty folder for derived outputs")
    args = parser.parse_args()
    root = args.run_dir.resolve()
    output = args.output_dir.resolve()
    require(root.is_dir(), f"Run directory not found: {root}")
    require(not output.exists() or (output.is_dir() and not any(output.iterdir())), "Output directory must be new or empty; prior outputs will not be overwritten")
    require(output != root and root not in output.parents, "Keep derived output outside the frozen run directory")
    analysis = root / "analysis" / SESSION
    sources = [analysis / name for name in ("selected-models.csv", "memory-models.csv", "timing-summary.csv")]
    selected, memory, timing = [read_csv(path) for path in sources]
    require(len(selected) == len(memory) == 13107, "Expected 13,107 selected and memory rows")
    require(len(timing) == 1356, "Expected 1,356 timing summaries")
    require(all(row["status"] == "PASS" for row in selected + memory + timing), "A source record is not PASS")
    sidx = {(row["member"], row["variant"]): row for row in selected}
    midx = {(row["member"], row["variant"]): row for row in memory}
    tidx = {(row["workload"], row["variant"], row["phase"]): row for row in timing}
    require(len(sidx) == len(midx) == 13107 and len(tidx) == 1356, "Duplicate source record keys")
    require(sidx.keys() == midx.keys(), "Selected and memory model keys differ")
    for key, row in sidx.items():
        mem = midx[key]
        for field in ("original_bytes", "codec_body_bytes", "requested_k", "realized_k", "stream_fnv64", "decoder_table_bytes"):
            require(row[field] == mem[field], f"Memory record mismatch: {key}, {field}")
        require(mem["encode_after_release_delta"] == mem["decode_after_return_delta"] == "0", f"Unreleased requested heap: {key}")
        if row["variant"] != "adaptive_order1":
            bits = sum(int(row[field]) for field in ("count_bits", "map_bits", "table_bits", "padding_bits", "payload_bits")) + 8 * int(row["state_bytes"])
            require(bits == 8 * int(row["codec_body_bytes"]), f"Body accounting mismatch: {key}")
    require({row["session"] for row in timing} == {SESSION}, "This exporter requires the frozen final DCC005 session")
    require({row["profile"] for row in timing} == {"FULL"}, "Timing summaries must use FULL profile")
    batches = defaultdict(list)
    sample_paths = sorted((root / "jobs").glob(f"*-timing/{SESSION}/records.samples.csv"))
    require(len(sample_paths) == 77, "Expected 77 raw timing sample files")
    sources.extend(sample_paths)
    for path in sample_paths:
        workload = path.parent.parent.name.removesuffix("-timing")
        for row in read_csv(path):
            if row["stage"] != "measurement":
                continue
            require(row["verified"] == "1" and row["profile"] == "FULL", f"Unverified measurement in {path}")
            seconds = int(row["elapsed_ticks"]) / int(row["timer_frequency"]) / int(row["iterations"])
            require(close(seconds, row["seconds_per_operation"]), f"QPC conversion mismatch in {path}")
            batches[(workload, row["variant"], row["phase"])].append(seconds)
    require(batches.keys() == tidx.keys(), "Raw measurement tasks and timing summaries differ")
    require(sum(map(len, batches.values())) == 14916, "Expected 14,916 measured batches")
    medians = {}
    relative_iqrs = []
    for key, values in batches.items():
        require(len(values) == 11, f"Expected eleven measured batches: {key}")
        for field, p in (("q1_seconds", .25), ("median_seconds", .5), ("q3_seconds", .75)):
            require(close(quantile(values, p), tidx[key][field]), f"Timing statistic mismatch: {key}, {field}")
        medians[key] = statistics.median(values)
        relative_iqrs.append((quantile(values, .75) - quantile(values, .25)) / medians[key])
    groups = defaultdict(list)
    for row in selected:
        if row["scope"] != "synthetic":
            groups[(row["group"], int(row["block_size"]), row["variant"])].append(row)
    def rate(rows):
        return statistics.mean(8 * int(row["codec_body_bytes"]) / int(row["original_bytes"]) for row in rows)
    def body(rows):
        return sum(int(row["codec_body_bytes"]) for row in rows)
    def throughput(rows, phase):
        workloads = {row["workload"] for row in rows}
        variants = {row["variant"] for row in rows}
        require(len(variants) == 1, "Throughput group mixes variants")
        variant = next(iter(variants))
        seconds = sum(medians[(workload, variant, phase)] for workload in sorted(workloads))
        return sum(int(row["original_bytes"]) for row in rows) / seconds / 1e6
    table1 = []
    labels = (("huff_single", "Huffman single"), ("huff_reassignment", "Huffman R"), ("huff_native", "Huffman N"), ("huff_matched", "Huffman M"), ("rans_single", "rANS single"), ("rans_highbits", "rANS high bits"), ("rans_reassignment", "rANS R"), ("rans_native", "rANS N"), ("rans_matched", "rANS M"), ("adaptive_order1", "Adaptive order 1"))
    for variant, label in labels:
        row = {"variant": variant, "label": label}
        for group, count in zip(WHOLE, (18, 11, 12)):
            values = groups[(group, 0, variant)]
            require(len(values) == count, f"Wrong whole-file count: {group}, {variant}")
            row[group + "_mean_bpb"] = rate(values)
        table1.append(row)
    table2 = []
    for label, (exact, estimated) in FAMILIES.items():
        pairs = [row for row in selected if row["variant"] == exact and row["scope"] != "synthetic"]
        require(len(pairs) == 761, f"Wrong exact/estimated member count: {label}")
        differences = [int(sidx[(row["member"], estimated)]["codec_body_bytes"]) - int(row["codec_body_bytes"]) for row in pairs]
        require(min(differences) >= 0, f"Estimated output smaller than exact minimum: {label}")
        row = {"generator": label, "compared_members": len(pairs), "different_sizes": sum(value != 0 for value in differences), "total_added_bytes": sum(differences), "largest_addition_bytes": max(differences)}
        for group in WHOLE:
            row[group + "_encode_speedup"] = throughput(groups[(group, 0, estimated)], "encode") / throughput(groups[(group, 0, exact)], "encode")
        table2.append(row)
    table3 = []
    for size in SIZES:
        for label, (_, estimated) in FAMILIES.items():
            rows = groups[("silesia-blocks", size, estimated)]
            require(len(rows) == 240, f"Wrong sampled-block count: {size}, {label}")
            table3.append({"block_size_KiB": size // 1024, "generator": label, "unweighted_mean_member_bpb": rate(rows), "encode_MB_per_second": throughput(rows, "encode"), "decode_MB_per_second": throughput(rows, "decode"), "maximum_member_encode_requested_heap_KiB": max(int(midx[(row["member"], estimated)]["encode_peak_requested_heap_bytes"]) for row in rows) / 1024})
    table4 = []
    for size in SIZES:
        row = {"block_size_KiB": size // 1024}
        for coder, ablation in (("huff", "init_only"), ("rans", "init_only"), ("huff", "payload_only"), ("rans", "payload_only")):
            exact = body(groups[("silesia-blocks", size, coder + "_reassignment")])
            other = body(groups[("silesia-blocks", size, coder + "_" + ablation)])
            row[coder + "_" + ablation + "_increase_percent"] = 100 * (other / exact - 1)
        table4.append(row)
    by_file = defaultdict(list)
    for row in selected:
        if row["scope"] == "blocks":
            by_file[(row["file"], int(row["block_size"]), row["variant"])].append(row)
    points = []
    for file, size in sorted({(key[0], key[1]) for key in by_file}):
        rrows = by_file[(file, size, "rans_estimated_total")]
        nrows = by_file[(file, size, "rans_native_estimated")]
        require(len(rrows) == len(nrows) == 20, "A figure group does not contain twenty blocks per method")
        points.append({"file": file, "block_size_KiB": size // 1024, "members": 20, "reassignment_unweighted_member_bpb": rate(rrows), "native_unweighted_member_bpb": rate(nrows), "encode_speedup": throughput(rrows, "encode") / throughput(nrows, "encode"), "relative_unweighted_mean_bpb_penalty_percent": 100 * (rate(rrows) / rate(nrows) - 1)})
    require(len(points) == 36, "Expected 36 figure groups")
    sections = [
        "# Regenerated DCC manuscript tables and figure data",
        f"Source session: `{SESSION}`. R = reassignment; N = native-policy merge/remap; M = matched-cost merge/remap. All values were calculated from selected/memory records and raw timing batches. No benchmark was rerun.",
        "## Table 1. Whole-file unweighted mean bits per byte",
        md_table(["Coder and model", "Calgary (18)", "Canterbury (11)", "Silesia (12)"], [[row["label"]] + [f'{row[group + "_mean_bpb"]:.3f}' for group in WHOLE] for row in table1]),
        "Clustered rows use exact selection. Adaptive order 1 is a rate reference. Sizes include codec bodies and exclude ten-byte outer framing and stored fallback.",
        "## Table 2. Estimated versus exact rANS selection",
        md_table(["Method", "Different sizes", "Added bytes", "Largest addition", "Encode speedup Cal / Can / Sil"], [[row["generator"], f'{row["different_sizes"]} / {row["compared_members"]}', row["total_added_bytes"], row["largest_addition_bytes"], " / ".join(f'{row[group + "_encode_speedup"]:.2f}' for group in WHOLE)] for row in table2]),
        "Size comparisons include 41 whole-file entries plus 720 blocks per generator. Speedups here apply to whole-file encoding.",
        "## Table 3. Sampled independent blocks; estimated selection for all generators",
        md_table(["Block KiB", "Method", "Rate bpb", "Encode MB/s", "Decode MB/s", "Peak heap KiB"], [[row["block_size_KiB"], row["generator"], f'{row["unweighted_mean_member_bpb"]:.3f}', f'{row["encode_MB_per_second"]:.2f}', f'{row["decode_MB_per_second"]:.2f}', f'{row["maximum_member_encode_requested_heap_KiB"]:.1f}'] for row in table3]),
        "Rates are unweighted means over 240 blocks per size. Throughput is total input bytes / sum of workload median seconds / 1,000,000. Peak heap is maximum additional requested encoder heap per member, not process RAM.",
        "## Table 4. Increase in total codec-body bytes versus exact reassignment",
        md_table(["Block KiB", "Huffman init only", "rANS init only", "Huffman payload only", "rANS payload only"], [[row["block_size_KiB"]] + [f'{row[field]:.2f}%' for field in ("huff_init_only_increase_percent", "rans_init_only_increase_percent", "huff_payload_only_increase_percent", "rans_payload_only_increase_percent")] for row in table4]),
        "These are ratios of total body bytes, not ratios of unweighted mean rates. Ablations change one component at a time.",
        "## Figure 1. Estimated-selection reassignment versus native-policy merging",
        "See `Figure-1.csv` for exact coordinates and `Figure-1.svg` for an inspectable plot. Each point is twenty blocks from one Silesia file at one size. Positive rate differences indicate larger reassignment output. The 36 groups are correlated samples, not independent corpora. SVG formatting differs from the manuscript; its numerical coordinates are the same.",
        "## Verification boundary",
        "This exporter verifies selected/memory identity, static body accounting, source record counts, and all timing medians and type-7 quartiles from the raw measured QPC batches. It does not independently execute codecs, prove all-input correctness, authenticate corpus publishers, or replace the complete candidate-winner audit. Timing is from the original machine and does not predict another machine's absolute speed.",
    ]
    output.mkdir(parents=True, exist_ok=True)
    for number, rows in enumerate((table1, table2, table3, table4), 1):
        write_csv(output / f"Table-{number}.csv", rows)
    write_csv(output / "Figure-1.csv", points)
    (output / "Figure-1.svg").write_text(svg_scatter(points), encoding="utf-8")
    (output / "TABLES.md").write_text("\n\n".join(sections) + "\n", encoding="utf-8")
    report = {
        "status": "PASS", "session": SESSION, "benchmark_rerun": False,
        "checked_counts": {"selected_rows": len(selected), "memory_rows": len(memory), "timing_tasks": len(timing), "raw_measurement_batches": sum(map(len, batches.values())), "raw_timing_files": len(sample_paths), "table_1_rows": len(table1), "table_2_rows": len(table2), "table_3_rows": len(table3), "table_4_rows": len(table4), "figure_points": len(points)},
        "timing_variation": {"tasks_above_10_percent_relative_iqr": sum(value > .1 for value in relative_iqrs), "maximum_relative_iqr": max(relative_iqrs)},
        "source_sha256": {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
        "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "generated_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(output.iterdir()) if path.is_file()},
    }
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: Tables 1-4 and 36 Figure 1 points regenerated in {output}")
    print("Verified 13,107 selected/memory pairs and 1,356 timing tasks from 14,916 measured batches.")
    print("No compression experiment was rerun. See verification.json and TABLES.md.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, ZeroDivisionError) as error:
        print(f"FAILED: {error}", file=sys.stderr)
        sys.exit(1)
