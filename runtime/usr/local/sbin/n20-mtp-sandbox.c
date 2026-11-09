#define _GNU_SOURCE
/* Confine the root-started MTP responder without modifying world permissions. */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static char jail[PATH_MAX], world_target[PATH_MAX], world_source[PATH_MAX];
static int world_mounted,cleanup_active,test_mode;
static void cleanup(int test);

static void fail(const char *what){perror(what);if(cleanup_active)cleanup(test_mode);exit(1);}
static void path(char *out,size_t n,const char *suffix){
    int count=snprintf(out,n,"%s%s",jail,suffix);
    if(count<0||(size_t)count>=n){errno=ENAMETOOLONG;fail("sandbox path");}
}
static void directory(const char *suffix){char p[PATH_MAX];path(p,sizeof(p),suffix);if(mkdir(p,0755)<0&&errno!=EEXIST)fail("sandbox mkdir");}
static int uid_in_use(unsigned wanted){
    DIR *proc=opendir("/proc");if(!proc)return -1;struct dirent *entry;int used=0;
    while((entry=readdir(proc))){
        char *end;long pid=strtol(entry->d_name,&end,10);if(!*entry->d_name||*end||pid<=0)continue;
        char name[64];int n=snprintf(name,sizeof(name),"/proc/%ld/status",pid);if(n<0||(size_t)n>=sizeof(name)){used=-1;break;}
        FILE *f=fopen(name,"r");if(!f){if(errno==ENOENT||errno==ESRCH)continue;used=-1;break;}
        char line[256];while(fgets(line,sizeof(line),f)){unsigned real,effective,saved,fs;if(sscanf(line,"Uid: %u %u %u %u",&real,&effective,&saved,&fs)==4){if(real==wanted||effective==wanted||saved==wanted||fs==wanted)used=1;break;}}
        fclose(f);if(used)break;
    }
    closedir(proc);return used;
}
static void bind_path(const char *source,const char *suffix,int isdir,int readonly){
    char target[PATH_MAX];path(target,sizeof(target),suffix);
    if(isdir){if(mkdir(target,0755)<0)fail("sandbox directory");}
    else {int fd=open(target,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0644);if(fd<0)fail("sandbox file");close(fd);}
    if(mount(source,target,NULL,MS_BIND,NULL)<0)fail("sandbox bind");
    if(readonly&&mount(NULL,target,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV,NULL)<0)fail("sandbox read-only bind");
}
static int detach_world(void){if(world_mounted){if(umount2(world_target,MNT_DETACH)<0){perror("sandbox unmount world");return 0;}world_mounted=0;}return 1;}
static int bind_world(void){
    /* Pin the directory before mounting so a concurrent rename or symlink
       cannot redirect a privileged mount to another host path. */
    int fd=open(world_source,O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return 0;
    char source[64];snprintf(source,sizeof(source),"/proc/self/fd/%d",fd);
    int ok=mount(source,world_target,NULL,MS_BIND,NULL)==0;
    if(ok&&mount(NULL,world_target,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)<0){umount2(world_target,MNT_DETACH);ok=0;}
    close(fd);world_mounted=ok;return ok;
}
static void cleanup(int test){
    static const char *files[]={"/usr/bin/umtprd","/usr/bin/setpriv","/lib/ld-linux-aarch64.so.1","/lib/aarch64-linux-gnu/libc.so.6","/lib/aarch64-linux-gnu/libcap-ng.so.0","/etc/umtprd/umtprd.conf","/bin/busybox","/dev/null"};
    static const char *dirs[]={"/dev/ffs-mtp","/srv/minecraft/world","/srv/minecraft","/srv","/dev","/usr/bin","/usr","/lib/aarch64-linux-gnu","/lib","/etc/umtprd","/etc","/bin"};
    char p[PATH_MAX];(void)detach_world();if(!test){path(p,sizeof(p),"/dev/ffs-mtp");umount2(p,MNT_DETACH);}
    for(size_t i=0;i<sizeof(files)/sizeof(files[0]);i++){path(p,sizeof(p),files[i]);umount2(p,MNT_DETACH);}
    umount2(jail,MNT_DETACH);
    for(size_t i=0;i<sizeof(files)/sizeof(files[0]);i++){path(p,sizeof(p),files[i]);unlink(p);}
    for(size_t i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++){path(p,sizeof(p),dirs[i]);rmdir(p);}
    rmdir(jail);
}
int main(int argc,char **argv){
    int test=argc==3&&!strcmp(argv[1],"--test");test_mode=test;
    if(argc!=1&&!test){fprintf(stderr,"usage: %s [--test parent-directory]\n",argv[0]);return 2;}
    if(geteuid()!=0){fprintf(stderr,"MTP sandbox requires root at startup\n");return 1;}
    /* Reserve a numeric identity distinct from common shared service users. */
    unsigned identity=test?61124:61123;
    errno=0;if(getpwuid(identity)||errno){fprintf(stderr,"MTP sandbox UID is not available\n");return 1;}
    errno=0;if(getgrgid(identity)||errno){fprintf(stderr,"MTP sandbox GID is not available\n");return 1;}
    if(uid_in_use(identity)!=0){fprintf(stderr,"MTP sandbox UID is already in use or cannot be checked\n");return 1;}
    const char *parent=test?argv[2]:"/srv/minecraft";
    if(parent[0]!='/'||strlen(parent)>PATH_MAX-16){fprintf(stderr,"invalid world parent\n");return 1;}
    snprintf(world_source,sizeof(world_source),"%s/world",parent);
    struct stat st;if(lstat("/run/n20",&st)||!S_ISDIR(st.st_mode)||st.st_uid!=0||(st.st_mode&022)){fprintf(stderr,"unsafe runtime directory\n");return 1;}
    /* Serialize startup so two responders cannot share the reserved UID. */
    const char *lockpath=test?"/run/n20/mtp-sandbox-test.lock":"/run/n20/mtp-sandbox.lock";
    int lockfd=open(lockpath,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lockfd<0||fstat(lockfd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=0||(st.st_mode&077)||flock(lockfd,LOCK_EX|LOCK_NB)){fprintf(stderr,"MTP sandbox lock is unavailable or in use\n");return 1;}
    if(unshare(CLONE_NEWNS|CLONE_NEWNET)<0)fail("sandbox namespace");
    if(mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL)<0)fail("sandbox private mounts");
    snprintf(jail,sizeof(jail),"/run/n20/mtp-sandbox-XXXXXX");if(!mkdtemp(jail))fail("sandbox temporary directory");cleanup_active=1;
    path(world_target,sizeof(world_target),"/srv/minecraft/world");
    if(chmod(jail,0755)<0)fail("sandbox root mode");
    directory("/bin");directory("/usr");directory("/usr/bin");directory("/lib");directory("/lib/aarch64-linux-gnu");directory("/etc");directory("/etc/umtprd");directory("/dev");directory("/srv");directory("/srv/minecraft");directory("/srv/minecraft/world");
    if(mount(jail,jail,NULL,MS_BIND,NULL)<0)fail("sandbox root bind");
    bind_path("/usr/bin/setpriv","/usr/bin/setpriv",0,1);
    bind_path("/lib/ld-linux-aarch64.so.1","/lib/ld-linux-aarch64.so.1",0,1);
    bind_path("/lib/aarch64-linux-gnu/libc.so.6","/lib/aarch64-linux-gnu/libc.so.6",0,1);
    bind_path("/lib/aarch64-linux-gnu/libcap-ng.so.0","/lib/aarch64-linux-gnu/libcap-ng.so.0",0,1);
    bind_path("/dev/null","/dev/null",0,0);
    if(test)bind_path("/usr/local/bin/busybox","/bin/busybox",0,1);
    else {bind_path("/usr/local/lib/n20/umtprd","/usr/bin/umtprd",0,1);bind_path("/etc/umtprd/umtprd.conf","/etc/umtprd/umtprd.conf",0,1);bind_path("/dev/ffs-mtp","/dev/ffs-mtp",1,0);}
    /* The child retains DAC override for read access: the jail itself must
       therefore be read-only, independently of its directory permissions. */
    if(mount(NULL,jail,NULL,MS_BIND|MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)<0)fail("sandbox read-only root");
    int watch=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);if(watch<0)fail("sandbox inotify");
    if(inotify_add_watch(watch,parent,IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO|IN_DELETE_SELF|IN_MOVE_SELF)<0)fail("sandbox watch world parent");
    if(!bind_world()){fprintf(stderr,"sandbox world unavailable or unsafe\n");cleanup(test);return 1;}
    sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGCHLD);sigaddset(&mask,SIGTERM);sigaddset(&mask,SIGINT);
    if(sigprocmask(SIG_BLOCK,&mask,NULL)<0)fail("sandbox block signals");
    int signals=signalfd(-1,&mask,SFD_CLOEXEC|SFD_NONBLOCK);if(signals<0)fail("sandbox signalfd");
    pid_t child=fork();if(child<0)fail("sandbox fork");
    if(child==0){
        cleanup_active=0;
        sigprocmask(SIG_UNBLOCK,&mask,NULL);close(watch);close(signals);
        /* No inherited host descriptors may provide a path out of the jail. */
        DIR *fds=opendir("/proc/self/fd");if(!fds)fail("sandbox enumerate descriptors");
        int own=dirfd(fds);struct dirent *entry;
        while((entry=readdir(fds))){char *end;long fd=strtol(entry->d_name,&end,10);if(*entry->d_name&&!*end&&fd>=3&&fd!=own&&fd<=INT_MAX)close((int)fd);}
        closedir(fds);
        if(chroot(jail)<0||chdir("/")<0)fail("sandbox chroot");
        /* DAC override permits reading mode-600 world files and opening the
           FunctionFS endpoints. Do not grant DAC_READ_SEARCH: it also enables
           open_by_handle_at, which can bypass pathname confinement. */
        char *args[]={"setpriv","--bounding-set=-all,+dac_override","--inh-caps=+dac_override","--ambient-caps=+dac_override",test?"--reuid=61124":"--reuid=61123",test?"--regid=61124":"--regid=61123","--clear-groups","--no-new-privs",test?"/bin/busybox":"/usr/bin/umtprd",test?"sh":NULL,NULL};
        execv("/usr/bin/setpriv",args);fail("sandbox exec");
    }
    if(test){puts("SANDBOX_READY");fflush(stdout);}
    int status=0,done=0,world_changed=0;
    while(!done){
        struct pollfd fds[2]={{watch,POLLIN,0},{signals,POLLIN,0}};
        if(poll(fds,2,-1)<0){if(errno==EINTR)continue;perror("sandbox poll");break;}
        if(fds[0].revents&(POLLERR|POLLHUP|POLLNVAL)){fprintf(stderr,"sandbox world watch failed\n");break;}
        if(fds[0].revents&POLLIN){
            char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));ssize_t n;
            while((n=read(watch,buffer,sizeof(buffer)))>0){
                for(char *p=buffer;p<buffer+n;){struct inotify_event *e=(struct inotify_event *)p;p+=sizeof(*e)+e->len;
                    if(e->mask&(IN_DELETE_SELF|IN_MOVE_SELF|IN_IGNORED)){fprintf(stderr,"sandbox world parent moved\n");done=1;break;}
                    if((e->mask&IN_Q_OVERFLOW)||(e->len&&!strcmp(e->name,"world"))){if(!detach_world()){done=1;break;}world_changed=1;done=1;if(test){puts("WORLD_CHANGED");fflush(stdout);}break;}
                }
            }
        }
        if(fds[1].revents&POLLIN){struct signalfd_siginfo s;while(read(signals,&s,sizeof(s))==sizeof(s)){if(s.ssi_signo==SIGCHLD){pid_t r=waitpid(child,&status,WNOHANG);if(r==child){child=0;done=1;}}else done=1;}}
    }
    if(child>0){kill(child,SIGTERM);while(waitpid(child,&status,0)<0&&errno==EINTR){}}
    close(watch);close(signals);cleanup(test);
    return world_changed?75:WIFEXITED(status)?WEXITSTATUS(status):1;
}
