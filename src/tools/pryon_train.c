#define _POSIX_C_SOURCE 200809L
#include "pryon_qat.h"
#include "json.h"
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

struct bytes{unsigned char *p;size_t n;};
struct field{unsigned id,wire;uint64_t value;unsigned char *data;size_t n;};
struct model{
    struct bytes file;struct pq_head head;unsigned char *q,*s,*z,*sum,*b;
    unsigned char *graph,*graph_begin,*graph_end,*sum_begin,*sum_end,*sum_tensor;
    size_t graph_len,sum_tensor_len;int32_t sum_values[3];
};
static int read_file(const char *name,struct bytes *b,size_t max)
{
    FILE*f=fopen(name,"rb");if(!f)return -1;
    if(fseek(f,0,SEEK_END)){fclose(f);return -1;}long size=ftell(f);
    if(size<1||(size_t)size>max||fseek(f,0,SEEK_SET)){fclose(f);return -1;}
    b->n=(size_t)size;b->p=malloc(b->n+1);if(!b->p){fclose(f);return -1;}
    int bad=fread(b->p,1,b->n,f)!=b->n;fclose(f);b->p[b->n]=0;return bad?-1:0;
}
static int varint(unsigned char **p,unsigned char *end,uint64_t*v)
{
    *v=0;for(int s=0;s<64;s+=7){if(*p>=end)return -1;unsigned c=*(*p)++;if(s==63&&(c&0xfe))return -1;*v|=(uint64_t)(c&127)<<s;if(!(c&128))return 0;}return -1;
}
static int field(unsigned char **p,unsigned char *end,struct field*f)
{
    if(*p==end)return 0;uint64_t tag,len;if(varint(p,end,&tag)||!(tag>>3))return -1;
    memset(f,0,sizeof *f);f->id=(unsigned)(tag>>3);f->wire=tag&7;
    if(f->wire==0)return varint(p,end,&f->value)?-1:1;
    if(f->wire==2){if(varint(p,end,&len)||len>(uint64_t)(end-*p))return -1;}
    else if(f->wire==1)len=8;else if(f->wire==5)len=4;else return -1;
    if(len>(uint64_t)(end-*p))return -1;f->data=*p;f->n=(size_t)len;*p+=len;return 1;
}
static int text(char *s,size_t cap,const struct field*f)
{if(f->wire!=2||f->n>=cap)return -1;memcpy(s,f->data,f->n);s[f->n]=0;return 0;}
static int model_load(const char *path,struct model*m)
{
    memset(m,0,sizeof *m);if(read_file(path,&m->file,8*1024*1024))return -1;
    unsigned char *p=m->file.p,*end=p+m->file.n;struct field f;int rc;
    while(p<end){
        unsigned char*begin=p;if(field(&p,end,&f)<1)return -1;
        if(f.id==7&&f.wire==2){if(m->graph)return -1;m->graph=f.data;m->graph_len=f.n;m->graph_begin=begin;m->graph_end=p;}
    }
    if(!m->graph)return -1;
    char names[5][512]={{0}};int found=0;p=m->graph;end=p+m->graph_len;
    while((rc=field(&p,end,&f))>0)if(f.id==1&&f.wire==2){
        unsigned char *a=f.data,*z=a+f.n;struct field v;char name[512]="",op[80]="",inputs[5][512]={{0}};int count=0,r;
        while((r=field(&a,z,&v))>0){
            if(v.id==1){if(count>=5||text(inputs[count++],512,&v))break;}
            else if(v.id==3){if(text(name,sizeof name,&v))return -1;}
            else if(v.id==4){if(text(op,sizeof op,&v))return -1;}
        }
        if(!strcmp(op,"HybridGemm")&&strstr(name,"fc_output_ww/Tensordot/MatMul")){
            if(found++||count!=5)return -1;for(int k=0;k<4;k++)strcpy(names[k],inputs[k+1]);
        }
    }
    if(rc<0||found!=1)return -1;
    strcpy(names[4],"StatefulPartitionedCall/functional_5/fc_output_ww/BiasAdd/ReadVariableOp:0");
    unsigned char **dest[5]={&m->q,&m->s,&m->z,&m->sum,&m->b};
    p=m->graph;end=p+m->graph_len;
    while(p<end){
        unsigned char*begin=p;if(field(&p,end,&f)<1)return -1;
        if(f.id!=5||f.wire!=2)continue;
        unsigned char*a=f.data,*z=a+f.n,*raw=NULL;size_t rawlen=0;char name[512]="";
        int dtype=0,nd=0,ni=0,r;uint64_t dims[8]={0};int32_t ints[3]={0};struct field v;
        while((r=field(&a,z,&v))>0){
            if(v.id==8){if(text(name,sizeof name,&v))return -1;}
            else if(v.id==2&&v.wire==0)dtype=(int)v.value;
            else if(v.id==9&&v.wire==2){raw=v.data;rawlen=v.n;}
            else if(v.id==1){
                if(v.wire==0){if(nd>=8)return -1;dims[nd++]=v.value;}
                else if(v.wire==2){unsigned char*b=v.data,*be=b+v.n;while(b<be){if(nd>=8||varint(&b,be,&dims[nd++]))return -1;}}
            }else if(v.id==5){
                if(v.wire==0){if(ni<3)ints[ni]=(int32_t)v.value;ni++;}
                else if(v.wire==2){unsigned char*b=v.data,*be=b+v.n;uint64_t value;
                    while(b<be){if(varint(&b,be,&value))return -1;if(ni<3)ints[ni]=(int32_t)value;ni++;}}
            }
        }
        if(r<0)return -1;
        for(int k=0;k<5;k++)if(!strcmp(name,names[k])){
            if(*dest[k])return -1;
            if(k==0){if(dtype!=3||nd!=2||dims[0]!=3||dims[1]!=100||rawlen!=300||!raw)return -1;}
            else if(nd!=1||dims[0]!=3||dtype!=((k==1||k==4)?1:6))return -1;
            if(k==3){
                if(raw&&rawlen==12)memcpy(m->sum_values,raw,12);
                else if(!raw&&ni==3)memcpy(m->sum_values,ints,12);else return -1;
                m->sum=(unsigned char*)m->sum_values;m->sum_begin=begin;m->sum_end=p;
                m->sum_tensor=f.data;m->sum_tensor_len=f.n;
            }else{if(!raw||rawlen!=(k==0?300u:12u))return -1;*dest[k]=raw;}
        }
    }
    if(!m->q||!m->s||!m->z||!m->sum||!m->b)return -1;
    memcpy(m->head.q,m->q,300);memcpy(m->head.scale,m->s,12);memcpy(m->head.zero,m->z,12);memcpy(m->head.bias,m->b,12);
    return 0;
}
struct builder{unsigned char*p;size_t n,cap;};
static int append(struct builder*b,const void*p,size_t n)
{if(n>b->cap-b->n)return -1;memcpy(b->p+b->n,p,n);b->n+=n;return 0;}
static int putvar(struct builder*b,uint64_t v)
{unsigned char x[10];size_t n=0;do{x[n]=(v&127)|(v>127?128:0);v>>=7;n++;}while(v);return append(b,x,n);}
static int blob(struct builder*b,unsigned id,const void*p,size_t n)
{return putvar(b,((uint64_t)id<<3)|2)||putvar(b,n)||append(b,p,n)?-1:0;}
static int model_save(struct model*m,const float*w,float bias,const char*path)
{
    int8_t q[100];float scale;int32_t zero,sum;pq_quantize(w,q,&scale,&zero,&sum);
    memcpy(m->q+100,q,100);memcpy(m->s+4,&scale,4);memcpy(m->z+4,&zero,4);memcpy(m->sum+4,&sum,4);memcpy(m->b+4,&bias,4);
    memcpy(m->head.q[1],q,100);m->head.scale[1]=scale;m->head.zero[1]=zero;m->head.bias[1]=bias;
    /* Original sums use packed int32_data, not raw_data. Rebuild only that
     * TensorProto and the enclosing lengths; every other field stays intact. */
    unsigned char tensorbuf[2048];struct builder tensor={tensorbuf,0,sizeof tensorbuf};
    unsigned char*p=m->sum_tensor,*end=p+m->sum_tensor_len;struct field f;
    while(p<end){unsigned char*begin=p;if(field(&p,end,&f)<1)return -1;
        if(f.id!=5&&f.id!=9&&append(&tensor,begin,(size_t)(p-begin)))return -1;}
    if(blob(&tensor,9,m->sum,12))return -1;
    size_t cap=m->file.n+4096;unsigned char*gb=malloc(cap),*mb=malloc(cap);if(!gb||!mb){free(gb);free(mb);return -1;}
    struct builder graph={gb,0,cap},model={mb,0,cap};
    int bad=append(&graph,m->graph,(size_t)(m->sum_begin-m->graph))||blob(&graph,5,tensor.p,tensor.n)||
        append(&graph,m->sum_end,(size_t)(m->graph+m->graph_len-m->sum_end))||
        append(&model,m->file.p,(size_t)(m->graph_begin-m->file.p))||blob(&model,7,graph.p,graph.n)||
        append(&model,m->graph_end,(size_t)(m->file.p+m->file.n-m->graph_end));
    if(bad){free(gb);free(mb);return -1;}
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0){free(gb);free(mb);return -1;}
    size_t at=0;while(at<model.n){ssize_t n=write(fd,mb+at,model.n-at);if(n<=0){bad=-1;break;}at+=(size_t)n;}
    if(fsync(fd)||close(fd))bad=-1;free(gb);free(mb);return bad;
}
static int clip_load(const char*path,int label,const struct pq_head*h,struct pq_clip*c)
{
    struct bytes b={0};if(read_file(path,&b,4096u*25*100*4+16))return -1;
    uint32_t header[4];if(b.n<16){free(b.p);return -1;}memcpy(header,b.p,16);
    if(header[0]!=0x46595250||header[1]!=1||header[2]!=25||header[3]!=100||(b.n-16)%10000){free(b.p);return -1;}
    memset(c,0,sizeof *c);c->cycles=(int)((b.n-16)/10000);c->label=label;
    c->x=malloc(b.n-16);if(!c->x){free(b.p);return -1;}memcpy(c->x,b.p+16,b.n-16);free(b.p);
    return pq_prepare(c,h);
}
static int safe_name(const char*s)
{if(!*s||strstr(s,".."))return 0;for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='-'||*s=='_'||*s=='.'))return 0;return 1;}

