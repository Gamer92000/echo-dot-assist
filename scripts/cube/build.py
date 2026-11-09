#!/usr/bin/env python3
"""Build Cube artifacts with an installed Android SDK/NDK; no device writes."""
import argparse, hashlib, json, os, re, shutil, subprocess, zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
def run(args):
    subprocess.run([str(v) for v in args],cwd=ROOT,check=True)
def git(*args):
    os.environ['TZ']='UTC0'
    return subprocess.check_output(["git", *args],cwd=ROOT,text=True).strip()

def build(sdk,java,stock):
    ndks=sorted((sdk/'ndk').iterdir(),key=lambda p:tuple(int(v) for v in p.name.split('.') if v.isdigit()))
    if not ndks: raise SystemExit('Install an Android NDK in the Android SDK first.')
    host='windows-x86_64' if os.name=='nt' else 'linux-x86_64'
    clang=ndks[-1]/'toolchains/llvm/prebuilt'/host/'bin'/('armv7a-linux-androideabi28-clang.cmd' if os.name=='nt' else 'armv7a-linux-androideabi28-clang')
    platform=sorted((sdk/'platforms').glob('android-*'),key=lambda p:int(p.name.split('-')[1]))[-1]
    buildtools=sorted((sdk/'build-tools').iterdir(),key=lambda p:tuple(int(v) for v in re.findall(r'\d+',p.name)))[-1]
    out=ROOT/'build/gazelle';out.mkdir(parents=True,exist_ok=True)
    javaout=out/'java';javaout.mkdir(exist_ok=True)
    suffix='.exe' if os.name=='nt' else ''
    run([java/'bin'/('javac'+suffix),'--release','8','-cp',platform/'android.jar','-d',javaout,*sorted((ROOT/'devices/gazelle').glob('*.java'))])
    env_java=os.environ.get('JAVA_HOME');os.environ['JAVA_HOME']=str(java)
    try: run([buildtools/('d8.bat' if os.name=='nt' else 'd8'),'--min-api','28','--lib',platform/'android.jar','--output',out,*javaout.glob('*.class')])
    finally:
        if env_java is None: os.environ.pop('JAVA_HOME',None)
        else: os.environ['JAVA_HOME']=env_java
    make=(ROOT/'Makefile').read_text(encoding='utf8')
    assets=re.search(r'^WEB := (.+)$',make,re.M).group(1).split()
    run([os.sys.executable,ROOT/'tools/embed.py',ROOT/'build/web_assets.c',*assets])
    make=(ROOT/'Makefile').read_text(encoding='utf8');section=make.split('HASSMIC :=',1)[1].split('\nHASSMIC_H',1)[0]
    # The shared Makefile now selects ring.c/ring_band.c through $(RING).
    sources=re.findall(r'(?:src|build)/[\w/.-]+\.c',section.replace('$(RING)','src/hassmic/ring.c').replace('$(SCREEN)','src/hassmic/screen_none.c'))
    sources += ['src/third_party/rnnoise/'+n+'.c' for n in ['denoise','rnn','rnn_data','pitch','kiss_fft','celt_lpc']]
    sources += ['devices/gazelle/board.c','src/hassmic/audio_cube.c','src/hassmic/wake_pryon.c','src/hassmic/sound_none.c','src/hassmic/whisper_none.c']
    for name in ['libpryon.so','libopus.so']:
        if not (stock/name).is_file():raise SystemExit(f'Missing {stock/name}. Obtain it from your own unlocked Cube; do not redistribute Amazon libraries.')
    version=git('show','-s','--format=%cd','--date=format-local:%Y.%m.%d.%H%M%S','HEAD')
    revision=git('describe','--always','--dirty',"--exclude=*")
    built=git('show','-s','--format=%cI','HEAD')
    flags=['-O2','-Wall','-Wextra','-fPIE','-DCUBE=1',f'-DVERSION="{version}"',f'-DBUILD="{revision}"',f'-DBUILD_TIME="{built}"','-Isrc/include','-Isrc/hassmic']
    flags += sources+['-o',str(out/'hassmic'),'-pie','-fuse-ld=lld','-Wl,--allow-shlib-undefined','-lm','-ldl','-lz',str(stock/'libpryon.so'),str(stock/'libopus.so')]
    rsp=out/'build.rsp';rsp.write_text('\n'.join('"'+v.replace('\\','/').replace('"','\\"')+'"' for v in flags))
    run([clang,'@'+str(rsp)])
    manifest={'format':1,'product':'Cube Assist','version':version+'-dev','device':'gazelle','source_commit':git('rev-parse','HEAD'),'source_revision':revision,'files':{}}
    shutil.copyfile(ROOT/'scripts/cube/service.sh',out/'service.sh')
    # Keep upstream alignment data beside the binary; do not package the speech corpus.
    align=sorted((ROOT/'wakealign').glob('*.wa'))+sorted((ROOT/'wakealign').glob('*.was'))
    for p in align: shutil.copyfile(p,out/p.name)
    shutil.copyfile(ROOT/'wakealign/README.md',out/'wakealign-LICENSE.txt')
    for name in ['hassmic','classes.dex','service.sh','wakealign-LICENSE.txt']+[p.name for p in align]:
        manifest['files'][name]=hashlib.sha256((out/name).read_bytes()).hexdigest()
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    dist=ROOT/'dist';dist.mkdir(exist_ok=True)
    with zipfile.ZipFile(dist/'cube-assist-gazelle.zip','w',zipfile.ZIP_DEFLATED) as z:
        for name in ['manifest.json',*manifest['files']]:z.write(out/name,name)
    print(json.dumps(manifest,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,default=Path(os.environ.get('ANDROID_HOME',str(Path.home()/'AppData/Local/Android/Sdk'))))
    p.add_argument('--java',type=Path,default=Path(os.environ['JAVA_HOME']) if os.environ.get('JAVA_HOME') else None,required=not bool(os.environ.get('JAVA_HOME')))
    p.add_argument('--stock',type=Path,required=True,help='Private directory containing this Cube\'s libpryon.so and libopus.so')
    a=p.parse_args();build(a.sdk.resolve(),a.java.resolve(),a.stock.resolve())
