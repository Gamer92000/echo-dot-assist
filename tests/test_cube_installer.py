"""Installer trust-boundary and failure-path tests; no device is contacted."""
import hashlib,importlib.util,json,tempfile,unittest,zipfile
from pathlib import Path
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('cube_setup',Path(__file__).resolve().parents[1]/'scripts/cube/setup.py')
setup=importlib.util.module_from_spec(spec);spec.loader.exec_module(setup)

def bundle(path,mutate=None):
    files={'hassmic':b'\x7fELFtest','classes.dex':b'dex\n035\0','service.sh':b'#!/system/bin/sh\n'}
    manifest={'format':1,'device':'gazelle','version':'test','files':{k:hashlib.sha256(v).hexdigest() for k,v in files.items()}}
    if mutate:mutate(files,manifest)
    with zipfile.ZipFile(path,'w') as z:
        z.writestr('manifest.json',json.dumps(manifest))
        for k,v in files.items():z.writestr(k,v)

class Fake:
    def __init__(self,existing=False):self.existing=existing;self.commands=[];self.written={};self.services=[]
    def exists(self,path):
        if path==setup.HOOK or path==setup.TRIAL+'/run.lock':return False
        return path in self.written or self.existing and (path==setup.ROOT or path in [setup.ROOT+'/service.sh',setup.ROOT+'/original-wake-setting'])
    def shell(self,cmd,*a):
        self.commands.append(cmd)
        if cmd.startswith('cat ') and cmd.endswith('/original-wake-setting'):return '0'
        if cmd.startswith('sha256sum '):return hashlib.sha256(self.written[cmd.split()[1]]).hexdigest()+' file'
        return ''
    def put(self,data,dest,mode='600'):self.written[dest]=data
    def service(self,action,root=setup.ROOT):self.services.append((action,root))

class InstallerTests(unittest.TestCase):
    def setUp(self):self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)/'bundle.zip';bundle(self.path)
    def tearDown(self):self.temp.cleanup()
    def test_valid_bundle(self):self.assertEqual(setup.validate_bundle(self.path)[0]['device'],'gazelle')
    def test_alignment_resources_hashed_and_allowed(self):
        def add(files, manifest):
            for name in ['alexa-en-GB.wa', 'english_mfa.was', 'wakealign-LICENSE.txt']:
                files[name] = b'resource'
                manifest['files'][name] = hashlib.sha256(files[name]).hexdigest()
        bundle(self.path, add)
        self.assertIn('english_mfa.was', setup.validate_bundle(self.path)[1])
        with zipfile.ZipFile(self.path, 'a') as z: z.writestr('unexpected.key', b'not permitted')
        with self.assertRaises(ValueError): setup.validate_bundle(self.path)

    def test_tampered_binary_refused_before_device_access(self):
        bundle(self.path,lambda f,m:f.update(hassmic=b'changed'))
        with self.assertRaises(ValueError):setup.install(None,self.path)
    def test_zip_path_escape_refused(self):
        with zipfile.ZipFile(self.path,'a') as z:z.writestr('../service.sh','bad')
        with self.assertRaises(ValueError):setup.validate_bundle(self.path)
    def test_host_shell_injection_refused(self):
        for host in ['192.0.2.10;reboot','host$(bad)','192.0.2.10:5555;id','192.0.2.10:0']:
            with self.assertRaises(ValueError):setup.endpoint(host)
    def test_wrong_device_read_only(self):
        class Wrong(Fake):
            def shell(self,cmd,*a):self.commands.append(cmd);return {'id -u':'0','getprop ro.product.device':'donut','getprop ro.build.version.sdk':'28'}.get(cmd,'')
        d=Wrong()
        with self.assertRaises(RuntimeError):setup.preflight(d)
        self.assertFalse(d.written);self.assertTrue(all(c=='id -u' or c.startswith('getprop ') for c in d.commands))
    def test_failed_first_install_restores_alexa_without_boot_hook(self):
        d=Fake()
        with patch.object(setup,'preflight',return_value={'original_wake_setting':'0'}),patch.object(setup,'health',side_effect=RuntimeError('no mic')):
            with self.assertRaises(RuntimeError):setup.install(d,self.path)
        self.assertNotIn(setup.HOOK,d.written)
        self.assertIn('settings put secure alexa_wake_word_disabled 0',d.commands)
        self.assertIn(('stop',setup.ROOT),d.services)
    def test_failed_update_restores_and_restarts_previous_installation(self):
        d=Fake(True)
        with patch.object(setup,'preflight',return_value={'original_wake_setting':'1'}),patch.object(setup,'health',side_effect=RuntimeError('no mic')):
            with self.assertRaises(RuntimeError):setup.install(d,self.path)
        self.assertEqual(d.services[-1],('start 0',setup.ROOT))
        self.assertTrue(any(c.startswith('cp -a ') and c.endswith('/installation '+setup.ROOT) for c in d.commands))
        self.assertEqual(d.written[setup.ROOT+'/original-wake-setting'],b'0\n')

if __name__=='__main__':unittest.main()