int main(int argc,char**argv)
{
    if(argc!=4&&argc!=5){fprintf(stderr,"usage: pryon-train SOURCE_ONNX FEATURE_MANIFEST NEW_ONNX [QAT_STEPS]\n");return 2;}
    int steps=argc==5?atoi(argv[4]):1600;if(steps<1||steps>100000)return 2;
    struct model model;if(model_load(argv[1],&model)){fprintf(stderr,"Unsupported original-architecture head\n");return 1;}
    struct bytes manifest={0};struct json j={0};if(read_file(argv[2],&manifest,256*1024)||json_parse(&j,(char*)manifest.p,manifest.n))return 1;
    int n=json_len(&j,0);if(json_type(&j,0)!=J_ARR||n<12||n>200){fprintf(stderr,"Require12..200feature records, got%d\n",n);return 1;}
    char directory[1024];if(strlen(argv[2])>=sizeof directory)return 1;strcpy(directory,argv[2]);char*slash=strrchr(directory,'/');if(slash)*slash=0;else strcpy(directory,".");
    struct {char path[1400];int label,train;}*rows=calloc((size_t)n,sizeof *rows);
    struct pq_clip*clips=calloc((size_t)n,sizeof *clips);if(!rows||!clips)return 1;
    int index=0,count[2]={0};
    JSON_EACH(&j,0,k){
        char file[240],split[32];int obj=k;
        if(!json_str(&j,json_get(&j,obj,"path"),file,sizeof file)||!safe_name(file)||
           !json_str(&j,json_get(&j,obj,"split"),split,sizeof split))return 1;
        int label=(int)json_int(&j,json_get(&j,obj,"label"),-1);if(label!=0&&label!=1)return 1;
        if(strcmp(split,"train")&&strcmp(split,"test")&&strcmp(split,"diagnostic"))return 1;
        rows[index].label=label;rows[index].train=!strcmp(split,"train");
        if(snprintf(rows[index].path,sizeof rows[index].path,"%s/%s",directory,file)>=(int)sizeof rows[index].path)return 1;
        if(rows[index].train)count[label]++;index++;
    }
    json_free(&j);free(manifest.p);if(index!=n||count[0]<5||count[1]<5){fprintf(stderr,"TRAIN counts invalid:%d/%d rows%d/%d\n",count[0],count[1],index,n);return 1;}
    int train=0;for(int i=0;i<n;i++)if(rows[i].train){if(clip_load(rows[i].path,rows[i].label,&model.head,clips+train)){fprintf(stderr,"Invalid TRAIN features %d\n",i);return 1;}train++;}
    struct pq_fit fit={.head=&model.head};if(pq_normalize(&fit,clips,train)){fprintf(stderr,"Invalid TRAIN normalization\n");return 1;}
    printf("{\"phase\":\"loaded\",\"training_recordings\":%d}\n",train);fflush(stdout);
    float w[100],bias;double loss;struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
    if(pq_train(&fit,clips,train,steps,w,&bias,&loss)){fprintf(stderr,"Native optimizer failed\n");return 1;}
    if(model_save(&model,w,bias,argv[3])){perror("new model");return 1;}
    printf("{\"phase\":\"frozen\",\"training_loss\":%.9g}\n",loss);fflush(stdout);
    /* Reserved features are opened only AFTER the model has been written/fsynced. */
    int counts[2][4]={{0}},ci=0;
    for(int i=0;i<n;i++){
        struct pq_clip held={0},*c;
        if(rows[i].train)c=clips+ci++;
        else{if(clip_load(rows[i].path,rows[i].label,&model.head,&held))return 1;c=&held;}
        double peak;int accepts=pq_accepts(c,&model.head,&peak);if(accepts<0)return 1;
        int outcome=rows[i].label?(accepts?0:1):(accepts?2:3);counts[rows[i].train?0:1][outcome]++;
        printf("{\"phase\":\"evaluation\",\"index\":%d,\"train\":%s,\"label\":%d,\"accepts\":%d,\"peak\":%.9g}\n",i,rows[i].train?"true":"false",rows[i].label,accepts,peak);fflush(stdout);
        if(!rows[i].train)pq_release(&held);
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);double elapsed=t1.tv_sec-t0.tv_sec+(t1.tv_nsec-t0.tv_nsec)/1e9;
    printf("{\"phase\":\"complete\",\"seconds\":%.3f,\"train\":{\"tp\":%d,\"fn\":%d,\"fp\":%d,\"tn\":%d},\"test\":{\"tp\":%d,\"fn\":%d,\"fp\":%d,\"tn\":%d}}\n",elapsed,counts[0][0],counts[0][1],counts[0][2],counts[0][3],counts[1][0],counts[1][1],counts[1][2],counts[1][3]);
    for(int i=0;i<train;i++)pq_release(clips+i);free(clips);free(rows);free(model.file.p);return 0;
}
