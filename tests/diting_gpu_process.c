/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include "../userland/gpu-test-process.h"
extern char **environ;
static volatile sig_atomic_t notifications;
static void notify(int number){(void)number;++notifications;}
struct Context {uint8_t data[4096];unsigned bytes,ticks;int cancel,reject;};
static int data(void *pointer,const uint8_t *bytes,unsigned count){
    struct Context *context=pointer;if(context->reject)return 0;
    assert(count<=sizeof(context->data)-context->bytes);memcpy(context->data+context->bytes,bytes,count);context->bytes+=count;return 1;
}
static int tick(void *pointer){struct Context *context=pointer;++context->ticks;return !context->cancel||context->ticks<5;}
static unsigned descriptors(void){DIR *dir=opendir("/proc/self/fd");assert(dir);unsigned count=0;while(readdir(dir))++count;closedir(dir);return count;}
static int child(const char *mode,const char *lock_path){
    struct rlimit zero={0,0};setrlimit(RLIMIT_CORE,&zero);
    if(!strcmp(mode,"exit"))return 7;
    if(!strcmp(mode,"crash")){assert(write(1,"prefix",6)==6);raise(SIGABRT);return 9;}
    if(!strcmp(mode,"eof")){close(1);usleep(150000);return 0;}
    if(!strcmp(mode,"lease")){
        assert(fcntl(198,F_GETFD)>=0);
        int other=open(lock_path,O_RDWR);assert(other>=0);assert(flock(other,LOCK_EX|LOCK_NB)&&errno==EWOULDBLOCK);close(other);
    }
    if(!strcmp(mode,"orphan")){dprintf(1,"leased %d\n",getpid());usleep(900000);return 0;}
    if(!strcmp(mode,"overflow")){uint8_t large[1024];memset(large,0xcc,sizeof(large));for(unsigned i=0;i<16;++i)if(write(1,large,sizeof(large))<0)return 8;pause();return 0;}
    if(!strcmp(mode,"hang")||!strcmp(mode,"cancel")||!strcmp(mode,"reject")){signal(SIGTERM,SIG_IGN);assert(write(1,"ready",5)==5);for(;;)pause();}
    for(unsigned i=0;i<64;++i){const uint8_t byte=(uint8_t)i;assert(write(1,&byte,1)==1);if(i%8==0)usleep(1000);}
    if(!strcmp(mode,"interrupt"))for(unsigned i=0;i<3;++i){kill(getppid(),SIGUSR1);usleep(10000);}
    return 0;
}
int main(int argc,char **argv){
    if(argc==5&&!strcmp(argv[1],"worker"))return child(argv[2],argv[3]);
    if(argc!=3)return 64;
    const char *mode=argv[1];int lease=open(argv[2],O_RDWR|O_CREAT|O_CLOEXEC,0600);assert(lease>=0&&!flock(lease,LOCK_EX|LOCK_NB));
    char *arguments[]={argv[0],"worker",argv[1],argv[2],"fixed",NULL};
    if(!strcmp(mode,"lease-orphan")){
        arguments[2]="orphan";posix_spawn_file_actions_t actions;posix_spawnattr_t attr;pid_t pid=0;
        assert(!posix_spawn_file_actions_init(&actions)&&!posix_spawnattr_init(&attr));
        assert(!posix_spawn_file_actions_adddup2(&actions,lease,198));
        assert(!posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP)&&!posix_spawnattr_setpgroup(&attr,0));
        assert(!posix_spawn(&pid,argv[0],&actions,&attr,arguments,environ));_Exit(0);
    }
    struct Context context={0};context.cancel=!strcmp(mode,"cancel");context.reject=!strcmp(mode,"reject");
    uint32_t timeout=!strcmp(mode,"hang")?100:2000;unsigned limit=!strcmp(mode,"overflow")?2048:4096;
    int saved=-1;if(!strcmp(mode,"closed-stdout")){saved=dup(1);assert(saved>=0);close(1);arguments[2]="success";}
    signal(SIGUSR1,notify);const uint64_t start=diting_gpu_monotonic_ms();const unsigned before=descriptors();
    struct DitingGpuProcessResult result={0};
    const unsigned loops=!strcmp(mode,"leaks")?32:1;
    if(loops>1)arguments[2]="success";
    for(unsigned i=0;i<loops;++i){memset(&context.data,0,sizeof(context.data));context.bytes=0;
        result=diting_gpu_process(!strcmp(mode,"missing")?"/nonexistent/diting-owned-worker":argv[0],arguments,environ,lease,timeout,limit,&context,data,tick);
    }
    assert(descriptors()==before);
    const uint64_t elapsed=diting_gpu_monotonic_ms()-start;
    if(saved>=0){assert(dup2(saved,1)==1);close(saved);}
    if(!strcmp(mode,"hang"))assert(result.error==ETIMEDOUT&&result.signal==SIGKILL&&!result.unreaped&&elapsed<1500);
    else if(!strcmp(mode,"cancel"))assert(result.cancelled&&!result.unreaped&&result.signal==SIGKILL);
    else if(!strcmp(mode,"reject"))assert(result.error==EIO&&!result.unreaped);
    else if(!strcmp(mode,"overflow"))assert(result.error==EOVERFLOW&&result.bytes<=limit&&!result.unreaped);
    else if(!strcmp(mode,"crash"))assert(!result.error&&result.signal==SIGABRT&&context.bytes==6);
    else if(!strcmp(mode,"exit"))assert(!result.error&&result.exit_code==7);
    else if(!strcmp(mode,"missing"))assert(result.error==ENOENT&&result.pid==0);
    else if(!strcmp(mode,"eof"))assert(!result.error&&result.exit_code==0&&elapsed>=100&&context.ticks<30);
    else {assert(!result.error&&!result.signal&&result.exit_code==0&&context.bytes==64);for(unsigned i=0;i<64;++i)assert(context.data[i]==i);}
    if(!strcmp(mode,"interrupt"))assert(notifications>0);
    close(lease);printf("PASS %s bytes=%u ticks=%u elapsed_ms=%llu\n",mode,result.bytes,context.ticks,(unsigned long long)elapsed);return 0;
}
