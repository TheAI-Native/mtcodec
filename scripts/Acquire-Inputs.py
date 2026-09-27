#!/usr/bin/env python3
"""Reacquire DCC005's 41 frozen corpus inputs into a NEW directory.

Python 3.9+, standard library only. No benchmark is run. Historical files and
manifests are never modified. Publisher archive and individual input SHA-256
checks must both pass; an incomplete acquisition exits with status 1.
"""
import argparse
import csv
import datetime
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import sys
import tarfile
import urllib.request
import zipfile

MANIFEST_SHA256 = "084d21a7e5136f8f70cebbe1752cd68476fb7d0bbd2afb6feb09b66566e15a1d"
SOURCES = {
    "calgary18": {
        "filename": "calgary.tar.gz", "count": 18, "bytes": 1070276,
        "url": "https://corpus.canterbury.ac.nz/resources/calgary.tar.gz",
        "sha256": "e109eebdc19c5cee533c58bd6a49a4be3a77cc52f84ba234a089148a4f2093b7",
    },
    "canterbury11": {
        "filename": "cantrbry.tar.gz", "count": 11, "bytes": 739071,
        "url": "https://corpus.canterbury.ac.nz/resources/cantrbry.tar.gz",
        "sha256": "f140e8a5b73d3f53198555a63bfb827889394a42f20825df33c810c3d5e3f8fb",
    },
    "silesia": {
        "filename": "silesia.zip", "count": 12, "bytes": 68182744,
        "url": "https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip",
        "sha256": "0626e25f45c0ffb5dc801f13b7c82a3b75743ba07e3a71835a41e3d9f63c77af",
    },
}


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(source, target):
    request = urllib.request.Request(source["url"], headers={"User-Agent": "DCC005-reproduction/1.0"})
    with urllib.request.urlopen(request, timeout=90) as response, target.open("xb") as handle:
        received = 0
        while True:
            block = response.read(1024 * 1024)
            if not block:
                break
            received += len(block)
            if received > source["bytes"]:
                raise ValueError("Archive exceeds pinned size; investigate publisher change.")
            handle.write(block)
        return response.geturl()


def members_by_basename(archive, is_zip):
    entries = archive.infolist() if is_zip else archive.getmembers()
    result = {}
    for member in entries:
        is_regular = not member.is_dir() if is_zip else member.isfile()
        if not is_regular:
            continue
        name = member.filename if is_zip else member.name
        basename = PurePosixPath(name).name
        if basename in result:
            raise ValueError("Ambiguous duplicate member basename: " + basename)
        result[basename] = member
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path,
                        help="Original DCC005 protocol/audited-corpus-manifest.csv")
    parser.add_argument("--output", required=True, type=Path,
                        help="NEW directory: creates data/<corpus>/<file> and a verification report")
    parser.add_argument("--archive-dir", type=Path,
                        help="Use all three existing publisher archives instead of downloading")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("Output already exists. Choose a NEW directory; nothing was overwritten.")
    try:
        manifest_bytes = args.manifest.read_bytes()
        manifest_hash = hashlib.sha256(manifest_bytes).hexdigest()
        if manifest_hash != MANIFEST_SHA256:
            raise ValueError("Manifest SHA-256 differs from frozen DCC005 manifest; stop and investigate.")
        rows = list(csv.DictReader(io.StringIO(manifest_bytes.decode("utf-8-sig"), newline="")))
        if len(rows) != 41:
            raise ValueError("Expected exactly 41 manifest rows.")
        for corpus, source in SOURCES.items():
            selected = [r for r in rows if r["corpus"] == corpus]
            if len(selected) != source["count"]:
                raise ValueError("Wrong number of manifest rows for " + corpus)
            for row in selected:
                if row["relative_path"] != "data/" + corpus + "/" + row["file"]:
                    raise ValueError("Unexpected manifest path: " + row["relative_path"])
                if PurePosixPath(row["file"]).name != row["file"] or "\\" in row["file"]:
                    raise ValueError("Unsafe manifest filename.")
        output.mkdir(parents=True)
        report = {"created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "purpose": "Independent input reacquisition; no benchmark executed",
                  "manifest_sha256": manifest_hash,
                  "archive_pins_verified_utc_date": "2026-09-27",
                  "complete": False, "archives": [], "files": [], "errors": []}
        for corpus, source in SOURCES.items():
            archive_record = {"corpus": corpus, **source, "verified": False}
            report["archives"].append(archive_record)
            try:
                if args.archive_dir:
                    archive_path = args.archive_dir / source["filename"]
                    archive_record["mode"] = "local_archive"
                else:
                    archive_dir = output / "downloads"
                    archive_dir.mkdir(exist_ok=True)
                    archive_path = archive_dir / source["filename"]
                    print("Downloading " + source["filename"], flush=True)
                    archive_record["mode"] = "publisher_download"
                    archive_record["resolved_url"] = download(source, archive_path)
                actual_size = archive_path.stat().st_size
                actual_hash = sha256_file(archive_path)
                archive_record["observed_bytes"] = actual_size
                archive_record["observed_sha256"] = actual_hash
                if actual_size != source["bytes"] or actual_hash != source["sha256"]:
                    raise ValueError("Pinned archive size/SHA-256 mismatch; stop and investigate.")
                archive_record["verified"] = True
                is_zip = source["filename"].endswith(".zip")
                opener = zipfile.ZipFile(archive_path) if is_zip else tarfile.open(archive_path, "r:gz")
                with opener as archive:
                    members = members_by_basename(archive, is_zip)
                    archive_record["regular_member_names"] = sorted(members)
                    for row in (r for r in rows if r["corpus"] == corpus):
                        expected_bytes = int(row["bytes"])
                        if row["file"] not in members:
                            raise ValueError("Required corpus file missing: " + row["file"])
                        member = members[row["file"]]
                        declared_bytes = member.file_size if is_zip else member.size
                        if declared_bytes != expected_bytes:
                            raise ValueError("Archive member size mismatch: " + row["file"])
                        stream = archive.open(member) if is_zip else archive.extractfile(member)
                        with stream:
                            data = stream.read(expected_bytes + 1)
                        digest = hashlib.sha256(data).hexdigest()
                        if len(data) != expected_bytes or digest != row["sha256"]:
                            raise ValueError("Corpus size/SHA-256 mismatch: " + row["file"])
                        target = output / row["relative_path"]
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with target.open("xb") as handle:
                            handle.write(data)
                        report["files"].append({"corpus": corpus, "file": row["file"],
                                                "relative_path": row["relative_path"],
                                                "bytes": len(data), "sha256": digest, "verified": True})
                print(corpus + ": " + str(source["count"]) + " frozen inputs match", flush=True)
            except Exception as error:
                message = corpus + ": " + str(error)
                report["errors"].append(message)
                print("ERROR: " + message, file=sys.stderr, flush=True)
        report["complete"] = len(report["files"]) == 41 and not report["errors"]
        with (output / "input-acquisition-verification.json").open("x", encoding="utf-8", newline="\n") as handle:
            json.dump(report, handle, indent=2)
            handle.write("\n")
        if not report["complete"]:
            print("INCOMPLETE. Do not run the benchmark with this directory. See verification report.", file=sys.stderr)
            return 1
        print("PASS: all 41 inputs match the frozen DCC005 manifest. No historical files were modified.")
        print("Verified input root: " + str(output))
        return 0
    except Exception as error:
        print("ERROR: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
