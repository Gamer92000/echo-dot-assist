import android.content.Context;
import android.graphics.*;
import android.os.*;
import android.view.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.TimeUnit;

/** Stock Cube animations on the LEDs, with a non-interactive TV bar using the same frames.
 * Never clears the LED controller globally or replaces Amazon's animation files. */
final class CubeIndicators {
    private final Handler ui=new Handler(Looper.getMainLooper());
    private final WindowManager windows;
    private final Bar bar;
    private final Map<String,Animation> active=new LinkedHashMap<>();
    private final Set<String> owned=new LinkedHashSet<>();
    private final File journal=new File(CubeAudio.HOME,"indicator-owned");
    private boolean attached;
    private Runnable pendingHide;
    private boolean transition;
    private static final Map<String,String> ALIASES=new HashMap<>();
    static {
        ALIASES.put("active-start","active-listening");
        ALIASES.put("mics-off_on","stark_privacy");
        ALIASES.put("authenticated_setup_mode","scone-setup");
        ALIASES.put("zzz_rainbow","setup-mode");
    }
    CubeIndicators(Context c){windows=(WindowManager)c.getSystemService(Context.WINDOW_SERVICE);bar=new Bar(c);
        // A previous helper may have been killed. Only its recorded patterns are ours to unset.
        try(BufferedReader r=new BufferedReader(new FileReader(journal))){String s;while((s=r.readLine())!=null)if(valid(s))owned.add(s);}catch(IOException ignored){}
        clear();
    }
    private static boolean valid(String p){return p.matches("[A-Za-z0-9_-]{1,64}")&&new File("/system/etc/led-resources/"+p+".animation").isFile();}
    private static String led(String...args)throws Exception{
        String[] command=new String[args.length+1];command[0]="/system/bin/ledctrl";System.arraycopy(args,0,command,1,args.length);
        java.lang.Process p=new ProcessBuilder(command).redirectErrorStream(true).start();
        if(!p.waitFor(2,TimeUnit.SECONDS)){p.destroyForcibly();throw new IOException("LED controller timeout");}
        ByteArrayOutputStream b=new ByteArrayOutputStream();byte[] buf=new byte[256];int n;
        while((n=p.getInputStream().read(buf))>0){if(b.size()+n>8192)throw new IOException("LED output limit");b.write(buf,0,n);}
        if(p.exitValue()!=0)throw new IOException("LED controller failed");return b.toString("UTF-8");
    }
    private void save()throws IOException{
        File tmp=new File(journal.getPath()+".new");
        try(FileOutputStream out=new FileOutputStream(tmp)){for(String p:owned)out.write((p+"\n").getBytes("US-ASCII"));out.getFD().sync();}
        if(!tmp.renameTo(journal))throw new IOException("LED ownership journal");
    }
    synchronized void set(String requested,boolean on)throws Exception{
        String p=ALIASES.containsKey(requested)?ALIASES.get(requested):requested;
        if(!valid(p)){CubeAudio.log("indicator unavailable: "+requested);return;}
        if(on){
            Animation a=Animation.load(p);active.put(p,a);
            if(!owned.contains(p)){
                String current=led("-g");boolean already=false;
                for(String token:current.split("[^A-Za-z0-9_-]+"))if(token.equals(p))already=true;
                if(!already){owned.add(p);save();led("-s",p);}
            }
            long expiry=oneShot(p)?Math.max(800,Math.min(3000,a.duration)):0;
            if(expiry>0)ui.postDelayed(()->{synchronized(CubeIndicators.this){if(active.get(p)==a)try{set(p,false);}catch(Exception e){CubeAudio.log("indicator expiry: "+e);}}},expiry);
        }else{
            active.remove(p);
            if(owned.contains(p)){led("-u",p);owned.remove(p);save();}
        }
        publish();
    }
    private static boolean oneShot(String p){return p.equals("active-end")||p.equals("mics-off_end")||p.equals("anim_start_error_short")||p.equals("ready-timer-short")||p.equals("call_connected_end");}
    private static int priority(String p){
        if(p.equals("stark_privacy"))return 100;
        if(p.startsWith("volume_"))return 90;
        if(p.equals("anim_start_error_short"))return 85;
        if(p.startsWith("active-"))return 80;
        if(p.equals("ready-timer")||p.equals("ready-timer-short"))return 70;
        return 10;
    }
    synchronized void transition(String from,String to)throws Exception{
        transition=true;
        try{if(!from.isEmpty())set(from,false);set(to,true);}
        finally{transition=false;publish();}
    }
    private void publish(){if(!transition)publish(false);}
    private void hide(){
        bar.change(null);
        if(attached){windows.removeViewImmediate(bar);attached=false;CubeAudio.log("TV status bar removed");}
    }
    private void publish(boolean immediate){
        Animation best=null;int score=-1;
        for(Map.Entry<String,Animation> e:active.entrySet())if(priority(e.getKey())>=score){score=priority(e.getKey());best=e.getValue();}
        final Animation chosen=best;
        ui.post(()->{
            try{
                if(pendingHide!=null){ui.removeCallbacks(pendingHide);pendingHide=null;}
                // The core unsets the old pattern before setting the next one. Keep the
                // same surface through that short gap instead of flashing the video.
                if(chosen==null){
                    if(immediate)hide();
                    else {pendingHide=()->{pendingHide=null;try{hide();}catch(RuntimeException e){CubeAudio.log("TV status bar remove: "+e);}};ui.postDelayed(pendingHide,250);}
                    return;
                }
                bar.change(chosen);
                if(!attached){
                    WindowManager.LayoutParams lp=new WindowManager.LayoutParams(WindowManager.LayoutParams.MATCH_PARENT,14,
                        WindowManager.LayoutParams.TYPE_SYSTEM_OVERLAY,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE|WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE|WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
                    lp.gravity=Gravity.BOTTOM;lp.setTitle("Cube Assist status bar");
                    windows.addView(bar,lp);attached=true;CubeAudio.log("TV status bar attached");
                }
                bar.invalidate();
            }catch(RuntimeException e){CubeAudio.log("TV status bar unavailable: "+e);}
        });
    }
    synchronized void clear(){
        active.clear();
        for(String p:new ArrayList<>(owned))try{led("-u",p);owned.remove(p);}catch(Exception e){CubeAudio.log("LED restore: "+e);}
        try{save();}catch(IOException e){CubeAudio.log("LED journal: "+e);}publish(true);
    }
    static final class Animation {
        final List<int[]> colors=new ArrayList<>();final List<Integer> times=new ArrayList<>();
        int loop=-1;long duration;final long start=SystemClock.uptimeMillis();
        static Animation load(String name)throws IOException{
            Animation a=new Animation();
            try(BufferedReader r=new BufferedReader(new FileReader("/system/etc/led-resources/"+name+".animation"))){
                String line;while((line=r.readLine())!=null){
                    if(a.colors.size()>10000)throw new IOException("animation too long");
                    line=line.trim();if(line.equals("loop")){a.loop=a.colors.size();continue;}
                    String[] halves=line.split(":",2);if(halves.length!=2)continue;
                    try{
                        int ms=Integer.parseInt(halves[0]);if(ms<1||ms>60000)continue;
                        String[] rgb=halves[1].split(",");if(rgb.length<5)continue;
                        int[] c=new int[5];for(int i=0;i<5;i++){
                            String hex=rgb[i].trim();if(hex.length()==3)hex=""+hex.charAt(0)+hex.charAt(0)+hex.charAt(1)+hex.charAt(1)+hex.charAt(2)+hex.charAt(2);
                            c[i]=0xff000000|Integer.parseInt(hex,16);
                        }
                        a.colors.add(c);a.times.add(ms);a.duration+=ms;
                    }catch(NumberFormatException ignored){}
                }
            }
            if(a.colors.isEmpty())throw new IOException("empty animation: "+name);return a;
        }
        int[] frame(){
            long t=SystemClock.uptimeMillis()-start;
            if(t>=duration){
                if(loop<0||loop>=times.size())return colors.get(colors.size()-1);
                long intro=0;for(int i=0;i<loop;i++)intro+=times.get(i);t=intro+(t-intro)%(duration-intro);
            }
            for(int i=0;i<times.size();i++){if(t<times.get(i))return colors.get(i);t-=times.get(i);}return colors.get(colors.size()-1);
        }
    }
    static final class Bar extends View {
        Animation animation;final Paint paint=new Paint();
        int[] previous;long changedAt;
        Bar(Context c){super(c);setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);}
        void change(Animation next){
            if(next==animation)return;
            previous=animation==null||next==null?null:frame();
            animation=next;changedAt=SystemClock.uptimeMillis();
        }
        int[] frame(){
            int[] current=animation.frame();
            long elapsed=SystemClock.uptimeMillis()-changedAt;
            if(previous==null||elapsed>=120){previous=null;return current;}
            int[] blended=new int[current.length];float f=elapsed/120f;
            for(int i=0;i<current.length;i++)blended[i]=Color.rgb(
                Math.round(Color.red(previous[i])*(1-f)+Color.red(current[i])*f),
                Math.round(Color.green(previous[i])*(1-f)+Color.green(current[i])*f),
                Math.round(Color.blue(previous[i])*(1-f)+Color.blue(current[i])*f));
            return blended;
        }
        protected void onDraw(Canvas canvas){
            if(animation==null)return;int[] colors=frame();
            paint.setShader(new LinearGradient(0,0,getWidth(),0,colors,null,Shader.TileMode.CLAMP));
            canvas.drawRect(0,0,getWidth(),getHeight(),paint);postInvalidateDelayed(32);
        }
    }
}
