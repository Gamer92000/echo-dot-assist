#define _GNU_SOURCE
#include "cube.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
void wake_property(const char *name,int value){(void)name;(void)value;}
static void equal(const char *want,int field){char value[128];cube_route_text(field,value,sizeof value);assert(!strcmp(want,value));}
int main(void){
    char home[]="/tmp/cube-routing-test-XXXXXX";assert(mkdtemp(home));
    setenv("HASSMIC_STATE",home,1);setenv("CUBE_ASSIST_HOME",home,1);
    assert(cube_route_setting(0,-1)==0);assert(cube_route_setting(1,-1)==100);assert(cube_route_setting(3,-1)==17);
    assert(cube_route_setting(0,2)==2);assert(cube_route_setting(0,-1)==2);
    assert(cube_route_setting(1,62)==62);assert(cube_route_setting(2,24)==24);assert(cube_route_setting(1,-1)==62);
    assert(cube_route_setting(0,3)==-1);assert(cube_route_setting(2,101)==-1);assert(cube_route_setting(2,-1)==24);
    equal("Unknown",0);equal("Unconfirmed (status stale)",1);
    struct timespec t;clock_gettime(CLOCK_BOOTTIME,&t);long long now=(long long)t.tv_sec*1000+t.tv_nsec/1000000;
    char path[512];snprintf(path,sizeof path,"%s/runtime-status",home);setenv("CUBE_ROUTING_STATUS",path,1);
    FILE *f=fopen(path,"w");assert(f);fprintf(f,"%lld 1 2 2 0 2\n",now);fclose(f);
    equal("On",0);equal("TV",1);equal("None",2);equal("Verified HDMI receiver sense",3);equal("TV",4);
    f=fopen(path,"w");fprintf(f,"%lld 2 0 1 0 2\n",now);fclose(f);equal("Off",0);equal("Idle",1);equal("Cube speaker",4);
    f=fopen(path,"w");fprintf(f,"%lld 1 2 2 0 2\n",now-6000);fclose(f);equal("Unknown",0);equal("Unconfirmed (status stale)",1);
    f=fopen(path,"w");fputs("broken",f);fclose(f);equal("Unknown",0);
    unlink(path);for(int i=0;i<4;i++){snprintf(path,sizeof path,"%s/%s",home,cube_route_names[i]);unlink(path);}rmdir(home);
    puts("PASS: route settings persist independently, reject invalid values, and report actual/idle/stale states");
}
