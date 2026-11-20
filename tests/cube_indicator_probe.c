/* On-device integration probe: exercise real helper transitions without a HA pipeline. */
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int exact(int fd,void *data,size_t n,int write_it){
    char *p=data;while(n){ssize_t k=write_it?write(fd,p,n):read(fd,p,n);if(k<=0)return -1;p+=k;n-=k;}return 0;
}
static int indicator(int on,const char *pattern){
    struct sockaddr_un a={.sun_family=AF_UNIX};const char *name="cube-assist-audio";
    memcpy(a.sun_path+1,name,strlen(name));int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd<0||connect(fd,(void*)&a,sizeof(sa_family_t)+1+strlen(name)))return -1;
    uint32_t header[]={htonl(0x43554241),htonl(7),htonl(on),htonl(strlen(pattern))},reply;
    int rc=exact(fd,header,sizeof header,1)||exact(fd,&reply,4,0)||reply||
        exact(fd,(void*)pattern,strlen(pattern),1)||exact(fd,&reply,4,0)||reply;
    close(fd);return rc?-1:0;
}
static int transition(const char *from,const char *to){
    struct sockaddr_un a={.sun_family=AF_UNIX};const char *name="cube-assist-audio";
    memcpy(a.sun_path+1,name,strlen(name));int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if(fd<0||connect(fd,(void*)&a,sizeof(sa_family_t)+1+strlen(name)))return -1;
    uint32_t header[]={htonl(0x43554241),htonl(8),htonl(strlen(from)),htonl(strlen(to))},reply;
    int rc=exact(fd,header,sizeof header,1)||exact(fd,&reply,4,0)||reply||
        exact(fd,(void*)from,strlen(from),1)||exact(fd,(void*)to,strlen(to),1)||exact(fd,&reply,4,0)||reply;
    close(fd);return rc?-1:0;
}
int main(void){
    const char *states[]={"active-start","active-thinking","active-talking","active-end"};
    for(int i=0;i<4;i++){
        if(transition(i?states[i-1]:"",states[i]))return 1;
        usleep(400000);
    }
    if(indicator(0,states[3]))return 1;
    usleep(600000);puts("Indicator sequence finished");return 0;
}
