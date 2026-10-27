import android.content.Context;
import android.media.*;
import android.net.*;
import android.os.*;
import java.io.*;
import java.lang.reflect.Method;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** Private local audio adapter. No network listener, package install or firmware changes. */
public class CubeAudio {
    static final String HOME=System.getenv("CUBE_ASSIST_HOME")==null?"/data/local/cube-assist":System.getenv("CUBE_ASSIST_HOME");
    static AudioManager manager;
    static CubeIndicators indicators;
    static Context context;
    static Object asp;static Method aspCommand,isStreamActive;
    static CubeDucking duck;
    static AudioFocusRequest focus;static HandlerThread focusThread;
    static volatile boolean focusHeld,suppressed,running=true;
    static volatile long busySince;
    static String originalWake;
    static final AtomicBoolean capturing=new AtomicBoolean();
    static final AtomicInteger ttsTracks=new AtomicInteger(),musicTracks=new AtomicInteger();
    static final ThreadPoolExecutor workers=new ThreadPoolExecutor(12,12,0,TimeUnit.SECONDS,new ArrayBlockingQueue<Runnable>(32));
    static void log(String s){System.err.println(System.currentTimeMillis()+" cube-audio: "+s);}
    static void initialize()throws Exception{
        Looper.prepareMainLooper();Class<?> at=Class.forName("android.app.ActivityThread");
        Object thread=at.getDeclaredMethod("systemMain").invoke(null);
        context=(Context)at.getDeclaredMethod("getSystemContext").invoke(thread);
        manager=(AudioManager)context.getSystemService(Context.AUDIO_SERVICE);duck=new CubeDucking(manager);
        indicators=new CubeIndicators(context);
        Class<?> ac=Class.forName("com.amazon.asp.AudioSignalProcessor");asp=ac.getDeclaredMethod("getInstance").invoke(null);
        aspCommand=ac.getDeclaredMethod("command",int.class,byte[].class,byte[].class);
        isStreamActive=Class.forName("android.media.AudioSystem").getMethod("isStreamActive",int.class,int.class);
        try(BufferedReader r=new BufferedReader(new FileReader(HOME+"/original-wake-setting"))){originalWake=r.readLine();}
        if(!"0".equals(originalWake)&&!"1".equals(originalWake)&&!"null".equals(originalWake))throw new IOException("original wake setting missing or invalid");
    }
    static synchronized void flag(int command,boolean value)throws Exception{
        int rc=(Integer)aspCommand.invoke(asp,command,new byte[]{(byte)(value?1:0),0,0,0},new byte[0]);
        if(rc!=0)throw new IOException("ASP "+command+" rc="+rc);
    }
    static boolean music()throws Exception{return musicTracks.get()>0||(Boolean)isStreamActive.invoke(null,3,0);}
    static String settings(String... args)throws Exception{
        String[] command=new String[args.length+1];command[0]="/system/bin/settings";System.arraycopy(args,0,command,1,args.length);
        java.lang.Process p=new ProcessBuilder(command).redirectErrorStream(true).start();
        if(!p.waitFor(4,TimeUnit.SECONDS)){p.destroyForcibly();throw new IOException("settings timeout");}
        ByteArrayOutputStream b=new ByteArrayOutputStream();byte[] buf=new byte[256];int n;
        while((n=p.getInputStream().read(buf))>0){if(b.size()+n>4096)throw new IOException("settings output limit");b.write(buf,0,n);}
        if(p.exitValue()!=0)throw new IOException("settings failed");return b.toString("UTF-8").trim();
    }
    static synchronized void suppression(boolean enabled)throws Exception{
        if(enabled==suppressed)return;
        String want=enabled?"1":originalWake;
        if(want.equals("null"))settings("delete","secure","alexa_wake_word_disabled");else settings("put","secure","alexa_wake_word_disabled",want);
        if(!settings("get","secure","alexa_wake_word_disabled").equals(want))throw new IOException("wake setting failed verification");
        suppressed=enabled;log("stock_wake_disabled="+want);
    }
    static synchronized void acquire()throws Exception{
        if(focusHeld)return;
        if(focus==null){focusThread=new HandlerThread("cube-focus");focusThread.start();
            focus=new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK)
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ASSISTANT).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                .setOnAudioFocusChangeListener(change->{if(change<0)release();},new Handler(focusThread.getLooper())).build();}
        if(manager.requestAudioFocus(focus)!=AudioManager.AUDIOFOCUS_REQUEST_GRANTED)throw new IOException("focus denied");
        focusHeld=true;busySince=SystemClock.elapsedRealtime();duck.begin();
    }
    static synchronized void release(){duck.release();if(focusHeld){focusHeld=false;manager.abandonAudioFocusRequest(focus);}}
    static synchronized void restore(){
        if(indicators!=null)indicators.clear();
        release();try{flag(146,false);flag(1,false);flag(13,music());}catch(Exception e){log("AFE restore error "+e);}
        try{suppression(false);}catch(Exception e){log("wake restore error "+e);}
    }
    static int control(int command,int value)throws Exception{
        switch(command){
            case 1: if(value==0)release();else acquire();return 0; // EDA pipeline state
            case 2: flag(146,value!=0);return 0;
            case 3: suppression(value!=0);if(value==0)restore();return 0;
            case 4: return music()?1:0;
            default: throw new IOException("unknown control");
        }
    }
    static void capture(DataOutputStream out)throws Exception{
        if(!capturing.compareAndSet(false,true))throw new IOException("capture already active");
        AudioRecord record=null;
        try{
            record=new AudioRecord(6,16000,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT,8192);
            if(record.getState()!=AudioRecord.STATE_INITIALIZED)throw new IOException("capture init");
            boolean chosen=false;for(AudioDeviceInfo d:manager.getDevices(AudioManager.GET_DEVICES_INPUTS))if(d.getType()==AudioDeviceInfo.TYPE_BUILTIN_MIC){chosen=record.setPreferredDevice(d);break;}
            if(!chosen)throw new IOException("built-in mic absent");record.startRecording();out.writeInt(0);out.flush();
            byte[] samples=new byte[1024];log("capture running");
            while(running){int n=record.read(samples,0,samples.length);if(n<=0)throw new IOException("capture read="+n);out.writeInt(n);out.write(samples,0,n);out.flush();}
        }finally{if(record!=null){if(record.getRecordingState()==AudioRecord.RECORDSTATE_RECORDING)record.stop();record.release();}capturing.set(false);restore();log("capture closed");}
    }
    static void playback(int role,int rate,int channels,DataInputStream in,DataOutputStream out)throws Exception{
        if(role<1||role>5||rate<8000||rate>96000||(channels!=1&&channels!=2))throw new IOException("invalid playback format");
        int mask=channels==1?AudioFormat.CHANNEL_OUT_MONO:AudioFormat.CHANNEL_OUT_STEREO;
        AudioTrack track=new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY).setContentType(role==2?AudioAttributes.CONTENT_TYPE_MUSIC:AudioAttributes.CONTENT_TYPE_SPEECH).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(rate).setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(mask).build())
            .setBufferSizeInBytes(Math.max(8192,AudioTrack.getMinBufferSize(rate,mask,AudioFormat.ENCODING_PCM_16BIT)))
            .setTransferMode(AudioTrack.MODE_STREAM).build();
        boolean speech=role==1||role==3||role==5,counted=false;long written=0,lastHead=0,wrap=0;
        try{
            boolean chosen=false;for(AudioDeviceInfo d:manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS))if(d.getType()==AudioDeviceInfo.TYPE_BUILTIN_SPEAKER){chosen=track.setPreferredDevice(d);break;}
            if(!chosen)throw new IOException("built-in speaker absent");
            if(speech){if(ttsTracks.incrementAndGet()==1)flag(1,true);}else musicTracks.incrementAndGet();counted=true;
            track.setVolume(1f);track.play();out.writeInt(0);out.flush();
            byte[] data=new byte[16384];
            while(running){
                int op=in.readInt();long head=Integer.toUnsignedLong(track.getPlaybackHeadPosition());if(head<lastHead)wrap+=1L<<32;lastHead=head;head+=wrap;
                if(op==1){int n=in.readInt();if(n<0||n>data.length||n%(channels*2)!=0)throw new IOException("invalid audio block");in.readFully(data,0,n);
                    int at=0;while(at<n){int k=track.write(data,at,n-at);if(k<=0)throw new IOException("playback write="+k);at+=k;}written+=n/(channels*2);out.writeInt(n);
                }else if(op==2){out.writeLong(Math.max(0,written-head)*1000000/rate);
                }else if(op==3){boolean drain=in.readInt()!=0;long deadline=SystemClock.elapsedRealtime()+3000;
                    while(drain&&head<written&&SystemClock.elapsedRealtime()<deadline){Thread.sleep(10);long h=Integer.toUnsignedLong(track.getPlaybackHeadPosition());if(h<lastHead)wrap+=1L<<32;lastHead=h;head=wrap+h;}
                    AudioDeviceInfo route=track.getRoutedDevice();log("playback role="+role+" route="+(route==null?-1:route.getType()));out.writeInt(0);out.flush();break;
                }else throw new IOException("unknown audio operation");out.flush();
            }
        }finally{
            try{track.stop();}finally{track.release();}
            if(counted){if(speech){if(ttsTracks.decrementAndGet()==0)flag(1,false);}else musicTracks.decrementAndGet();}
        }
    }
    static void serve(LocalSocket socket){
        try(LocalSocket s=socket){
            if(s.getPeerCredentials().getUid()!=0)throw new IOException("root client required");s.setSoTimeout(5000);
            DataInputStream in=new DataInputStream(s.getInputStream());DataOutputStream out=new DataOutputStream(s.getOutputStream());
            if(in.readInt()!=0x43554241)throw new IOException("bad IPC version");int role=in.readInt(),a=in.readInt(),b=in.readInt();
            if(role==0)capture(out);
            else if(role==6){out.writeInt(0);out.writeInt(control(a,b));out.flush();}
            else if(role==7){
                if(b<1||b>64||(a!=0&&a!=1))throw new IOException("invalid indicator");
                out.writeInt(0);out.flush();byte[] name=new byte[b];in.readFully(name);
                indicators.set(new String(name,"US-ASCII"),a!=0);out.writeInt(0);out.flush();
            }
            else if(role==8){
                if(a<0||a>64||b<1||b>64)throw new IOException("invalid indicator transition");
                out.writeInt(0);out.flush();byte[] from=new byte[a],to=new byte[b];in.readFully(from);in.readFully(to);
                indicators.transition(new String(from,"US-ASCII"),new String(to,"US-ASCII"));out.writeInt(0);out.flush();
            }
            else playback(role,a,b,in,out);
        }catch(Exception e){log("client closed: "+e);}
    }
    public static void main(String[] args)throws Exception{
        initialize();
        if(args.length==1&&args[0].equals("reset")){duck.recover();suppressed=true;restore();System.exit(0);}
        new CubeDiscovery(context);
        Runtime.getRuntime().addShutdownHook(new Thread(()->{running=false;restore();}));
        new Thread(()->{Boolean previous=null;while(running){try{Thread.sleep(1000);
            if(suppressed){boolean playing=music();if(previous==null||playing!=previous){flag(13,playing);previous=playing;}}
            else previous=null;
            if(focusHeld){duck.update();if(SystemClock.elapsedRealtime()-busySince>65000){flag(146,false);release();log("focus deadline");}}
        }catch(Exception e){log("monitor error: "+e);}}},"cube-audio-monitor").start();
        String name=System.getenv("CUBE_AUDIO_SOCKET");if(name==null)name="cube-assist-audio";
        final String socketName=name;
        new Thread(()->{try(LocalServerSocket listener=new LocalServerSocket(socketName)){
            log("ready local socket="+socketName);
            while(running){LocalSocket s=listener.accept();try{workers.execute(()->serve(s));}catch(RejectedExecutionException e){s.close();}}
        }catch(Exception e){log("listener failed: "+e);}finally{restore();System.exit(1);}},"cube-local-ipc").start();
        Looper.loop();
    }
}
