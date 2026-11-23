/* EchoRoot's ledd accepts one named animation. Never grab the input devices or
 * periodically reassert a pattern: buttond retains ownership of the volume UI. */
#include "ring.h"
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <time.h>
#include <errno.h>
#include <sys/prctl.h>
const char *const ring_effects[]={"None","Blue","Cyan","Green","Orange","Red","White","Rainbow","Comets","Magenta pulse"};
const int ring_neffects=10;
const struct ring_slot ring_slots[]={{"listening","active-start",RING_LISTEN}};
const int ring_nslots=0;
static const char *const effects[]={"solid_cyan","solid_blue","solid_cyan","solid_green","solid_orange","solid_red","solid_white","zzz_rainbow","zzz_comets","zzz_magenta-pulse"};
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static struct ring_light light={.brightness=1,.color_brightness=1,.g=1,.b=1,.fill=100};
static char current[80],pending[80];
static int enabled,dirty;
static long long until;
static long long now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000LL+t.tv_nsec/1000000;}
static void queue(const char *s){snprintf(pending,sizeof pending,"%s",s);dirty=1;pthread_cond_signal(&cv);}
static const char *base(void){return light.on?effects[light.effect]:"off";}
static int muted(void){unsigned char bits[(KEY_MAX+8)/8]={0};int fd=open("/dev/input/event0",O_RDONLY|O_NONBLOCK);if(fd<0)return 0;int ok=ioctl(fd,EVIOCGKEY(sizeof bits),bits);close(fd);return ok>=0 && !(bits[KEY_MUTE/8]&(1<<(KEY_MUTE%8)));}
static void play(const char *s){
 if(muted() && !getenv("MISTO_RING_IGNORE_MUTE")) {
  fprintf(stderr,"ring: skipped %s: EVIOCGKEY reports KEY_MUTE=0 (active low)\n",s);return;
 }
 const char *cmd=getenv("MISTO_LIPC_SET");if(!cmd)cmd="/usr/local/bin/lipc-set-prop";
 fprintf(stderr,"ring: request %s\n",s);
 int p[2];if(pipe(p)){perror("ring: pipe");return;}
 pid_t id=fork();
 if(!id){
  close(p[0]);setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);
  /* hassmic ignores SIGCHLD. Native glibc LIPC tools may wait for their own
   * children; do not export that process policy to them. Capture exit status
   * through a small supervisor since hassmic itself auto-reaps children. */
  signal(SIGCHLD,SIG_DFL);signal(SIGPIPE,SIG_DFL);
  pid_t tool=fork();
  if(!tool){
   close(p[1]);prctl(PR_SET_PDEATHSIG,SIGKILL);
   int n=open("/dev/null",O_RDONLY);if(n>=0){dup2(n,0);if(n>2)close(n);}
   execl(cmd,cmd,"com.doppler.ledd","Pattern",s,(char*)0);
   perror("ring: exec lipc-set-prop");_exit(127);
  }
  int st=-1;if(tool>0){while(waitpid(tool,&st,0)<0){if(errno!=EINTR){st=-1;break;}}}
  write(p[1],&st,sizeof st);close(p[1]);_exit(0);
 }
 close(p[1]);
 if(id>0){
  struct pollfd f={p[0],POLLIN|POLLHUP,0};int rc;do{rc=poll(&f,1,2500);}while(rc<0&&errno==EINTR);
  if(rc<=0){kill(-id,SIGKILL);kill(id,SIGKILL);fprintf(stderr,"ring: %s timed out\n",s);}
  else {int st=-1;ssize_t n=read(p[0],&st,sizeof st);
   if(n==sizeof st&&st>=0&&WIFEXITED(st))fprintf(stderr,"ring: %s exit=%d\n",s,WEXITSTATUS(st));
   else fprintf(stderr,"ring: %s failed (status=%d, bytes=%ld)\n",s,st,(long)n);
  }
  waitpid(id,NULL,WNOHANG);
 }else perror("ring: fork");
 close(p[0]);
}
static void *worker(void *arg){(void)arg;for(;;){char s[80];pthread_mutex_lock(&mu);while(!dirty)pthread_cond_wait(&cv,&mu);/* Collapse an unset/set state transition. */pthread_mutex_unlock(&mu);usleep(30000);pthread_mutex_lock(&mu);snprintf(s,sizeof s,"%s",pending);dirty=0;pthread_mutex_unlock(&mu);play(s);}return NULL;}
void ring_start(void(*run)(const char*,const char*),void(*reload)(int)){
 (void)run;(void)reload;const char *cmd=getenv("MISTO_LIPC_SET");enabled=!access(cmd?cmd:"/usr/local/bin/lipc-set-prop",X_OK)&&!getenv("MISTO_RING_DISABLE");
 if(enabled){pthread_t t;if(pthread_create(&t,NULL,worker,NULL))enabled=0;else pthread_detach(t);}fprintf(stderr,"ring: EchoRoot ledd v4.1 %s (stock volume animation)\n",enabled?"enabled":"unavailable");
}
int ring_custom(void){return enabled;}
void ring_set(const char *s){if(!enabled)return;
 /* Only known EchoRoot names: no Android-only volume_step or setup tokens. */
 const char *p=NULL;long long ms=0;
 if(!strcmp(s,"active-start"))p="active-on";
 else if(!strcmp(s,"active-thinking"))p=s;
 else if(!strcmp(s,"active-talking"))p=s;
 else if(!strcmp(s,"active-end")){p=s;ms=700;}
 else if(!strcmp(s,"anim_start_error_short")){p=s;ms=1500;}
 else if(!strcmp(s,"zzz_rainbow")){p=s;ms=3000;}
 else if(!strcmp(s,"ready-timer")||!strcmp(s,"ready-timer-short"))p="ready-timer";
 else if(!strcmp(s,"mics-off_on"))p=s;
 else if(!strcmp(s,"mics-off_end")){p=s;ms=700;}
 else if(!strcmp(s,"call_incoming")||!strcmp(s,"call_outbound_ringing"))p="call_incoming";
 else if(!strcmp(s,"call_connected"))p="solid_green";
 else if(!strcmp(s,"call_connected_end")){p=s;ms=700;}
 if(!p)return;
 pthread_mutex_lock(&mu);snprintf(current,sizeof current,"%s",s);until=ms?now()+ms:0;queue(p);pthread_mutex_unlock(&mu);
}
void ring_unset(const char *s){if(!enabled)return;pthread_mutex_lock(&mu);if(!strcmp(current,s)){current[0]=0;until=0;queue(base());}pthread_mutex_unlock(&mu);}
void ring_tick(void){if(!enabled)return;pthread_mutex_lock(&mu);if(until&&now()>=until){until=0;current[0]=0;queue(base());}pthread_mutex_unlock(&mu);}
void ring_light_get(struct ring_light *l){pthread_mutex_lock(&mu);*l=light;pthread_mutex_unlock(&mu);}
void ring_light_set(const struct ring_light *l,unsigned flash){(void)flash;pthread_mutex_lock(&mu);light=*l;if(light.effect<0||light.effect>=ring_neffects)light.effect=0;if(enabled&&!current[0])queue(base());pthread_mutex_unlock(&mu);}
int ring_effect_listed(int i){return i>=0&&i<ring_neffects;}
int ring_assistant_steps(void){return 0;}int ring_per_led(void){return 0;}int ring_generate(const char *d){(void)d;return 1;}
int ring_slot_available(int i){(void)i;return 0;}int ring_slot_live(int i){(void)i;return 0;}int ring_leds(void){return 12;}
int ring_check(const char *a,size_t n,char *e,size_t c){(void)a;(void)n;if(c)snprintf(e,c,"Custom designs are unavailable on EchoRoot");return -1;}
void ring_reload(void){}int ring_designs_live(void){return 0;}char *ring_stock(int i,size_t *n){(void)i;(void)n;return NULL;}
int ring_rotation(int s){(void)s;return 0;}void ring_level(int d){(void)d;}int ring_band_rows(int s){(void)s;return -1;}
