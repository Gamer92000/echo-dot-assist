import android.media.*;
import android.os.SystemClock;
import java.io.*;
import java.lang.reflect.*;
import java.util.*;

/** Extra attenuation, separate from Android's focus ducking and user volume. */
public class CubeDucking {
    static final int SHAPE_ID=17900; // Private prototype ID; never replace system duck ID 1.
    static final String CONFIG=CubeAudio.HOME+"/extra-duck-db";
    final AudioManager manager;
    final Map<Integer,Object> players=new HashMap<>();
    int db;
    CubeDucking(AudioManager manager){this.manager=manager;}
    static Object call(Object target,String name)throws Exception{return target.getClass().getMethod(name).invoke(target);}
    static VolumeShaper.Configuration shape(int db,long duration)throws Exception {
        float gain=(float)Math.pow(10,-db/20.0);
        VolumeShaper.Configuration.Builder builder=new VolumeShaper.Configuration.Builder()
            .setDuration(duration).setInterpolatorType(VolumeShaper.Configuration.INTERPOLATOR_TYPE_LINEAR)
            .setCurve(new float[]{0,0.002f,0.99f,1},new float[]{1,gain,gain,1});
        builder.getClass().getMethod("setId",int.class).invoke(builder,SHAPE_ID);
        // Wall clock, like the stock duck shape. Returns to unity even if we die.
        int clockFlag=VolumeShaper.Configuration.class.getField("OPTION_FLAG_CLOCK_TIME").getInt(null);
        builder.getClass().getMethod("setOptionFlags",int.class).invoke(builder,clockFlag);
        return builder.build();
    }
    static void apply(Object proxy,VolumeShaper.Configuration config,VolumeShaper.Operation op)throws Exception {
        proxy.getClass().getMethod("applyVolumeShaper",VolumeShaper.Configuration.class,VolumeShaper.Operation.class).invoke(proxy,config,op);
    }
    static void remove(Object proxy)throws Exception {
        VolumeShaper.Configuration id=VolumeShaper.Configuration.class.getConstructor(int.class).newInstance(SHAPE_ID);
        Object builder=Class.forName("android.media.VolumeShaper$Operation$Builder").getConstructor().newInstance();
        builder.getClass().getMethod("terminate").invoke(builder);
        apply(proxy,id,(VolumeShaper.Operation)call(builder,"build"));
    }
    static boolean media(AudioPlaybackConfiguration player) {
        int usage=player.getAudioAttributes().getUsage();
        return usage==AudioAttributes.USAGE_MEDIA||usage==AudioAttributes.USAGE_GAME;
    }
    synchronized void begin() {
        db=0;
        try(BufferedReader reader=new BufferedReader(new FileReader(CONFIG))){
            String value=reader.readLine();
            if(value==null||value.length()>2)throw new IOException("invalid extra duck dB");
            int parsed=Integer.parseInt(value.trim());
            if(parsed<0||parsed>24)throw new IOException("extra duck dB must be 0..24");
            db=parsed;
        }catch(FileNotFoundException e){}catch(Exception e){CubeAudio.log("EXTRA_DUCK_CONFIG_ERROR using 0dB");}
        CubeAudio.log("EXTRA_DUCK db="+db);
        update();
    }
    synchronized void update() {
        if(db==0)return;
        try {
            for(AudioPlaybackConfiguration player:manager.getActivePlaybackConfigurations()){
                int uid=(Integer)call(player,"getClientUid");
                if(uid==android.os.Process.myUid()||!media(player)||!(Boolean)call(player,"isActive"))continue;
                int id=(Integer)call(player,"getPlayerInterfaceId");
                if(players.containsKey(id))continue;
                Object proxy=call(player,"getPlayerProxy");if(proxy==null)continue;
                // Save before the binder call, so a partial failure still gets cleanup.
                players.put(id,proxy);
                apply(proxy,shape(db,65000),VolumeShaper.Operation.PLAY);
                CubeAudio.log("EXTRA_DUCK_APPLY player="+id+" db="+db);
            }
        }catch(Exception e){CubeAudio.log("EXTRA_DUCK_ERROR "+e);release();}
    }
    synchronized void release() {
        db=0;
        for(Map.Entry<Integer,Object> player:players.entrySet()){
            try{remove(player.getValue());CubeAudio.log("EXTRA_DUCK_RELEASE player="+player.getKey());}
            catch(Exception e){CubeAudio.log("EXTRA_DUCK_RELEASE_ERROR player="+player.getKey());}
        }
        players.clear();
    }
    // The independent crash guard runs in a fresh process; it removes only our ID.
    void recover() {
        int attempts=0;
        for(AudioPlaybackConfiguration player:manager.getActivePlaybackConfigurations()){
            try{Object proxy=call(player,"getPlayerProxy");if(proxy!=null){remove(proxy);attempts++;}}
            catch(Exception e){CubeAudio.log("EXTRA_DUCK_RECOVERY_ERROR "+e.getClass().getSimpleName());}
        }
        CubeAudio.log("EXTRA_DUCK_RECOVERY players="+attempts);
    }
    static float volume(AudioTrack track)throws Exception {
        Method get=AudioTrack.class.getDeclaredMethod("playerGetVolumeShaperState",int.class);get.setAccessible(true);
        Object state=get.invoke(track,SHAPE_ID);
        return state==null?1f:((Number)call(state,"getVolume")).floatValue();
    }
    static void selfTest(AudioManager manager)throws Exception {
        AudioTrack track=new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(16000).setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(160000).setTransferMode(AudioTrack.MODE_STATIC).build();
        Object proxy=null;
        try{
            track.write(new byte[160000],0,160000);track.play();Thread.sleep(100);
            for(AudioPlaybackConfiguration player:manager.getActivePlaybackConfigurations())
                if((Integer)call(player,"getClientPid")==android.os.Process.myPid()&&media(player))proxy=call(player,"getPlayerProxy");
            if(proxy==null)throw new IOException("test player proxy unavailable");
            apply(proxy,shape(6,1500),VolumeShaper.Operation.PLAY);Thread.sleep(250);
            float gain=volume(track);CubeAudio.log("DUCK_TEST 6dB_gain="+gain);
            if(Math.abs(gain-0.5011872f)>0.03)throw new IOException("6dB gain mismatch");
            Thread.sleep(1500);float expired=volume(track);CubeAudio.log("DUCK_TEST expired_gain="+expired);
            if(Math.abs(expired-1f)>0.01)throw new IOException("expiry failed");
            remove(proxy);
            apply(proxy,shape(12,65000),VolumeShaper.Operation.PLAY);Thread.sleep(300);
            gain=volume(track);CubeAudio.log("DUCK_TEST 12dB_gain="+gain);
            if(Math.abs(gain-0.2511886f)>0.03)throw new IOException("12dB gain mismatch");
            // Exercise the same recovery path invoked by the independent guard.
            new CubeDucking(manager).recover();Thread.sleep(100);
            float restored=volume(track);CubeAudio.log("DUCK_TEST restored_gain="+restored);
            if(Math.abs(restored-1f)>0.01)throw new IOException("guard cleanup failed");
            CubeAudio.log("DUCK_TEST PASS");
        }finally{if(proxy!=null)try{remove(proxy);}catch(Exception e){}track.stop();track.release();}
    }
}
