#!/usr/bin/env python3
"""Read-only checks for the prepared mtcodec publication files (Python stdlib)."""
import argparse, csv, hashlib, json, re, sys, zipfile
from pathlib import Path

def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo-root',type=Path,required=True)
    p.add_argument('--evidence-zip',type=Path)
    p.add_argument('--release',action='store_true')
    a=p.parse_args();r=a.repo_root.resolve()
    with (r/'provenance/frozen-files.csv').open(encoding='utf-8-sig',newline='') as f:rows=list(csv.DictReader(f))
    assert len(rows)==44,'Expected44 frozen scientific files'
    for x in rows:
        q=r/x['path'];assert q.is_file(),f'Missing {q}'
        assert q.stat().st_size==int(x['bytes']) and sha(q)==x['sha256'],f'Changed frozen file {q}'
    for name in ['README.md','LICENSE.txt','THIRD_PARTY_NOTICES.md','CITATION.cff','.gitattributes','.gitignore','scripts/Reproduce-Study.py','scripts/Export-Paper-Results.py','scripts/Acquire-Inputs.py']:
        assert (r/name).is_file(),f'Missing publication file {name}'
    evidence_verified=False
    if a.evidence_zip:
        info=json.loads((r/'provenance/publication-derivation.json').read_text())
        assert sha(a.evidence_zip)==info['public_evidence_sha256'],'Public evidence archive hash mismatch'
        with zipfile.ZipFile(a.evidence_zip) as z:assert z.testzip() is None,'Evidence ZIP damaged'
        evidence_verified=True
    if a.release:
        info=json.loads((r/'RELEASE.json').read_text())
        assert re.fullmatch(r'https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+',info['repository_url']),'Repository URL missing/invalid'
        assert re.fullmatch(r'10\.\d{4,9}/[^\s]+',info['doi']),'Real reserved DOI required'
        assert re.fullmatch(r'\d{4}-\d{2}-\d{2}',info['date_released']),'Release date required'
        v=json.loads((r/'provenance/publication-smoke-validation.json').read_text())
        assert v['status']=='PASS' and v['phase']=='smoke' and v['execution_platform']=='win32','Passing Windows smoke record required'
        assert v['unit_checks']==52 and v['native_oracle_checks']==144 and v['expected_failure_controls']==5,'Smoke record coverage differs'
        assert v['wrapper_sha256']==sha(r/'scripts/Reproduce-Study.py'),'Wrapper changed since recorded smoke validation'
    print(json.dumps({'status':'PASS','frozen_files':len(rows),'evidence_zip_verified':evidence_verified,'release_metadata_checked':a.release},indent=2))

if __name__=='__main__':
    try:main()
    except Exception as e:print('STOPPED: '+str(e),file=sys.stderr);sys.exit(1)
