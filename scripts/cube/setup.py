#!/usr/bin/env python3
"""Cube Assist: guided install for an already unlocked, rooted Fire TV Cube 3."""
import argparse,datetime,hashlib,ipaddress,json,os,re,shlex,shutil,socket,subprocess,sys,tempfile,time,urllib.request,zipfile
from pathlib import Path

ROOT='/data/local/cube-assist'
TRIAL='/data/local/tmp/cube-assist-trial'
BACKUPS='/data/local/cube-assist-backups'
HOOK='/data/adb/service.d/90-cube-assist.sh'
HERE=Path(__file__).resolve().parent
REPO=HERE.parents[1]
FILES={'hassmic','classes.dex','service.sh'}
TESTED_BUILD='PS7717.5741N'

def quoted(value):return shlex.quote(str(value))
def endpoint(value):
    host,sep,port=value.partition(':')
    ipaddress.IPv4Address(host)
    if sep and (not port.isdecimal() or not 1<=int(port)<=65535):raise ValueError('Invalid ADB port')
    return host+':'+(port or '5555')
def validate_bundle(path):
    with zipfile.ZipFile(path) as z:
        names=z.namelist()
        if len(names)!=len(set(names)) or set(names)!=FILES|{'manifest.json'}:raise ValueError('Unexpected bundle contents')
        if any(i.file_size>32*1024*1024 for i in z.infolist()):raise ValueError('Bundle file too large')
        m=json.loads(z.read('manifest.json'))
        if m.get('format')!=1 or m.get('device')!='gazelle' or set(m.get('files',{}))!=FILES:raise ValueError('Not a Cube Assist gazelle bundle')
        content={name:z.read(name) for name in FILES}
        for name,data in content.items():
            if hashlib.sha256(data).hexdigest()!=m['files'][name]:raise ValueError('Checksum mismatch: '+name)
        if not content['hassmic'].startswith(b'\x7fELF') or not content['classes.dex'].startswith(b'dex\n'):raise ValueError('Invalid executable format')
        return m,content

class Device:
    def __init__(self,address,adb='adb'):
        self.address=endpoint(address);self.host=self.address.split(':')[0];self.adb=adb
    def call(self,args,timeout=30):
        p=subprocess.run([self.adb,'-s',self.address,*args],capture_output=True,text=True,timeout=timeout)
        if p.returncode:raise RuntimeError((p.stderr or p.stdout).strip() or 'ADB failed')
        return p.stdout.strip()
    def shell(self,command,timeout=30):return self.call(['shell','su','-c',quoted(command)],timeout)
    def connect(self):
        subprocess.run([self.adb,'connect',self.address],capture_output=True,timeout=15,check=True)
        if self.call(['get-state'])!='device':raise RuntimeError('Accept the ADB debugging prompt on the TV, then run setup again.')
    def exists(self,path):return self.shell(f'if [ -e {quoted(path)} ]; then echo yes; fi')=='yes'
    def put(self,data,dest,mode='600'):
        # Fixed temporary location; no arbitrary input is interpolated as shell code.
        with tempfile.TemporaryDirectory(prefix='cube-assist-') as td:
            f=Path(td)/'payload';f.write_bytes(data)
            self.call(['push',str(f),'/data/local/tmp/cube-assist-transfer'])
        self.shell(f'cp /data/local/tmp/cube-assist-transfer {quoted(dest)} && chmod {mode} {quoted(dest)} && rm -f /data/local/tmp/cube-assist-transfer')
    def service(self,action,root=ROOT):return self.shell(f'CUBE_ASSIST_HOME={quoted(root)} sh {quoted(root+"/service.sh")} {action}',45)

def preflight(d,persistent=False):
    if d.shell('id -u')!='0':raise RuntimeError('Root is required. Unlock and root the Cube before using this installer.')
    product=d.shell('getprop ro.product.device');sdk=d.shell('getprop ro.build.version.sdk')
    build=d.shell('getprop ro.build.display.id')
    if product!='gazelle' or sdk!='28':raise RuntimeError(f'Unsupported device: {product}, Android API {sdk}. Only Cube 3 gazelle/API28 is supported.')
    if TESTED_BUILD not in build:raise RuntimeError(f'Firmware {build} has not been validated. Supported: {TESTED_BUILD}. No changes made.')
    for path in ['/system/lib/libpryon.so','/system/lib/libopus.so','/system/framework/aspclient.jar','/system/bin/ledctrl']:
        if not d.exists(path):raise RuntimeError('Missing required stock component: '+path)
    if persistent:
        if not d.exists('/data/adb/service.d') or not d.shell('magisk -V').isdigit():raise RuntimeError('A working Magisk service.d boot mechanism is required for persistent installation.')
    original=d.shell('settings get secure alexa_wake_word_disabled')
    if original not in ('0','1','null'):raise RuntimeError('Cannot safely snapshot the Alexa wake setting')
    return {'device':product,'sdk':sdk,'firmware':build,'original_wake_setting':original}

