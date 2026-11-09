import android.content.Context;
import android.media.*;
import android.os.*;
import java.lang.reflect.*;
import java.nio.file.*;

/** Silent hardware check of the real routing adapter, with an isolated config directory. */
public class CubeRoutingProbe {
 static void setting(String key,int value)throws Exception{Files.write(Paths.get(CubeAudio.HOME+"/state/"+key),(value+"\n").getBytes("UTF-8"));}
 static AudioTrack track(boolean stereo){return new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ASSISTANCE_ACCESSIBILITY).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build()).setAudioFormat(new AudioFormat.Builder().setSampleRate(48000).setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(stereo?AudioFormat.CHANNEL_OUT_STEREO:AudioFormat.CHANNEL_OUT_MONO).build()).setBufferSizeInBytes(8192).setTransferMode(AudioTrack.MODE_STREAM).build();}
 static void phase(CubeRouting routing,AudioTrack a,AudioTrack b,int mode,int expected,String label)throws Exception{
  setting("audio_output",mode);routing.tick();
  byte[] silence=new byte[1920];
  for(int i=0;i<30;i++){routing.beforeWrite(a,2);a.write(silence,0,silence.length);routing.beforeWrite(b,1);b.write(silence,0,silence.length);Thread.sleep(5);}
  routing.tick();
  for(AudioTrack t:new AudioTrack[]{a,b}){
   AudioDeviceInfo actual=t.getRoutedDevice();int type=actual==null?-1:actual.getType();
   System.out.println(label+" route="+type+" session="+t.getAudioSessionId()+" head="+Integer.toUnsignedLong(t.getPlaybackHeadPosition()));
   if(type!=expected)throw new AssertionError(label+" route "+type);
  }
 }
 public static void main(String[] args)throws Exception{
  if(!CubeAudio.HOME.equals("/data/local/tmp/cube-routing-diagnostics-20261029/adapter-test"))throw new SecurityException("isolated test directory required");
  Looper.prepareMainLooper();Class<?> at=Class.forName("android.app.ActivityThread");Object th=at.getDeclaredMethod("systemMain").invoke(null);Context ctx=(Context)at.getDeclaredMethod("getSystemContext").invoke(th);
  CubeRouting routing=new CubeRouting((AudioManager)ctx.getSystemService(Context.AUDIO_SERVICE),ctx);
  AudioTrack music=track(true),speech=track(false);int result=0;
  try{
   routing.add(music,2);routing.add(speech,1);music.play();speech.play();
   int ms=music.getAudioSessionId(),ss=speech.getAudioSessionId();
   phase(routing,music,speech,0,9,"auto_tv");long head=Integer.toUnsignedLong(music.getPlaybackHeadPosition());
   phase(routing,music,speech,1,2,"forced_cube");
   CubeAudio.focusHeld=true;setting("music_duck_percent",25);routing.tick();routing.gains();
   phase(routing,music,speech,2,9,"forced_tv_ducked");
   CubeAudio.focusHeld=false;routing.gains();phase(routing,music,speech,0,9,"auto_tv_restored");
   if(ms!=music.getAudioSessionId()||ss!=speech.getAudioSessionId()||Integer.toUnsignedLong(music.getPlaybackHeadPosition())<=head)throw new AssertionError("session restarted");
   // A stalled detector must move ongoing Auto playback to the Cube as PCM continues.
   routing.freshAt=SystemClock.elapsedRealtime()-6000;
   byte[] silence=new byte[1920];
   for(int i=0;i<30;i++){for(AudioTrack t:new AudioTrack[]{music,speech}){routing.beforeWrite(t,t==music?2:1);t.write(silence,0,silence.length);}Thread.sleep(5);}
   if(music.getRoutedDevice().getType()!=2||speech.getRoutedDevice().getType()!=2)throw new AssertionError("stale fallback");
   System.out.println("stale_auto_fallback both routes=2");
   phase(routing,music,speech,0,9,"fresh_auto_recovered");
   // Invalidating the tested sink profile forces an honest Unknown + Cube fallback.
   Files.write(Paths.get(CubeAudio.HOME+"/state/tv_receiver_profile"),"invalid".getBytes("UTF-8"));
   phase(routing,music,speech,0,2,"unknown_fallback");
   if(routing.state!=0||routing.reason!=1)throw new AssertionError("fallback status");
   System.out.println("PASS: two live tracks switch together without resetting sessions, verified Auto and unknown fallback");
  }catch(Throwable e){e.printStackTrace();result=1;}
  finally{CubeAudio.focusHeld=false;for(AudioTrack t:new AudioTrack[]{music,speech}){routing.remove(t);t.stop();t.release();}routing.tick();System.out.println("released_tracks="+routing.tracks.size()+" active="+routing.active);System.out.flush();}
  Runtime.getRuntime().halt(result);
 }
}
