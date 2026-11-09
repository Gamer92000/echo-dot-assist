/* Android AudioRecord/AudioTrack backend. The helper preserves Amazon's HAL/AFE. */
#include "audio.h"
#include "cube.h"
#include "statefile.h"
#include "wake.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

static int all(int fd,void *buf,size_t n,int writing) {
    char *p=buf;
    while(n){ssize_t k=writing?send(fd,p,n,MSG_NOSIGNAL):recv(fd,p,n,0);
        if(k<0&&errno==EINTR)continue;if(k<=0)return -1;p+=k;n-=k;}
    return 0;
}
static int put(int fd,int v){uint32_t n=htonl(v);return all(fd,&n,4,1);}
static int get(int fd,int *v){uint32_t n;if(all(fd,&n,4,0))return -1;*v=(int)ntohl(n);return 0;}
static int connect_role(int role,int rate,int channels) {
    const char *name=getenv("CUBE_AUDIO_SOCKET");if(!name)name="cube-assist-audio";
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(strlen(name)>sizeof addr.sun_path-2)return -1;
    memcpy(addr.sun_path+1,name,strlen(name));
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
    struct timeval tv={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    int rc;
    if(connect(fd,(struct sockaddr*)&addr,sizeof(sa_family_t)+1+strlen(name))||put(fd,0x43554241)||put(fd,role)||put(fd,rate)||put(fd,channels)||get(fd,&rc)||rc){close(fd);return -1;}
    return fd;
}
int cube_control(int command,int value){int fd=connect_role(6,command,value),rc=-1;if(fd>=0){get(fd,&rc);close(fd);}return rc;}
int cube_indicator(const char *op,const char *pattern){
    size_t n=strlen(pattern);if(n>64)return -1;
    int fd=connect_role(7,!strcmp(op,"-s"),(int)n),rc=-1;
    if(fd>=0){if(!all(fd,(void*)pattern,n,1))get(fd,&rc);close(fd);}return rc;
}
int cube_indicator_transition(const char *from,const char *to){
    if(!from)from="";if(!to)to="";size_t a=strlen(from),b=strlen(to);
    if(a>64||b>64)return -1;int fd=connect_role(8,(int)a,(int)b),rc=-1;
    if(fd>=0){if(!all(fd,(void*)from,a,1)&&!all(fd,(void*)to,b,1))get(fd,&rc);close(fd);}return rc;
}
static int capfd=-1;
static unsigned char capture[4096];
int cap_open(void){capfd=connect_role(0,16000,1);return capfd<0?-1:0;}
int cap_read(const void **data){int n;static int blocks;if(get(capfd,&n)||n<=0||n>(int)sizeof capture||n%2||all(capfd,capture,n,0))return -1;
    if(++blocks>=31){blocks=0;int music=cube_control(4,0);if(music>=0)wake_property("AudioPlayerState",music);}
    *data=capture;return n;}
void cap_close(void){if(capfd>=0)close(capfd);capfd=-1;}
static atomic_int main_volume=40,tts_volume=40,music_q=65536,tts_q=65536,call_q=65536;
static int q(float g){return (int)lroundf(fmaxf(0,fminf(1,g))*65536);}
void audio_music_gain(float g){atomic_store(&music_q,q(g));}
void audio_tts_gain(float g){atomic_store(&tts_q,q(g));}
void audio_call_gain(float g){atomic_store(&call_q,q(g));}
struct stream {int fd;int role;atomic_int *gain;};
static struct stream tts={-1,1,&tts_q},music={-1,2,&music_q},bt={-1,4,&music_q},voip={-1,5,&call_q};
static int open_stream(struct stream *s,unsigned rate,unsigned ch){s->fd=connect_role(s->role,rate,ch);return s->fd<0?-1:0;}
static int write_stream(struct stream *s,const void *pcm,size_t len) {
    const unsigned char *p=pcm;if(s->fd<0||len%2)return -1;
    while(len){size_t n=len>4096?4096:len;short buffer[2048];
        int level=atomic_load(s->role==1?&tts_volume:&main_volume);
        int gain=atomic_load(s->gain)*level/100;
        for(size_t i=0;i<n/2;i++){short v;memcpy(&v,p+2*i,2);buffer[i]=(short)((v*gain+32768)>>16);}
        int rc;if(put(s->fd,1)||put(s->fd,n)||all(s->fd,buffer,n,1)||get(s->fd,&rc)||rc!=(int)n)return -1;
        p+=n;len-=n;
    }return 0;
}
static long long queued(struct stream *s){int hi,lo;if(s->fd<0||put(s->fd,2)||get(s->fd,&hi)||get(s->fd,&lo))return 0;return ((long long)(uint32_t)hi<<32)|(uint32_t)lo;}
static void close_stream(struct stream *s,int drain){if(s->fd<0)return;int rc;if(!put(s->fd,3)&&!put(s->fd,drain))get(s->fd,&rc);close(s->fd);s->fd=-1;}
int play_open(unsigned r,unsigned c){return open_stream(&tts,r,c);}
int play_write(const void *d,size_t n){return write_stream(&tts,d,n);}
void play_close(int drain){close_stream(&tts,drain);}
#define STREAM_API(name) \
int name##_open(unsigned r,unsigned c){return open_stream(&name,r,c);} \
int name##_write(const void*d,size_t n){return write_stream(&name,d,n);} \
long long name##_queued_us(void){return queued(&name);} \
void name##_close(void){close_stream(&name,0);}
STREAM_API(music) STREAM_API(bt) STREAM_API(voip)
/* Direct AudioTrack playback-head accounting; no OpenSL prefill offset. */
long long music_prime_us(void) { return 0; }
void (*audio_earcon_started)(unsigned lead_ms);
static int always(void){return 1;}
void play_earcon(const short *p,size_t n,unsigned r){play_earcon_while(p,n,r,always);}
void play_earcon_while(const short *p,size_t n,unsigned r,int(*go)(void)){
    atomic_int gain=65536;struct stream ear={-1,3,&gain};if(open_stream(&ear,r,1))return;
    if(audio_earcon_started)audio_earcon_started(0);
    while(n&&go()){size_t k=n>512?512:n;if(write_stream(&ear,p,k*2))break;p+=k;n-=k;}
    close_stream(&ear,n==0);
}
/* Fire TV retains its Bluetooth stack. This port only routes to the built-in speaker. */
int audio_a2dp_route(int on, uint64_t addr, const char *name) {
    (void)addr; (void)name; return on ? -1 : 0;
}
int audio_a2dp_lost(uint64_t addr) { (void)addr; return 1; }
void audio_a2dp_reset(void) {}
int audio_a2dp_aipc(void) { return 0; }

/* The new common audio API replaces Cube-specific volume hooks in main.c. */
int vol_read(enum vol which, int fallback) {
    switch (which) {
    case VOL_MAIN: return atomic_load(&main_volume);
    case VOL_TTS: return atomic_load(&tts_volume);
    case VOL_MUTE: return 0;
    default: return fallback;
    }
}
void vol_write(enum vol which, int v) {
    if (v < 0 || v > 100) return;
    if (which == VOL_MAIN) atomic_store(&main_volume, v);
    if (which == VOL_TTS) atomic_store(&tts_volume, v);
}
void afe_listening(int on) {
    if (cube_control(2, on) < 0) fprintf(stderr, "cube: listening state update failed\n");
}
/* The helper does not expose Echo AFE energy telemetry or equalizer controls. */
int afe_arbitration(long ts, long te, char *json, size_t n) {
    (void)ts; (void)te; if (n) json[0] = 0; return 0;
}
void afe_stream_stopped(void) {}
int afe_eq_get(char *json, size_t n) { if (n) json[0] = 0; return 0; }
void afe_eq_set(const char *json) { (void)json; }

int cube_extra_duck(int value){
    const char *home=getenv("CUBE_ASSIST_HOME");if(!home)home="/data/local/cube-assist";
    char path[512],data[8];snprintf(path,sizeof path,"%s/extra-duck-db",home);
    if(value>=0&&value<=24){int n=snprintf(data,sizeof data,"%d\n",value);if(statefile_write(path,0600,data,n))return -1;}
    int v=0;FILE*f=fopen(path,"r");if(f){if(fscanf(f,"%d",&v)!=1||v<0||v>24)v=0;fclose(f);}return v;
}

/* Configuration is shared with the Java helper through atomic files, not slow IPC under core_lock. */
const char *const cube_route_names[CUBE_ROUTE_SETTINGS]={"audio_output","cube_output_volume","tv_output_volume","music_duck_percent"};
const char *const cube_output_modes[3]={"Auto","Cube speaker","TV"};
int cube_route_setting(int field,int value){
    if(field<0||field>=CUBE_ROUTE_SETTINGS)return -1;
    const char *home=getenv("HASSMIC_STATE");if(!home)home="/data/local/cube-assist/state";
    char path[512],data[16];snprintf(path,sizeof path,"%s/%s",home,cube_route_names[field]);
    int max=field==CUBE_OUTPUT_MODE?2:100;
    if(value>=0){if(value>max)return -1;int n=snprintf(data,sizeof data,"%d\n",value);if(statefile_write(path,0600,data,n))return -1;}
    int fallback=field==CUBE_OUTPUT_MODE?0:field==CUBE_MUSIC_DUCK?17:100,v=fallback;
    FILE*f=fopen(path,"r");if(f){if(fscanf(f,"%d",&v)!=1||v<0||v>max)v=fallback;fclose(f);}return v;
}
void cube_route_text(int field,char *out,unsigned cap){
    const char *home=getenv("CUBE_ASSIST_HOME");if(!home)home="/data/local/cube-assist";
    char path[512];const char *runtime=getenv("CUBE_ROUTING_STATUS");
    if(runtime)snprintf(path,sizeof path,"%s",runtime);else snprintf(path,sizeof path,"%s/routing-status",home);
    long long stamp=0;int state=0,active=0,selected=1,reason=4,source=0,valid=0;
    FILE*f=fopen(path,"r");if(f){valid=fscanf(f,"%lld %d %d %d %d %d",&stamp,&state,&active,&selected,&reason,&source)==6;fclose(f);}
    struct timespec t;clock_gettime(CLOCK_BOOTTIME,&t);long long now=(long long)t.tv_sec*1000+t.tv_nsec/1000000;
    int fresh=valid&&stamp>0&&now>=stamp&&now-stamp<=5000;
    const char *states[]={"Unknown","On","Off"};
    const char *actual[]={"Idle","Cube speaker","TV","Switching / unconfirmed","Mixed outputs"};
    const char *reasons[]={"None","TV state unknown; using Cube","HDMI unavailable; using Cube","HDMI routing failed; using Cube","TV detection stale; Auto uses Cube"};
    const char *sources[]={"No reliable evidence","Fresh CEC power report","Verified HDMI receiver sense","HDMI disconnected"};
    const char *value="Unknown";
    if(field==0)value=fresh&&state>=0&&state<3?states[state]:"Unknown";
    else if(field==1)value=fresh&&active>=0&&active<5?actual[active]:"Unconfirmed (status stale)";
    else if(field==2)value=fresh&&reason>=0&&reason<5?reasons[reason]:reasons[4];
    else if(field==3)value=fresh&&source>=0&&source<4?sources[source]:"Stale";
    else if(field==4)value=fresh?(selected==2?"TV":"Cube speaker"):"Unknown";
    snprintf(out,cap,"%s",value);
}