def health(d,root,port,seconds=12):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        try:
            with urllib.request.urlopen(f'http://{d.host}:28931/api/hello',timeout=3) as r:hello=json.load(r)
            if hello.get('codename')!='gazelle':raise RuntimeError('Wrong device replied on settings port')
            with socket.create_connection((d.host,port),timeout=2):pass
            status=d.shell(f'p=$(cat {quoted(root+"/native.pid")}); a=$(cat {quoted(root+"/audio.pid")}); kill -0 "$p" && kill -0 "$a" && grep -q "capture running" {quoted(root+"/audio.log")} && echo healthy')
            if status.endswith('healthy'):return hello
        except (OSError,ValueError,RuntimeError):pass
        time.sleep(1)
    raise RuntimeError('New service did not become healthy; restoring the previous installation.')

def stamp():return datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S')+'-'+os.urandom(3).hex()
def restore_wake(d,value):
    if value=='null':d.shell('settings delete secure alexa_wake_word_disabled')
    elif value in ('0','1'):d.shell('settings put secure alexa_wake_word_disabled '+value)

def install(d,bundle,protocol='esphome',trial=False,preset=None):
    manifest,files=validate_bundle(bundle)
    preset_data=None
    if preset:
        preset_data=Path(preset).read_bytes()
        if len(preset_data)>65536 or b'\0' in preset_data:raise ValueError('Preset must be a settings export of at most 64 KiB')
        preset_data.decode('utf-8')
    report=preflight(d,not trial);root=TRIAL if trial else ROOT
    other=ROOT if trial else TRIAL
    if d.exists(other+'/run.lock'):raise RuntimeError('Stop the other Cube Assist instance first; trial and installed services cannot share the microphone.')
    if not trial and d.exists(HOOK) and 'Cube Assist owned boot hook' not in d.shell('cat '+HOOK):raise RuntimeError('The boot-hook filename belongs to another installation; left unchanged.')
    tag=stamp();backup=BACKUPS+'/'+tag;existed=d.exists(root)
    original=report['original_wake_setting']
    if existed:
        if not d.exists(root+'/service.sh') or not d.exists(root+'/original-wake-setting'):raise RuntimeError('Existing directory is not a recognised Cube Assist installation')
        original=d.shell('cat '+root+'/original-wake-setting')
        if original not in ('0','1','null'):raise RuntimeError('Existing restore snapshot invalid')
        d.service('stop',root)
    d.shell(f'mkdir -p {BACKUPS} && chmod 700 {BACKUPS} && mkdir -m 700 {backup}')
    hook_existed=not trial and d.exists(HOOK)
    if hook_existed:d.shell(f'cp -p {HOOK} {backup}/boot-hook')
    if existed:d.shell(f'cp -a {root} {backup}/installation',120)
    print('Backup:',backup)
    try:
        d.shell(f'mkdir -p {root}/current {root}/state {root}/models && chmod 700 {root} {root}/current {root}/state {root}/models')
        d.put((original+'\n').encode(),root+'/original-wake-setting')
        for name,data in files.items():
            dest=root+'/service.sh' if name=='service.sh' else root+'/current/'+name
            d.put(data,dest,'700' if name!='classes.dex' else '600')
            if d.shell('sha256sum '+dest).split()[0]!=manifest['files'][name]:raise RuntimeError('Device checksum mismatch: '+name)
        d.put((protocol+'\n').encode(),root+'/protocol')
        if preset_data is not None:d.put(preset_data,root+'/state/preset')
        if not d.exists(root+'/extra-duck-db'):d.put(b'6\n',root+'/extra-duck-db')
        d.put(json.dumps({'format':1,'backup':backup,'preflight':report,'bundle':manifest,'trial':trial},indent=2).encode(),root+'/install.json')
        # A failed PC connection cannot leave a first trial permanently installed.
        d.service('start 600' if trial else 'start 0',root)
        hello=health(d,root,10700 if protocol=='wyoming' else 26053)
        time.sleep(3);health(d,root,10700 if protocol=='wyoming' else 26053,5)
        if not trial:
            hook=b'#!/system/bin/sh\n# Cube Assist owned boot hook\nCUBE_ASSIST_HOME=/data/local/cube-assist sh /data/local/cube-assist/service.sh start 0\n'
            d.put(hook,HOOK,'700')
        print('Installed:',hello['name'],'('+manifest['version']+')')
        return hello
    except BaseException:
        try:
            if d.exists(root+'/service.sh'):d.service('stop',root)
            if d.exists(root):d.shell(f'mv {root} {backup}/failed-installation')
            if existed:
                d.shell(f'cp -a {backup}/installation {root}',120);d.service('start 600' if trial else 'start 0',root)
            else:restore_wake(d,original)
            if not trial:
                if hook_existed:d.shell(f'cp -p {backup}/boot-hook {HOOK}')
                elif d.exists(HOOK):d.shell(f'mv {HOOK} {backup}/failed-boot-hook')
            print('Previous files and Alexa setting restored. Evidence retained at',backup)
        except Exception as e:print('Automatic restoration needs attention:',e,'Backup:',backup,file=sys.stderr)
        raise

