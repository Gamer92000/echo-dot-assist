import android.content.Context;
import android.net.nsd.*;
import android.os.*;
import android.util.Xml;
import org.xmlpull.v1.XmlPullParser;
import java.io.*;
import java.util.*;

/** Advertise the core's own service records through Android's existing mDNS daemon. */
final class CubeDiscovery {
    final NsdManager nsd;final Handler handler=new Handler(Looper.getMainLooper());
    final ArrayList<NsdManager.RegistrationListener> registered=new ArrayList<>();
    long stamp=-1;
    CubeDiscovery(Context c){nsd=(NsdManager)c.getSystemService(Context.NSD_SERVICE);handler.post(this::tick);}
    void tick(){
        try{
            File file=new File(CubeAudio.HOME,"services.xml");long now=file.lastModified();
            if(now>0&&now!=stamp){
                ArrayList<NsdServiceInfo> records=new ArrayList<>();
                try(FileInputStream in=new FileInputStream(file)){
                    XmlPullParser p=Xml.newPullParser();p.setInput(in,"UTF-8");NsdServiceInfo record=null;String name="Cube Assist";
                    while(p.next()!=XmlPullParser.END_DOCUMENT){
                        if(p.getEventType()==XmlPullParser.START_TAG){String tag=p.getName();
                            if(tag.equals("name"))name=p.nextText();
                            else if(tag.equals("service")){record=new NsdServiceInfo();record.setServiceName(name);}
                            else if(record!=null&&tag.equals("type"))record.setServiceType(p.nextText());
                            else if(record!=null&&tag.equals("port"))record.setPort(Integer.parseInt(p.nextText()));
                            else if(record!=null&&tag.equals("txt-record")){String t=p.nextText();int k=t.indexOf('=');record.setAttribute(k<0?t:t.substring(0,k),k<0?"":t.substring(k+1));}
                        }else if(p.getEventType()==XmlPullParser.END_TAG&&p.getName().equals("service")&&record!=null){records.add(record);record=null;}
                    }
                }
                for(NsdManager.RegistrationListener l:registered)try{nsd.unregisterService(l);}catch(RuntimeException ignored){}
                registered.clear();
                for(NsdServiceInfo record:records){
                    NsdManager.RegistrationListener listener=new NsdManager.RegistrationListener(){
                        public void onServiceRegistered(NsdServiceInfo info){CubeAudio.log("discovery: "+record.getServiceType()+" port="+record.getPort());}
                        public void onRegistrationFailed(NsdServiceInfo info,int code){CubeAudio.log("discovery registration failed: "+code);stamp=-1;}
                        public void onServiceUnregistered(NsdServiceInfo info){}
                        public void onUnregistrationFailed(NsdServiceInfo info,int code){CubeAudio.log("discovery unregister failed: "+code);}
                    };
                    nsd.registerService(record,NsdManager.PROTOCOL_DNS_SD,listener);registered.add(listener);
                }
                stamp=now;
            }
        }catch(Exception e){CubeAudio.log("discovery unavailable: "+e);}
        if(CubeAudio.running)handler.postDelayed(this::tick,10000);
    }
}
