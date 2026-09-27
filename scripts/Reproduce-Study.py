#!/usr/bin/env python3
"""DCC005 publication wrapper. Python standard library; no measurements by default.

Build/smoke/audit/full require Windows x64 MSVC developer command prompt.
inputs is read-only with respect to inputs and works on other platforms.
Every invocation writes a new output directory; it never resumes original runs.
Frozen scientific C code and PowerShell result checks are not modified.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone

CAPTURE = None

def fail(message):
    raise RuntimeError(message)

def require(condition, message):
    if not condition:
        fail(message)

def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))

def write_json(path, obj):
    Path(path).write_text(json.dumps(obj, indent=2) + '\n', encoding='utf-8')

def ps_literal(value):
    return "'" + str(value).replace("'", "''") + "'"

def rows(path):
    with Path(path).open(newline='', encoding='utf-8-sig') as stream:
        return list(csv.DictReader(stream))

def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def below(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False

def command(args, cwd, log, expected=0):
    args = [str(a) for a in args]
    record = {'argv': args, 'cwd': str(cwd), 'expected_exit': expected,
              'started_utc': datetime.now(timezone.utc).isoformat()}
    write_json(str(log) + '.command.json', record)
    print('Running:', Path(args[0]).name, ' '.join(args[1:3]), flush=True)
    # Redirect to files so long benchmark logs remain available on interruption.
    with open(str(log) + '.stdout.txt', 'w', encoding='utf-8') as out, \
         open(str(log) + '.stderr.txt', 'w', encoding='utf-8') as err:
        result = subprocess.run(args, cwd=cwd, stdout=out, stderr=err, check=False)
    record.update(exit_code=result.returncode, completed_utc=datetime.now(timezone.utc).isoformat())
    write_json(str(log) + '.command.json', record)
    require(result.returncode == expected,
            f'Exit {result.returncode}, expected {expected}; see {log}.stdout.txt and .stderr.txt')
    return record

def scientific_integrity(repo, output):
    manifest = rows(repo / 'package-files.csv')
    checked = []
    for row in manifest:
        rel = Path(row['relative_path'].replace('\\', '/'))
        require(not rel.is_absolute() and '..' not in rel.parts, 'Unsafe manifest path')
        if rel.parts[0] not in ('source', 'tests', 'vendor', 'fixtures', 'protocol'):
            continue
        path = repo / rel
        require(path.is_file(), f'Missing frozen file: {path}')
        digest = sha(path)
        require(digest == row['sha256'], f'Frozen source/protocol hash mismatch: {path}')
        require(path.stat().st_size == int(row['bytes']), f'Frozen length mismatch: {path}')
        checked.append({'path': rel.as_posix(), 'sha256': digest, 'bytes': path.stat().st_size})
    require(len(checked) == 44, f'Expected 44 scientific package files; got {len(checked)}')
    write_json(output / 'scientific-integrity.json', {'status': 'PASS', 'files': checked})

def windows_tools():
    require(os.name == 'nt', 'This phase requires Windows; inputs-only also works on other platforms.')
    compiler = shutil.which('cl.exe')
    require(compiler is not None, 'Run from the x64 Native Tools Command Prompt for VS 2022.')
    require(re.search(r'\\Hostx64\\x64\\cl\.exe$', compiler, re.I),
            f'Expected x64-host/x64-target MSVC cl.exe; got {compiler}')
    require(not os.environ.get('CL', '').strip() and not os.environ.get('_CL_', '').strip(),
            'CL and _CL_ must be empty to avoid unrecorded compiler flags.')
    powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    require(powershell.is_file(), 'Windows PowerShell 5.1 not found.')
    return Path(compiler), powershell

def build(repo, output, compiler):
    directory = output / 'build'
    directory.mkdir()
    source, tests = repo / 'source', repo / 'tests'
    common = ['/nologo', '/Bv', '/O2', '/W4', '/std:c17', '/TC', '/utf-8', '/fp:precise', '/c',
              '/I' + str(source), '/I' + str(tests)]
    executables = {}
    for mode in ('oracle', 'production', 'memory'):
        extra = [] if mode == 'oracle' else ['/DDCC004_PRODUCTION']
        if mode == 'memory':
            extra += ['/DDCC004_MEMORY', '/FI' + str(source / 'dcc004_memory_redirect.h')]
        main = 'dcc003_check.c' if mode == 'oracle' else 'dcc005_run.c'
        units = [('codec', source / 'dcc003_codec.c', []),
                 ('reference', tests / 'dcc_original_bridge.c', []), ('main', tests / main, [])]
        units += [(f'policy{i}', source / 'cluster_policy.c', [f'/DDCC_POLICY={i}']) for i in range(3)]
        if mode == 'memory':
            units.append(('allocator', source / 'dcc004_memory.c', []))
        objects = []
        for name, path, opts in units:
            obj = directory / f'{mode}-{name}.obj'
            args = common + ([] if name == 'allocator' else extra) + opts + [str(path), '/Fo:' + str(obj)]
            command([compiler] + args, directory, directory / f'compile-{mode}-{name}')
            objects.append(str(obj))
        exe = directory / f'dcc005_{mode}.exe'
        command([compiler, '/nologo'] + objects + ['/Fe:' + str(exe), '/link', '/MACHINE:X64'],
                directory, directory / f'link-{mode}')
        data = exe.read_bytes()
        require(data[:2] == b'MZ', 'Invalid executable signature')
        pe = int.from_bytes(data[0x3c:0x40], 'little')
        require(data[pe:pe+4] == b'PE\0\0' and int.from_bytes(data[pe+4:pe+6], 'little') == 0x8664
                and int.from_bytes(data[pe+24:pe+26], 'little') == 0x20b, 'Build is not x64 PE32+')
        executables[mode] = exe
    for log in directory.glob('*.txt'):
        require(not re.search(r'\bwarning\s+(?:C\d+|D\d+|LNK\d+)', log.read_text(encoding='utf-8', errors='replace')),
                f'Compiler warning needs review: {log}')
    write_json(output / 'build-status.json', {'status': 'PASS', 'compiler': str(compiler),
               'executables': {key: {'path': str(value), 'sha256': sha(value)} for key, value in executables.items()}})
    return executables

def write_list(path, members, source_paths):
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream, delimiter='\t', lineterminator='\n', quoting=csv.QUOTE_NONE)
        writer.writerow(['member', 'offset', 'length', 'fnv64', 'path'])
        for row in members:
            source = str(source_paths[row['member']])
            require(not any(c in source for c in '\t\n\r'), 'Unsupported input path characters')
            writer.writerow([row['member'], row['offset'], row['length'], row['fnv64'], source])

def smoke(repo, output, exes):
    directory = output / 'smoke'
    directory.mkdir()
    for label, exe in [('unit-reference', exes['oracle']), ('unit-engine', exes['production'])]:
        dest = directory / f'{label}.csv'
        command([exe, '--unit', dest], directory, directory / label)
        result = rows(dest)
        require(len(result) == 26 and len({r['test'] for r in result}) == 26 and
                all(r['status'] == 'PASS' and r['passed'] == '1' for r in result), f'{label} checks failed')
    dest = directory / 'oracle.csv'
    command([exes['oracle'], '--oracle', repo / 'protocol/native-oracle-vectors.csv', dest],
            directory, directory / 'oracle')
    result = rows(dest)
    require(len(result) == 144 and len({(r['pool'], r['requested_k']) for r in result}) == 144 and
            all(r['status'] == 'PASS' and r['partition_equal'] == '1' and r['expected_k'] == r['actual_k'] for r in result),
            'Native oracle checks failed')
    members = [r for r in rows(repo / 'protocol/members.csv') if r['workload'] == 'syn-05']
    require(len(members) == 1, 'Synthetic protocol missing')
    test_list = directory / 'syn-05.tsv'
    write_list(test_list, members, {r['member']: repo / 'fixtures' / r['file'] for r in members})
    command([exes['production'], '--audit-list', test_list, directory / 'stream', '--inject-mismatch'],
            directory, directory / 'negative-stream', expected=4)
    command([exes['production'], '--audit-list', directory / 'missing.tsv', directory / 'missing'],
            directory, directory / 'negative-missing', expected=2)
    command([exes['memory'], '--bench-list', test_list, directory / 'instrumented', 'symmetric', '0', '--quick'],
            directory, directory / 'negative-instrumented', expected=4)
    command([exes['production'], '--bench-list', test_list, directory / 'batch', 'symmetric', '0', '--quick', '--inject-mismatch'],
            directory, directory / 'negative-batch', expected=4)
    bad_list = directory / 'altered.tsv'
    bad_list.write_text(re.sub(r'\t[0-9a-f]{16}\t', '\t0000000000000000\t', test_list.read_text(encoding='utf-8')), encoding='utf-8')
    command([exes['production'], '--audit-list', bad_list, directory / 'altered'],
            directory, directory / 'negative-altered', expected=2)
    write_json(output / 'smoke-status.json', {'status': 'PASS', 'unit_checks': 52, 'native_oracle_checks': 144,
                                            'expected_failure_controls': 5, 'publication_timing': False})

def inputs(repo, output, data_root):
    require(data_root is not None, 'inputs/audit/full requires --data-root or --study-root')
    corpus = rows(repo / 'protocol/audited-corpus-manifest.csv')
    members = rows(repo / 'protocol/members.csv')
    workloads = read_json(repo / 'protocol/workloads.json')
    require(len(corpus) == 41 and len(members) == 771 and len(workloads) == 87, 'Protocol count mismatch')
    for group in ('calgary18', 'canterbury11', 'silesia'):
        directory = data_root / group
        require(directory.is_dir(), f'Missing corpus directory: {directory}')
        expected = {r['file'] for r in corpus if r['corpus'] == group}
        actual = {p.name for p in directory.iterdir()}
        require(expected == actual and all(p.is_file() for p in directory.iterdir()),
                f'Corpus inventory mismatch: {directory}; expected only manifest files')
    for row in corpus:
        path = data_root / row['corpus'] / row['file']
        require(sha(path) == row['sha256'], f'Corpus SHA256 mismatch: {path}')
    paths = {}
    for row in members:
        path = (repo / 'fixtures' / row['file']) if row['group'] == 'synthetic' else data_root / row['relative_path'].split('/')[1] / row['file']
        require(path.is_file() and path.stat().st_size == int(row['source_bytes']), f'Member source length mismatch: {path}')
        offset, length = int(row['offset']), int(row['length'])
        require(offset >= 0 and length >= 0 and offset + length <= path.stat().st_size, 'Invalid member range')
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            stream.seek(offset)
            remaining = length
            while remaining:
                block = stream.read(min(remaining, 1024 * 1024))
                require(bool(block), 'Short input read')
                digest.update(block)
                remaining -= len(block)
        require(digest.hexdigest() == row['sha256'], f'Frozen member SHA256 mismatch: {row["member"]}')
        require(row['member'] not in paths, 'Duplicate member')
        paths[row['member']] = path
    directory = output / 'lists'
    directory.mkdir()
    for spec in workloads:
        selected = [r for r in members if r['workload'] == spec['id']]
        require(len(selected) == spec['members'], 'Workload membership mismatch')
        write_list(directory / (spec['id'] + '.tsv'), selected, paths)
    write_json(output / 'input-status.json', {'status': 'PASS', 'corpus_files': 41, 'members': 771, 'workloads': 87,
               'data_root': str(data_root), 'synthetic_root': str(repo / 'fixtures')})
    return workloads

CHECKER = r'''param([string]$Repo,[string]$Workload,[string]$AuditPrefix,[string]$MemoryPrefix,[string]$TimingPrefix,[string]$Mode)
$ErrorActionPreference='Stop'; Set-StrictMode -Version 2.0
$invariant=[Globalization.CultureInfo]::InvariantCulture
[Threading.Thread]::CurrentThread.CurrentCulture=$invariant
$experiment=ConvertFrom-Json -InputObject ([IO.File]::ReadAllText((Join-Path $Repo 'protocol\experiment.json')))
$variants=@($experiment.variants)
$memberRows=@(Import-Csv -LiteralPath (Join-Path $Repo 'protocol\members.csv'))
$workloads=ConvertFrom-Json -InputObject ([IO.File]::ReadAllText((Join-Path $Repo 'protocol\workloads.json')))
$spec=@($workloads | Where-Object {$_.id -eq $Workload})[0]
$members=@($memberRows | Where-Object {$_.workload -eq $Workload})
$oldSelected=@{}
foreach($r in @(Import-Csv -LiteralPath (Join-Path $Repo 'protocol\dcc004-selected.csv'))) {$oldSelected["$($r.group)|$($r.file)|$($r.variant)"]=$r}
. ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $Repo 'tests\runner-checks.ps1.inc'))))
if($Mode -eq 'audit') {
 Check-Loaded $AuditPrefix $members
 foreach($m in $members) {[void](Check-Validation ($AuditPrefix+'-'+$m.member) $m)}
} elseif($Mode -eq 'memory') {
 Check-Loaded $MemoryPrefix $members
 foreach($m in $members) {$gold=@(Import-Csv -LiteralPath ($AuditPrefix+'-'+$m.member+'.selected.csv')); [void](Check-Memory ($MemoryPrefix+'-'+$m.member) $gold)}
} elseif($Mode -eq 'timing') {
 $goldByMember=@{}
 foreach($m in $members) {$goldByMember[$m.member]=@(Import-Csv -LiteralPath ($AuditPrefix+'-'+$m.member+'.selected.csv'))}
 [void](Check-WorkloadTiming $TimingPrefix $spec $members $goldByMember)
} else {throw 'Unknown check mode'}
Write-Output 'PASS'
'''

def environment(output, powershell, compiler):
    dest = output / 'environment.json'
    script = "$ErrorActionPreference='Stop'; [Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false); [ordered]@{CapturedUtc=[DateTime]::UtcNow.ToString('o');Windows=(Get-CimInstance Win32_OperatingSystem|Select-Object Caption,Version,BuildNumber,OSArchitecture);CPU=@(Get-CimInstance Win32_Processor|Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);MemoryBytes=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory;PowerShellVersion=$PSVersionTable.PSVersion.ToString();Is64BitProcess=[Environment]::Is64BitProcess;CompilerVersion=(Get-Item -LiteralPath " + ps_literal(compiler) + ").VersionInfo.FileVersion}|ConvertTo-Json -Depth 8"
    log = output / 'capture-environment'
    command([powershell, '-NoLogo', '-NoProfile', '-Command', script], output, log)
    env = read_json(str(log) + '.stdout.txt')
    env['compiler_path'] = str(compiler)
    env['python_version'] = sys.version
    write_json(dest, env)

def power(output, label):
    directory = output / 'power'
    directory.mkdir(exist_ok=True)
    powercfg = Path(os.environ['SystemRoot']) / 'System32/powercfg.exe'
    active = directory / (label + '-active')
    command([powercfg, '/getactivescheme'], directory, active)
    text = Path(str(active) + '.stdout.txt').read_text(encoding='utf-8', errors='replace')
    match = re.search(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', text)
    require(match is not None, 'Cannot identify active power plan')
    guid = match.group().lower()
    settings = directory / (label + '-settings')
    command([powercfg, '/query', guid], directory, settings)
    return {'guid': guid, 'settings_sha256': sha(str(settings) + '.stdout.txt')}

def experiments(repo, output, exes, workloads, powershell, full):
    checker = output / 'check-records.ps1'
    checker.write_text(CHECKER, encoding='utf-8')
    baseline_power = power(output, 'initial') if full else None
    if full:
        require(baseline_power['guid'] == '381b4222-f694-41f0-9685-ff5bb260df2e',
                'Original experiment used Balanced power. Select Balanced before a new full timing run.')
    for mode in ('audit', 'memory') + (('timing',) if full else ()):
        for ordinal, spec in enumerate(workloads, 1):
            if mode == 'timing' and spec['scope'] == 'synthetic':
                continue
            print(f'{mode}: {ordinal}/87 {spec["id"]}', flush=True)
            directory = output / 'jobs' / (spec['id'] + '-' + mode)
            directory.mkdir(parents=True)
            prefix = directory / 'records'
            input_list = output / 'lists' / (spec['id'] + '.tsv')
            audit_prefix = output / 'jobs' / (spec['id'] + '-audit') / 'records'
            memory_prefix = output / 'jobs' / (spec['id'] + '-memory') / 'records'
            if mode == 'audit':
                args = [exes['production'], '--audit-list', input_list, prefix]
            elif mode == 'memory':
                args = [exes['memory'], '--memory-list', input_list, prefix]
            else:
                require(power(output, spec['id'] + '-before') == baseline_power, 'Power settings changed before timing')
                args = [exes['production'], '--bench-list', input_list, prefix, spec['timing_mode'], str(ordinal)]
            command(args, directory, directory / mode)
            if mode == 'timing':
                require(power(output, spec['id'] + '-after') == baseline_power, 'Power settings changed during timing')
            # Same in-memory script-block execution mechanism as the historical
            # launcher; no system execution-policy change and no shell quoting.
            check_command = '& ([scriptblock]::Create([IO.File]::ReadAllText(' + ps_literal(checker) + ')))'
            for key, value in [('Repo', repo), ('Workload', spec['id']), ('AuditPrefix', audit_prefix),
                               ('MemoryPrefix', memory_prefix), ('TimingPrefix', prefix), ('Mode', mode)]:
                check_command += ' -' + key + ' ' + ps_literal(value)
            command([powershell, '-NoLogo', '-NoProfile', '-Command', check_command],
                    directory, directory / 'validate')
    write_json(output / 'experiment-status.json', {'status': 'PASS', 'audit_workloads': 87, 'memory_workloads': 87,
               'timed_workloads': 77 if full else 0, 'original_results_modified': False,
               'new_measurements': True, 'source_of_original_paper_timings': False})

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--repo-root', type=Path, required=True, help='Public package root containing source/tests/vendor/protocol/package-files.csv')
    parser.add_argument('--output-dir', type=Path, required=True, help='NEW directory, outside original study, repo, and corpus inputs')
    parser.add_argument('--phase', required=True, choices=['build', 'smoke', 'inputs', 'audit', 'full'])
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--study-root', type=Path, help='Existing DCC - study root; reads its data subfolder only')
    group.add_argument('--data-root', type=Path, help='Directory directly containing calgary18/canterbury11/silesia')
    parser.add_argument('--allow-long-run', action='store_true', help='Required for audit/full, which may take many hours')
    args = parser.parse_args()
    repo, output = args.repo_root.resolve(), args.output_dir.resolve()
    study = args.study_root.resolve() if args.study_root else None
    data = args.data_root.resolve() if args.data_root else (study / 'data' if study else None)
    require(repo.is_dir(), 'Repository root does not exist')
    require(not output.exists(), f'Output directory already exists; choose a NEW name: {output}')
    for protected in [repo, study, data]:
        if protected is not None:
            require(not below(output, protected), f'Output must be outside protected input/repository directory: {protected}')
    if args.phase in ('audit', 'full'):
        require(args.allow_long_run, 'audit/full may take hours; repeat with --allow-long-run only when intended.')
    if args.phase in ('inputs', 'audit', 'full'):
        require(data is not None, 'Provide --study-root or --data-root for corpus inputs')
    if args.phase != 'inputs':
        for path in [repo, output, study, data]:
            if path is not None:
                require(str(path).isascii(), 'Use ASCII-only directory names for native reproduction; '
                        'the frozen C driver uses narrow Windows file paths. Spaces are supported.')
    output.mkdir(parents=True)
    status = {'status': 'STARTED', 'phase': args.phase, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'wrapper_version': '1.0.0-publication', 'repo_root': str(repo), 'output_dir': str(output),
              'command': sys.argv, 'execution_platform': sys.platform, 'running_on_windows': os.name == 'nt',
              'wrapper_sha256': sha(Path(__file__).resolve())}
    write_json(output / 'run-status.json', status)
    awake = False
    try:
        scientific_integrity(repo, output)
        if args.phase == 'inputs':
            inputs(repo, output, data)
        else:
            compiler, powershell = windows_tools()
            environment(output, powershell, compiler)
            exes = build(repo, output, compiler)
            if args.phase in ('smoke', 'audit', 'full'):
                smoke(repo, output, exes)
            if args.phase in ('audit', 'full'):
                workloads = inputs(repo, output, data)
                import ctypes
                require(ctypes.windll.kernel32.SetThreadExecutionState(0x80000001) != 0, 'Unable to prevent idle sleep')
                awake = True
                experiments(repo, output, exes, workloads, powershell, args.phase == 'full')
                postcheck = output / 'post-input-integrity'
                postcheck.mkdir()
                inputs(repo, postcheck, data)
        scientific_integrity(repo, output)
        status['status'] = 'PASS'
    except BaseException as exc:
        status.update(status='FAIL', error=str(exc))
        raise
    finally:
        if awake:
            import ctypes
            ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)
        status['completed_utc'] = datetime.now(timezone.utc).isoformat()
        write_json(output / 'run-status.json', status)
    print(f'PASS: {args.phase}. Outputs: {output}', flush=True)

if __name__ == '__main__':
    try:
        main()
    except (Exception, KeyboardInterrupt) as exc:
        print(f'STOPPED: {exc}', file=sys.stderr)
        sys.exit(1)
