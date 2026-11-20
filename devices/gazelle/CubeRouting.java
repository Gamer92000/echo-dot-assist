import android.content.Context;
import android.media.*;
import android.os.*;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;

/** One routing owner for all Assist tracks. Does not change system volume, TV power or HDMI policy. */
public final class CubeRouting {
    final AudioManager manager;
    final Context context;
    final Map<AudioTrack,Integer> tracks=new IdentityHashMap<>();
    final Handler handler=new Handler(Looper.getMainLooper());
    int state=CubeRoutePolicy.UNKNOWN,candidate=-1,selected=CubeRoutePolicy.CUBE,reason=1,source;
    int active;
    volatile int mode,cubeVolume=100,tvVolume=100,musicDuck=17;
    int cec=-1;long cecAt,queryAt,changedAt,failedUntil,sampledAt;
    volatile long freshAt;
    String previous="";
    CubeRouting(AudioManager manager,Context context){this.manager=manager;this.context=context;tick();}
    static String read(String path){try{return new String(Files.readAllBytes(Paths.get(path)),"UTF-8").trim();}catch(Exception e){return "";}}
    static int integer(String path,int fallback){try{return Integer.parseInt(read(path));}catch(Exception e){return fallback;}}
    static int config(String name,int max,int fallback){int v=integer(CubeAudio.HOME+"/state/"+name,fallback);return v<0||v>max?fallback:v;}
    static String digest(String value)throws Exception{
        byte[] d=MessageDigest.getInstance("SHA-256").digest(value.getBytes("UTF-8"));StringBuilder s=new StringBuilder();for(byte b:d)s.append(String.format("%02x",b&255));return s.toString();
    }
    AudioDeviceInfo device(int type){for(AudioDeviceInfo d:manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS))if(d.getType()==type)return d;return null;}
    void queryCec(){
        try{
            Object service=context.getSystemService("hdmi_control");Object playback=service.getClass().getMethod("getPlaybackClient").invoke(service);
            if(playback==null)return;
            Class<?> cb=Class.forName("android.hardware.hdmi.HdmiPlaybackClient$DisplayStatusCallback");
            Object listener=Proxy.newProxyInstance(cb.getClassLoader(),new Class<?>[]{cb},(proxy,m,args)->{
                if(m.getName().equals("onComplete")){synchronized(this){cec=(Integer)args[0];cecAt=SystemClock.elapsedRealtime();}}return null;
            });
            playback.getClass().getMethod("queryDisplayStatus",cb).invoke(playback,listener);
        }catch(Exception e){synchronized(this){cec=-1;cecAt=0;}}
    }
    synchronized void tick(){
        long now=SystemClock.elapsedRealtime();
        mode=config("audio_output",2,0);cubeVolume=config("cube_output_volume",100,100);tvVolume=config("tv_output_volume",100,100);musicDuck=config("music_duck_percent",100,17);
        String base="/sys/class/amhdmitx/amhdmitx0/";
        int hpd=integer(base+"hpd_state",-1),rx=integer(base+"rxsense_state",-1),ready=integer(base+"ready",-1);
        boolean hdmi=device(AudioDeviceInfo.TYPE_HDMI)!=null,verified=false;
        String edid=read(base+"rawedid"),profile=read(CubeAudio.HOME+"/state/tv_receiver_profile");
        try{verified=!edid.isEmpty()&&profile.equals(digest(edid));}catch(Exception ignored){}
        long age=cecAt==0?-1:now-cecAt;
        int observed=CubeRoutePolicy.detect(cec,age,hpd,rx,ready,hdmi,verified);
        // Require two observations for a stable transition; invalid evidence clears immediately.
        if(observed==CubeRoutePolicy.UNKNOWN||observed==candidate)state=observed;
        candidate=observed;
        source=hpd==0?3:(age>=0&&age<=30000&&(cec==0||cec==1)?1:(verified&&state!=0?2:0));
        sampledAt=now;freshAt=now;
        int[] wanted=CubeRoutePolicy.select(mode,state,hdmi,true);
        if(now<failedUntil){wanted[0]=CubeRoutePolicy.CUBE;wanted[1]=CubeRoutePolicy.ROUTE_FAILED;}
        if(selected!=wanted[0])changedAt=now;
        selected=wanted[0];reason=wanted[1];
        updateTracks(now);
        publish(now);
        if(now-queryAt>=15000){queryAt=now;handler.post(this::queryCec);}
    }
    synchronized void add(AudioTrack track,int role)throws Exception{
        tracks.put(track,role);changedAt=SystemClock.elapsedRealtime();
        try{apply(track,role);}catch(Exception e){
            if(selected!=CubeRoutePolicy.TV){tracks.remove(track);throw e;}
            // The HDMI endpoint can disappear between the last sample and a reply starting.
            boolean available=device(AudioDeviceInfo.TYPE_HDMI)!=null;
            selected=CubeRoutePolicy.CUBE;reason=available?CubeRoutePolicy.ROUTE_FAILED:CubeRoutePolicy.HDMI_MISSING;
            if(available)failedUntil=SystemClock.elapsedRealtime()+10000;
            updateTracks(SystemClock.elapsedRealtime());
            try{apply(track,role);}catch(Exception fallback){tracks.remove(track);throw fallback;}
        }
    }
    synchronized void remove(AudioTrack track){tracks.remove(track);publish(SystemClock.elapsedRealtime());}
    // Audio can continue if the monitor stalls: stale Auto must not keep using HDMI.
    void beforeWrite(AudioTrack track,int role){
        if(mode!=CubeRoutePolicy.AUTO||SystemClock.elapsedRealtime()-freshAt<=5000)return;
        AudioDeviceInfo speaker=device(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
        if(speaker!=null){track.setPreferredDevice(speaker);track.setVolume(CubeRoutePolicy.gain(cubeVolume,role==2||role==4,CubeAudio.focusHeld,musicDuck));}
    }
    synchronized void gains(){for(Map.Entry<AudioTrack,Integer> e:tracks.entrySet())volume(e.getKey(),e.getValue());}
    void volume(AudioTrack t,int role){
        int v=selected==CubeRoutePolicy.TV?tvVolume:cubeVolume;
        boolean music=role==2||role==4;
        t.setVolume(CubeRoutePolicy.gain(v,music,CubeAudio.focusHeld,musicDuck));
    }
    void apply(AudioTrack t,int role)throws Exception{
        AudioDeviceInfo d=device(selected==CubeRoutePolicy.TV?AudioDeviceInfo.TYPE_HDMI:AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
        if(d==null)throw new IOException("selected output unavailable");
        AudioDeviceInfo preferred=t.getPreferredDevice();
        if(preferred==null||preferred.getId()!=d.getId()){
            if(!t.setPreferredDevice(d))throw new IOException("output preference refused");
            CubeAudio.log("route requested="+selected+" session="+t.getAudioSessionId());
        }
        volume(t,role);
    }
    void updateTracks(long now){
        boolean failed=false;
        for(Map.Entry<AudioTrack,Integer> e:tracks.entrySet()){
            try{
                apply(e.getKey(),e.getValue());
                AudioDeviceInfo actual=e.getKey().getRoutedDevice();
                int want=selected==CubeRoutePolicy.TV?AudioDeviceInfo.TYPE_HDMI:AudioDeviceInfo.TYPE_BUILTIN_SPEAKER;
                if(actual!=null&&actual.getType()!=want&&now-changedAt>2500)failed=true;
            }catch(Exception ex){CubeAudio.log("route error "+ex);failed=true;}
        }
        if(failed&&selected==CubeRoutePolicy.TV){
            selected=CubeRoutePolicy.CUBE;reason=CubeRoutePolicy.ROUTE_FAILED;failedUntil=now+10000;changedAt=now;
            for(Map.Entry<AudioTrack,Integer> e:tracks.entrySet())try{apply(e.getKey(),e.getValue());}catch(Exception ex){CubeAudio.log("fallback error "+ex);}
        }
    }
    void publish(long now){
        int actual=0;
        for(AudioTrack t:tracks.keySet()){
            AudioDeviceInfo d=t.getRoutedDevice();int a=d==null?3:(d.getType()==2?1:d.getType()==9?2:3);
            if(actual!=0&&actual!=a){actual=4;break;}actual=a;
        }
        active=actual;
        String status=state+" "+active+" "+selected+" "+reason+" "+source;
        try{
            String name=System.getenv("CUBE_ROUTING_STATUS");if(name==null)name=CubeAudio.HOME+"/routing-status";
            Path file=Paths.get(name),temp=Paths.get(name+".tmp");
            Files.write(temp,(sampledAt+" "+status+"\n").getBytes("UTF-8"));
            Files.move(temp,file,StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);
        }catch(Exception e){CubeAudio.log("route status error "+e);}
        if(!status.equals(previous)){CubeAudio.log("route state/active/selected/reason/source="+status);previous=status;}
    }
}
