#!/usr/bin/env python3
"""Set actual release metadata and retain a sanitized successful Windows smoke record."""
import argparse, datetime, hashlib, json, re, sys
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--repo-root',type=Path,required=True)
    p.add_argument('--repository-url',required=True)
    p.add_argument('--doi',required=True)
    p.add_argument('--date',required=True,help='Actual intended publication date YYYY-MM-DD')
    p.add_argument('--smoke-output',type=Path,required=True)
    a=p.parse_args();r=a.repo_root.resolve()
    url=a.repository_url.rstrip('/');doi=a.doi.removeprefix('https://doi.org/')
    assert re.fullmatch(r'https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+',url),'Use your actual GitHub repository URL'
    assert re.fullmatch(r'10\.\d{4,9}/[^\s]+',doi),'Use the actual DOI reserved in your Zenodo draft'
    datetime.date.fromisoformat(a.date)
    status=json.loads((a.smoke_output/'run-status.json').read_text(encoding='utf-8-sig'))
    smoke=json.loads((a.smoke_output/'smoke-status.json').read_text(encoding='utf-8-sig'))
    assert status['status']=='PASS' and status['phase']=='smoke','Smoke execution must finish PASS'
    assert status.get('execution_platform')=='win32','This release record requires actual Windows smoke execution'
    assert smoke['status']=='PASS' and smoke['unit_checks']==52 and smoke['native_oracle_checks']==144 and smoke['expected_failure_controls']==5,'Smoke coverage differs'
    wrapper=r/'scripts/Reproduce-Study.py'
    current_hash=hashlib.sha256(wrapper.read_bytes()).hexdigest()
    assert status.get('wrapper_sha256')==current_hash,'The wrapper differs from the one used by the smoke run'
    record={k:status[k] for k in ['status','phase','started_utc','completed_utc','execution_platform','wrapper_sha256']}
    record.update({k:smoke[k] for k in ['unit_checks','native_oracle_checks','expected_failure_controls']})
    (r/'provenance/publication-smoke-validation.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
    title='mtcodec: Reproduction package for Compression and Encoding Cost in Context Clustered Huffman and rANS Coding'
    data={'title':title,'author':'Richard Leinecker','version':'v1.0.0-dcc-submission','repository_url':url,'doi':doi,'date_released':a.date}
    (r/'RELEASE.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
    cff=f'''cff-version: 1.2.0
message: "Please cite the specific archived release used in your research."
type: software
title: "{title}"
authors:
  - family-names: "Leinecker"
    given-names: "Richard"
    affiliation: "University of Central Florida"
version: "{data['version']}"
date-released: {a.date}
repository-code: "{url}"
url: "https://doi.org/{doi}"
doi: "{doi}"
'''
    (r/'CITATION.cff').write_text(cff,encoding='utf-8')
    citation=f"R. Leinecker, \"{title},\" version {data['version']}, Zenodo, {a.date[:4]}. https://doi.org/{doi}\n"
    (r/'RELEASE-CITATION.txt').write_text(citation,encoding='utf-8')
    print('Metadata saved. A reserved DOI becomes registered when the Zenodo record is published. No publication or upload performed.')

if __name__=='__main__':
    try:main()
    except Exception as e:print('STOPPED: '+str(e),file=sys.stderr);sys.exit(1)
