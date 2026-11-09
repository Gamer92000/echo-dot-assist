"""Real HA API parser and signed web requests against the Cube C backend; fake local audio, no devices."""
import asyncio,os,signal,socket,struct,subprocess,tempfile,threading,time
from pathlib import Path
from aioesphomeapi import APIClient,SelectInfo,NumberInfo,TextSensorInfo
from webclient import Browser,wait_port
PORT,WEB=17251,17253
ROOT=Path(__file__).resolve().parents[1]
def exact(c,n):
    b=b''
    while len(b)<n:
        p=c.recv(n-len(b))
        if not p:raise EOFError()
        b+=p
    return b
def serve(c):
    try:
        with c:
            magic,role,a,b=struct.unpack('!4i',exact(c,16));assert magic==0x43554241
            c.sendall(struct.pack('!i',0))
            if role==0:
                while True:c.sendall(struct.pack('!i',1024)+bytes(1024));time.sleep(.032)
            elif role==6:c.sendall(struct.pack('!i',0))
            elif role==7:exact(c,b);c.sendall(bytes(4))
            elif role==8:exact(c,a+b);c.sendall(bytes(4))
            else:
                while True:
                    op=struct.unpack('!i',exact(c,4))[0]
                    if op==1:
                        n=struct.unpack('!i',exact(c,4))[0];exact(c,n);c.sendall(struct.pack('!i',n))
                    elif op==2:c.sendall(bytes(8))
                    elif op==3:exact(c,4);c.sendall(bytes(4));return
                    else:raise AssertionError(op)
    except (EOFError,OSError):pass
async def test(home,proc):
    client=APIClient('127.0.0.1',PORT,None)
    await client.connect(login=True)
    try:
        entities,_=await client.list_entities_services();by={e.object_id:e for e in entities}
        modes=by['audio_output'];assert isinstance(modes,SelectInfo) and modes.options==['Auto','Cube speaker','TV'],modes
        for key in ['cube_output_volume','tv_output_volume','music_duck_percent']:
            n=by[key];assert isinstance(n,NumberInfo) and n.min_value==0 and n.max_value==100,n
        for key in ['detected_tv_state','active_audio_output','audio_routing_fallback','tv_state_evidence','selected_audio_output']:assert isinstance(by[key],TextSensorInfo)
        values={};client.subscribe_states(lambda x:values.update({x.key:x.state}))
        async def expect(key,value):
            for _ in range(60):
                if values.get(by[key].key)==value:return
                await asyncio.sleep(.1)
            raise AssertionError((key,value,values.get(by[key].key)))
        await expect('audio_output','Auto');await expect('detected_tv_state','Unknown')
        client.select_command(modes.key,'TV');await expect('audio_output','TV')
        assert (home/'state/audio_output').read_text().strip()=='2'
        client.number_command(by['tv_output_volume'].key,43);await expect('tv_output_volume',43)
        client.number_command(by['cube_output_volume'].key,78);await expect('cube_output_volume',78)
        client.number_command(by['music_duck_percent'].key,25);await expect('music_duck_percent',25)
        # A fresh helper sample becomes live HA state; expiry must clear it without a reconnect.
        stamp=int(time.clock_gettime(time.CLOCK_BOOTTIME)*1000)
        (home/'routing-status').write_text(f'{stamp} 1 2 2 0 2\n')
        await expect('detected_tv_state','On');await expect('active_audio_output','TV')
        await expect('tv_state_evidence','Verified HDMI receiver sense')
        await expect('detected_tv_state','Unknown');await expect('active_audio_output','Unconfirmed (status stale)')
        browser=Browser(WEB);assert browser.login('routing host test')=='waiting';proc.send_signal(signal.SIGUSR2)
        for _ in range(30):
            if browser.login('routing host test')=='approved':break
            await asyncio.sleep(.1)
        else:raise AssertionError('web approval')
        result=browser.set(audio_output='Cube speaker',tv_output_volume=61,music_duck_percent=12)
        assert result.get('status',200)==200,result
        await expect('audio_output','Cube speaker');await expect('tv_output_volume',61);await expect('music_duck_percent',12)
        await expect('cube_output_volume',78)
        settings={s['name']:s for s in browser.state()['settings']}
        assert settings['audio_output']['value']==1,settings['audio_output']
        print('PASS: real HA entity discovery, select/number commands, independent saved levels, live/stale diagnostics, signed web changes')
    finally:await client.disconnect()
def main():
    with tempfile.TemporaryDirectory(prefix='cube-routing-api-') as temp:
        home=Path(temp);(home/'state').mkdir();name='cube-routing-test-'+str(os.getpid())
        server=socket.socket(socket.AF_UNIX);server.bind('\0'+name);server.listen()
        def accept():
            while True:
                try:c,_=server.accept()
                except OSError:return
                threading.Thread(target=serve,args=(c,),daemon=True).start()
        threading.Thread(target=accept,daemon=True).start()
        env=dict(os.environ,CUBE_ASSIST_HOME=temp,HASSMIC_STATE=str(home/'state'),CUBE_AUDIO_SOCKET=name,HASSMIC_MODELS=str(home/'models'))
        with (ROOT/'build/routing-protocol-native.log').open('w') as log:
            proc=subprocess.Popen([str(ROOT/'build/hassmic-routing-host'),'-P','esphome','-p',str(PORT),'-W',str(WEB),'-n','Cube Routing Test','-L','-b','/dev/null'],env=env,stdout=log,stderr=log)
            try:wait_port(PORT,WEB);asyncio.run(test(home,proc))
            finally:
                proc.terminate()
                try:proc.wait(timeout=5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
                server.close()
if __name__=='__main__':main()