def uninstall(d,trial=False):
    root=TRIAL if trial else ROOT
    if not d.exists(root+'/original-wake-setting'):raise RuntimeError('No recognised installation to uninstall')
    original=d.shell('cat '+root+'/original-wake-setting')
    if original not in ('0','1','null'):raise RuntimeError('Restore snapshot invalid; stopped without removing files')
    d.service('stop',root)
    backup=BACKUPS+'/uninstalled-'+stamp();d.shell(f'mkdir -p {BACKUPS} && mkdir -m 700 {backup}')
    if not trial and d.exists(HOOK):
        if 'Cube Assist owned boot hook' not in d.shell('cat '+HOOK):raise RuntimeError('Boot hook is not ours; left untouched')
        d.shell(f'mv {HOOK} {backup}/boot-hook')
    d.shell(f'mv {root} {backup}/installation');restore_wake(d,original)
    print('Uninstalled. Alexa setting restored. Settings and files retained at',backup)

def guide(d,protocol,interactive=True,trial=False):
    print(f'\n1. Home Assistant: Settings > Devices & services > Add integration > '+('ESPHome' if protocol=='esphome' else 'Wyoming Protocol'))
    print(f'   Host {d.host}, port '+('26053' if protocol=='esphome' else '10700')+'. Choose your working Assist pipeline.')
    print('   Disable the old Cube Assist Temporary Wyoming entry if replacing the prototype.')
    print(f'2. Settings: http://{d.host}:28931 . Choose Ask, then briefly press the Cube action button to approve this browser.')
    print('   Voice: Amazon wake word; Extra TV ducking 0–24 dB. Lights: Cube and TV bar states.')
    print('3. Music Assistant: add/enable its Sendspin provider. Find this Cube; pair using its Sendspin token.')
    print(f'   Manual player address, if discovery fails: {d.host}:28928. The token is shown below and in HA’s disabled diagnostic entity.')
    print('4. Arbitration: Settings > Arbitration. Join your other EDA devices with the same key, or select the matching Kiosk mode.')
    print('5. With TV speech playing, say “Alexa, what time is it?” Check the bars, ducking, Cube reply and volume recovery.')
    print('   TV power detection is deferred. All audio output currently uses the Cube speaker.')
    if interactive:
        input('\nPress Enter after adding Home Assistant to continue. ')
    root=TRIAL if trial else ROOT
    token=d.shell(f'HASSMIC_STATE={root}/state {root}/current/hassmic -T').splitlines()[-1]
    if not token.startswith('SP:'):raise RuntimeError('Pairing token unavailable')
    print('\nSendspin pairing token (keep private):\n'+token)

def main():
    if hasattr(sys.stdout,'reconfigure'):sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['install','check','guide','status','start','stop','uninstall'],nargs='?',default='install')
    p.add_argument('--device',help='Cube IPv4 address, optionally :5555')
    p.add_argument('--adb',default='adb');p.add_argument('--bundle',type=Path,default=REPO/'dist/cube-assist-gazelle.zip')
    p.add_argument('--protocol',choices=['esphome','wyoming'],default='esphome')
    p.add_argument('--preset',type=Path,help='Settings-page export to apply on the next start')
    p.add_argument('--trial',action='store_true',help='Separate 10-minute trial; no boot hook')
    p.add_argument('--yes',action='store_true',help='Non-interactive install; print the complete setup guide')
    a=p.parse_args()
    print('Cube Assist — based on Echo Dot Assist\nAlready-unlocked Fire TV Cube 3 only. This tool does not unlock or flash a Cube.')
    address=a.device or input('Cube IP address: ').strip();d=Device(address,a.adb);d.connect()
    if a.action=='check':print(json.dumps(preflight(d,not a.trial),indent=2));return
    if a.action=='install':
        validate_bundle(a.bundle);print(json.dumps(preflight(d,not a.trial),indent=2))
        if not a.yes:
            print('Installation backs up an existing Cube Assist installation, keeps Fire OS/AFE running, and adds a removable Magisk startup hook.' if not a.trial else 'Trial runs for 10 minutes and restores Alexa when it ends.')
            if input('Install Cube Assist? [y/N] ').lower() not in ('y','yes'):return
        install(d,a.bundle,a.protocol,a.trial,a.preset);guide(d,a.protocol,not a.yes,a.trial)
    elif a.action=='guide':guide(d,a.protocol,not a.yes,a.trial)
    elif a.action=='uninstall':uninstall(d,a.trial)
    else:print(d.service('start 600' if a.action=='start' and a.trial else a.action,TRIAL if a.trial else ROOT))

if __name__=='__main__':
    try:main()
    except (Exception,KeyboardInterrupt) as e:print('\nSetup stopped:',e,file=sys.stderr);sys.exit(1)
