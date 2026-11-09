#!/usr/bin/env python3
"""Package the installer, verified payload and corresponding source; never private device data."""
import hashlib,json,zipfile
from pathlib import Path
from setup import validate_bundle
ROOT=Path(__file__).resolve().parents[2]
bundle=ROOT/'dist/cube-assist-gazelle.zip'
manifest,_=validate_bundle(bundle)
out=ROOT/'dist'/('Cube-Assist-'+manifest['version']+'.zip')
files={
    'README.md':ROOT/'devices/gazelle/README.md','LICENSE':ROOT/'LICENSE',
    'scripts/cube/setup.py':ROOT/'scripts/cube/setup.py',
    'dist/cube-assist-gazelle.zip':bundle,
}
# Include source and embedded third-party licence notices so recipients can rebuild/relink.
for directory in ['src','web','devices/gazelle','tools','wakealign']:
    for p in (ROOT/directory).rglob('*'):
        if p.is_file() and '__pycache__' not in p.parts:files['source/'+p.relative_to(ROOT).as_posix()]=p
for name in ['Makefile','LICENSE','scripts/cube/build.py','scripts/cube/service.sh','scripts/cube/setup.py']:
    files['source/'+name]=ROOT/name
with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
    for name,p in sorted(files.items()):
        info=zipfile.ZipInfo.from_file(p,name)
        info.create_system=3;info.external_attr=((0o100755 if name.endswith('.sh') else 0o100644)<<16)
        info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,p.read_bytes())
digest=hashlib.sha256(out.read_bytes()).hexdigest()
out.with_suffix('.zip.sha256').write_text(digest+'  '+out.name+'\n')
print(json.dumps({'artifact':str(out),'bytes':out.stat().st_size,'sha256':digest,'includes_corresponding_source':True},indent=2))
