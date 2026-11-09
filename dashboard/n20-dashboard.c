#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define W 1080
#define H 2400
#define BG 0x000000
#define PANEL 0x101A17
#define BORDER 0x263830
#define WHITE 0xECF7EF
#define MUTED 0x9DAFA4
#define DIM 0x6C8174
#define GREEN 0x9CEBB2
#define GREEN_DARK 0x1B3D2A
#define RED 0xF3A69F
#define RED_DARK 0x422321
#define AMBER 0xEACB81
#define AMBER_DARK 0x3D3420

static int drmfd=-1, touchfd=-1, powerfd=-1;
static uint32_t crtc_id, conn_id, fb_id, handle, pitch, dpms_id;
static drmModeModeInfo mode;
static void *mapped, *canvas;
static size_t map_size;
static FT_Library ft;
static FT_Face regular, bold, mono;
static int awake=1, page=0, confirm=0, last_server=-1, saved_brightness=1215;
static int panel_off, display_failed;
static int touch_x=0,touch_y=0,touch_down=0;
static int touch_slot=0, primary_slot=-1, touch_blocked=0, input_sync_lost=0;
static uint32_t active_slots;
static int xmin=0,xmax=1079,ymin=0,ymax=2399;
static long long last_activity=0, last_draw=0, message_until=0;
static long long metric_input_at, metric_window, metric_render_ms, metric_latency_ms, metric_latency_max;
static unsigned metric_frames, metric_inputs;
static int metrics_enabled, touch_monotonic, last_fresh=-1, last_message_visible=-1;
static char message[128];
static volatile sig_atomic_t exiting=0, sleep_requested=0;
static int signal_pipe[2]={-1,-1};
static int state_watch_fd=-1;
enum { HOME, PLAYERS, SETTINGS, LOGS, BACKUPS, WIFI, TOOLS, SERVER_SETTINGS, DEVICE_SETTINGS };
typedef struct {const char *title,*description;int page;} ToolEntry;
/* Add future tools here; the launcher paginates without changing navigation. */
static const ToolEntry tool_entries[]={
    {"World backups","Create or restore a saved world.",BACKUPS},
    {"Wi-Fi","Connect the phone to a network.",WIFI},
    {"Server log","Read recent server activity.",LOGS},
};
#define TOOL_COUNT ((int)(sizeof(tool_entries)/sizeof(tool_entries[0])))
enum { A_PAGE=1, A_START, A_RESTART, A_STOP, A_CONFIRM, A_CANCEL,
    A_LIST, A_ADD, A_REMOVE, A_ALLOW, A_DIFFICULTY, A_MAX, A_VIEW, A_SIM,
    A_BRIGHT, A_TIMEOUT, A_BACKUP, A_RESTORE, A_KEY, A_ERASE, A_SUBMIT,
    A_SHIFT, A_SCROLL, A_WIFI_SELECT, A_WIFI_SCAN, A_WIFI_DISCONNECT, A_WIFI_SPACE, A_WIFI_SHOW, A_WIFI_LEFT, A_WIFI_RIGHT, A_NOTICE };
typedef struct { int x,y,w,h,action,value; char arg[96]; } Hit;
static Hit hits[100], pressed_hit;
static int hit_count, pressed_valid, down_x, down_y, drag_cancel;
static int list_mode, list_offset, wifi_count, keyboard_open, keyboard_shift, difficulty_open, notice_open, idle_seconds=120;
static char wifi_password[65], wifi_selected[65], wifi_label[80];
static int wifi_keyboard_mode, wifi_show_password;
static size_t wifi_cursor;
static char typed_name[17], selected_name[32], selected_backup[96], selected_backup_label[96];
static long confirm_server_pid;static unsigned long long confirm_server_start;
static long long input_block_until, helper_due, state_read_due, display_save_due;
static int pending_touch_down, pending_touch_up;
static pid_t helper_pid;
static pid_t display_save_pid;
static int helper_mutation;
static char queued_command[32], queued_arg[96];
typedef struct {
    int valid, players, allow_count, backup_count, max_players, view_distance, simulation_distance, settings_pending, server_port, charge_stop, charge_resume;
    long updated;
    char server[24], whitelist[12], difficulty[20], error[160], operation[160];
    char player[64][32], allow[128][32], backup[64][96], backup_id[64][96];
} State;
static State state;

static long long now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static void signal_wakeup(void){int saved=errno;char byte=1;if(signal_pipe[1]>=0)(void)write(signal_pipe[1],&byte,1);errno=saved;}
static void on_signal(int sig){(void)sig;exiting=1;signal_wakeup();}
static void on_sleep_signal(int sig){(void)sig;sleep_requested=1;signal_wakeup();}
static void on_child_signal(int sig){(void)sig;signal_wakeup();}
/* Damage is tracked as whole scanline ranges; no extra framebuffer is needed. */
static int dirty_top=0,dirty_bottom=H,clip_top=0,clip_bottom=H;
static void invalidate_full(void){dirty_top=0;dirty_bottom=H;last_draw=0;}
static void invalidate_rows(int top,int bottom){
    if(top<0)top=0;
    if(bottom>H)bottom=H;
    if(top>=bottom)return;
    if(top<dirty_top)dirty_top=top;
    if(bottom>dirty_bottom)dirty_bottom=bottom;
    last_draw=0;
}static void msg(const char *s){snprintf(message,sizeof(message),"%s",s);message_until=now_ms()+8000;invalidate_full();}
static void rect(int x,int y,int w,int h,uint32_t c){if(x<0){w+=x;x=0;}if(y<clip_top){h-=clip_top-y;y=clip_top;}if(x+w>W)w=W-x;if(y+h>clip_bottom)h=clip_bottom-y;for(int j=0;j<h;j++){uint32_t*p=(uint32_t*)((char*)canvas+(size_t)(y+j)*pitch)+x;for(int i=0;i<w;i++)p[i]=c;}}
static uint32_t blend(uint32_t bg,uint32_t fg,unsigned a){unsigned inv=255-a;unsigned r=(((bg>>16)&255)*inv+((fg>>16)&255)*a)/255;unsigned g=(((bg>>8)&255)*inv+((fg>>8)&255)*a)/255;unsigned b=((bg&255)*inv+(fg&255)*a)/255;return (r<<16)|(g<<8)|b;}
/* Decode one UTF-8 scalar, consuming one byte on malformed input. */
static unsigned next_codepoint(const char **input){
    const unsigned char *p=(const unsigned char *)*input;unsigned c=*p++, value;int count;
    if(c<0x80){*input=(const char *)p;return c<32?'?':c;}
    if(c>=0xc2&&c<=0xdf){value=c&31;count=1;}
    else if(c>=0xe0&&c<=0xef){value=c&15;count=2;}
    else if(c>=0xf0&&c<=0xf4){value=c&7;count=3;}
    else{*input=(const char *)p;return '?';}
    for(int i=0;i<count;i++){if((p[i]&0xc0)!=0x80){*input=(const char *)p;return '?';}value=(value<<6)|(p[i]&63);}
    if((count==2&&value<0x800)||(count==3&&value<0x10000)||(value>=0xd800&&value<=0xdfff)||value>0x10ffff){*input=(const char *)p;return '?';}
    *input=(const char *)(p+count);return value;
}
static unsigned glyph_code(FT_Face face,unsigned c){return FT_Get_Char_Index(face,c)?c:'?';}
/* Bounded LRU cache: rasterization is reused without a full-screen layer. */
#define GLYPH_SLOTS 128
#define GLYPH_BYTES (256*1024)
typedef struct {FT_Face face;int size,advance,left,top,stride;unsigned code,width,rows;unsigned char *pixels;size_t bytes;unsigned long long used;} Glyph;
static Glyph glyph_cache[GLYPH_SLOTS],glyph_scratch;
static size_t glyph_bytes;
static unsigned long long glyph_clock;
static int glyph_cache_enabled=1;
static Glyph *load_glyph(FT_Face face,int size,unsigned c){
    if(glyph_cache_enabled)for(int i=0;i<GLYPH_SLOTS;i++){Glyph *g=&glyph_cache[i];if(g->face==face&&g->size==size&&g->code==c){g->used=++glyph_clock;return g;}}
    if(FT_Set_Pixel_Sizes(face,0,size)||FT_Load_Char(face,c,FT_LOAD_RENDER))return NULL;
    FT_GlyphSlot slot=face->glyph;size_t bytes=(size_t)slot->bitmap.width*slot->bitmap.rows;
    glyph_scratch=(Glyph){.face=face,.size=size,.code=c,.advance=(int)(slot->advance.x>>6),.left=slot->bitmap_left,.top=slot->bitmap_top,.stride=slot->bitmap.pitch,.width=slot->bitmap.width,.rows=slot->bitmap.rows,.pixels=slot->bitmap.buffer};
    if(!glyph_cache_enabled||bytes>GLYPH_BYTES||slot->bitmap.pixel_mode!=FT_PIXEL_MODE_GRAY)return &glyph_scratch;
    int chosen=-1;unsigned long long oldest=~0ULL;
    for(int i=0;i<GLYPH_SLOTS;i++)if(!glyph_cache[i].face){chosen=i;break;}else if(glyph_cache[i].used<oldest){oldest=glyph_cache[i].used;chosen=i;}
    Glyph *g=&glyph_cache[chosen];glyph_bytes-=g->bytes;free(g->pixels);memset(g,0,sizeof(*g));
    while(glyph_bytes+bytes>GLYPH_BYTES){int victim=-1;oldest=~0ULL;for(int i=0;i<GLYPH_SLOTS;i++)if(glyph_cache[i].face&&glyph_cache[i].used<oldest){oldest=glyph_cache[i].used;victim=i;}if(victim<0)return &glyph_scratch;Glyph *v=&glyph_cache[victim];glyph_bytes-=v->bytes;free(v->pixels);memset(v,0,sizeof(*v));}
    unsigned char *pixels=bytes?malloc(bytes):NULL;if(bytes&&!pixels)return &glyph_scratch;
    if(bytes)for(unsigned y=0;y<slot->bitmap.rows;y++)memcpy(pixels+(size_t)y*slot->bitmap.width,slot->bitmap.buffer+(ptrdiff_t)y*slot->bitmap.pitch,slot->bitmap.width);
    *g=glyph_scratch;g->stride=(int)g->width;g->pixels=pixels;g->bytes=bytes;g->used=++glyph_clock;glyph_bytes+=bytes;return g;
}
static int text_width(FT_Face face,int size,const char *s){int w=0;while(*s){unsigned c=glyph_code(face,next_codepoint(&s));Glyph *g=load_glyph(face,size,c);if(g)w+=g->advance;}return w;}
static void text(FT_Face face,int size,int x,int baseline,uint32_t color,const char*s,int maxwidth){
    int clipped=maxwidth>0&&text_width(face,size,s)>maxwidth;
    int limit=clipped?maxwidth-text_width(face,size,"..."):maxwidth;
    int start=x;
    while(*s){unsigned c=glyph_code(face,next_codepoint(&s));Glyph *g=load_glyph(face,size,c);if(!g)continue;if(maxwidth>0&&x-start+g->advance>limit)break;int px=x+g->left,py=baseline-g->top;
        for(unsigned yy=0;yy<g->rows;yy++)for(unsigned xx=0;xx<g->width;xx++){int dx=px+(int)xx,dy=py+(int)yy;if(dx<0||dx>=W||dy<clip_top||dy>=clip_bottom)continue;unsigned a=g->pixels[(ptrdiff_t)yy*g->stride+xx];if(a){uint32_t*dst=(uint32_t*)((char*)canvas+(size_t)dy*pitch)+dx;*dst=blend(*dst,color,a);}}
        x+=g->advance;
    }
    if(clipped&&limit>=0)text(face,size,x,baseline,color,"...",0);
}
static void centered(FT_Face f,int size,int x,int y,int w,uint32_t c,const char*s){text(f,size,x+(w-text_width(f,size,s))/2,y,c,s,w);}
static int read_line(const char *p,char *out,size_t n){FILE*f=fopen(p,"r");if(!f)return 0;if(!fgets(out,(int)n,f)){fclose(f);return 0;}fclose(f);out[strcspn(out,"\r\n")]=0;return 1;}
static int wifi_power_policy_alive(const char *path,time_t now){struct stat s;if(stat(path,&s)!=0)return 0;double age=difftime(now,s.st_mtime);return age>=-2&&age<=6;}
static long read_long(const char *p,long fallback){char s[80];return read_line(p,s,sizeof(s))?strtol(s,NULL,10):fallback;}
static long read_power_flag(const char *p){char s[8];if(!read_line(p,s,sizeof(s)))return -1;if(!strcmp(s,"0"))return 0;if(!strcmp(s,"1"))return 1;return -1;}
static long external_power(long usb,long ac){if(usb==1||ac==1)return 1;if(usb==0&&ac==0)return 0;return -1;}
static int server_identity(long *pid,unsigned long long *start){
    char path[80],line[2048],*cursor,*token;long p=read_long("/run/minecraft.pid",0);
    if(p<=1)return 0;
    snprintf(path,sizeof(path),"/proc/%ld/stat",p);
    if(!read_line(path,line,sizeof(line))||!(cursor=strrchr(line,')'))||cursor[1]!=' ')return 0;
    cursor+=2;for(int field=3;field<=22;field++){token=strsep(&cursor," ");if(!token||!*token)return 0;if(field==22){char *end;errno=0;unsigned long long value=strtoull(token,&end,10);if(errno||*end)return 0;*pid=p;*start=value;return 1;}}
    return 0;
}
static const char *battery_power_label(long online,const char *status,long charge_enabled){
    if(online==0)return "On battery";
    if(online!=1)return "Power status unavailable";
    if(!strcmp(status,"Charging"))return "Plugged in, charging";
    if(!strcmp(status,"Full"))return "Plugged in, full";
    if(!strcmp(status,"Not charging"))return charge_enabled==0?"Plugged in, charge paused":"Plugged in, not charging";
    return "Plugged in";
}
static int running(void){long pid=read_long("/run/minecraft.pid",0);return pid>1&&(kill((pid_t)pid,0)==0||errno==EPERM);}
static void uptime_str(char *s,size_t n){FILE*f=fopen("/proc/uptime","r");double secs=0;if(f){(void)fscanf(f,"%lf",&secs);fclose(f);}unsigned m=(unsigned)secs/60;snprintf(s,n,"%uh %02um",m/60,m%60);}
static void recent_logs(char lines[][160],int wanted){
    for(int i=0;i<wanted;i++)lines[i][0]=0;
    FILE*f=fopen("/var/log/minecraft.log","r");if(!f)return;fseek(f,0,SEEK_END);long len=ftell(f);if(len>24000)fseek(f,len-24000,SEEK_SET);else rewind(f);
    char buf[1024];int k=0;while(fgets(buf,sizeof(buf),f)){buf[strcspn(buf,"\r\n")]=0;if(!*buf)continue;snprintf(lines[k%wanted],160,"%.155s",buf);k++;}fclose(f);
    if(k>wanted){char tmp[12][160];for(int i=0;i<wanted;i++)memcpy(tmp[i],lines[(k+i)%wanted],160);for(int i=0;i<wanted;i++)memcpy(lines[i],tmp[i],160);}
}
static int wifi_address_valid(const char *status,const char *ip){struct in_addr parsed;return !strcmp(status,"COMPLETED")&&inet_pton(AF_INET,ip,&parsed)==1&&parsed.s_addr!=INADDR_ANY;}
static int wifi_snapshot_recent(FILE *f){struct stat st;if(fstat(fileno(f),&st)!=0)return 0;double age=difftime(time(NULL),st.st_mtime);return age>=0&&age<=45;}
static int wifi_status_available(void){FILE *f=fopen("/run/n20/wifi-state","r");if(!f)return 0;int recent=wifi_snapshot_recent(f);fclose(f);return recent;}
static void read_wifi_connection(const char *path,char *status,size_t status_size,char *ip,size_t ip_size){
    status[0]=ip[0]=0;FILE *f=fopen(path,"r");if(!f)return;
    if(wifi_snapshot_recent(f)){char line[160];while(fgets(line,sizeof(line),f)){line[strcspn(line,"\r\n")]=0;if(!strncmp(line,"IP\t",3))snprintf(ip,ip_size,"%.79s",line+3);else if(!strncmp(line,"STATE\t",6))snprintf(status,status_size,"%.79s",line+6);}}fclose(f);
}
static int fresh(void);
static void network_addr(char*s,size_t n){char status[80],ip[80];read_wifi_connection("/run/n20/wifi-state",status,sizeof(status),ip,sizeof(ip));if(!wifi_address_valid(status,ip))snprintf(s,n,"Wi-Fi address unavailable");else if(fresh()&&!strcmp(state.server,"online")&&state.server_port>0&&state.server_port<=65535)snprintf(s,n,"Wi-Fi: %.15s:%d",ip,state.server_port);else snprintf(s,n,"Wi-Fi: %.15s",ip);}
static int brightness(int value);
static void wake(void);
static void draw(void);
static int reset_display(void);
static void resync_touch(int fd);
static int bounded(int n,int lo,int hi){return n<lo?lo:n>hi?hi:n;}
static int page_offset(int offset,int count,int size){return bounded(offset,0,count?((count-1)/size)*size:0);}
static int save_display_settings(void){
    if(mkdir("/var/lib/n20",0700)<0 && errno!=EEXIST)return 0;
    FILE*f=fopen("/var/lib/n20/display.tmp","w");if(!f)return 0;
    int ok=fprintf(f,"%d %d\n",saved_brightness,idle_seconds)>0 && fflush(f)==0 && fsync(fileno(f))==0;
    if(fclose(f)!=0)ok=0;
    if(!ok||rename("/var/lib/n20/display.tmp","/var/lib/n20/display.conf")!=0){unlink("/var/lib/n20/display.tmp");return 0;}return 1;
}
/* Coalesce repeated taps so flash syncs do not delay every input event. */
static void schedule_display_save(void){display_save_due=now_ms()+1000;}
static void tick_display_save(long long now){
    if(display_save_pid){int status;pid_t done=waitpid(display_save_pid,&status,WNOHANG);if(done==display_save_pid){display_save_pid=0;if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){msg("Could not save display settings; retrying.");if(!display_save_due)display_save_due=now+5000;invalidate_full();}}}
    if(!display_save_pid&&display_save_due&&now>=display_save_due){
        pid_t pid=fork();if(pid<0){msg("Could not save display settings.");display_save_due=now+5000;invalidate_full();return;}
        if(!pid){if(drmfd>=0)close(drmfd);if(touchfd>=0)close(touchfd);if(powerfd>=0)close(powerfd);_exit(save_display_settings()?0:1);}
        display_save_pid=pid;display_save_due=0;
    }
}
static void load_display_settings(void){
    FILE*f=fopen("/var/lib/n20/display.conf","r");if(!f)return;
    int b=0,t=0;if(fscanf(f,"%d %d",&b,&t)==2 && b>=205 && b<=4095 && t>=30 && t<=600){saved_brightness=b;idle_seconds=t;}
    fclose(f);
}
static void copy_value(char *dst,size_t n,const char *src){if(!n)return;size_t len=strnlen(src,n-1);memcpy(dst,src,len);dst[len]=0;}
static int setting_value(const char *s){char *end;errno=0;long n=strtol(s,&end,10);return !errno&&end!=s&&!*end&&n>=0&&n<=10000?(int)n:-1;}
static void read_state(void){
    FILE *f=fopen("/run/n20/state","r");if(!f)return;
    State next={0};next.allow_count=next.backup_count=next.max_players=next.view_distance=next.simulation_distance=next.server_port=next.charge_stop=next.charge_resume=-1;char line[512];
    while(fgets(line,sizeof(line),f)){
        line[strcspn(line,"\r\n")]=0;char *v=strchr(line,'=');if(!v)continue;*v++=0;
        if(!strcmp(line,"server"))copy_value(next.server,sizeof(next.server),v);
        else if(!strcmp(line,"players"))next.players=bounded(atoi(v),0,10000);
        else if(!strcmp(line,"allow_count")){int count=setting_value(v);next.allow_count=count<0?-1:bounded(count,0,128);}
        else if(!strcmp(line,"backup_count")){int count=setting_value(v);next.backup_count=count<0?-1:bounded(count,0,64);}
        else if(!strcmp(line,"max_players"))next.max_players=setting_value(v);
        else if(!strcmp(line,"view_distance"))next.view_distance=setting_value(v);
        else if(!strcmp(line,"simulation_distance"))next.simulation_distance=setting_value(v);
        else if(!strcmp(line,"settings_pending"))next.settings_pending=atoi(v)!=0;
        else if(!strcmp(line,"charge_stop"))next.charge_stop=setting_value(v);
        else if(!strcmp(line,"charge_resume"))next.charge_resume=setting_value(v);
        else if(!strcmp(line,"server_port")){char *end;errno=0;long port=strtol(v,&end,10);next.server_port=!errno&&end!=v&&!*end&&port>=1&&port<=65535?(int)port:-1;}
        else if(!strcmp(line,"updated_unix"))next.updated=strtol(v,NULL,10);
        else if(!strcmp(line,"whitelist"))copy_value(next.whitelist,sizeof(next.whitelist),v);
        else if(!strcmp(line,"difficulty"))copy_value(next.difficulty,sizeof(next.difficulty),v);
        else if(!strcmp(line,"error"))copy_value(next.error,sizeof(next.error),v);
        else if(!strcmp(line,"operation"))copy_value(next.operation,sizeof(next.operation),v);
        else {
            int i=-1;char tail;
            if(sscanf(line,"player_%d%c",&i,&tail)==1&&i>=0&&i<64)copy_value(next.player[i],sizeof(next.player[i]),v);
            else if(sscanf(line,"allow_%d%c",&i,&tail)==1&&i>=0&&i<128)copy_value(next.allow[i],sizeof(next.allow[i]),v);
            else if(sscanf(line,"backup_id_%d%c",&i,&tail)==1&&i>=0&&i<64)copy_value(next.backup_id[i],sizeof(next.backup_id[i]),v);
            else if(sscanf(line,"backup_%d%c",&i,&tail)==1&&i>=0&&i<64)copy_value(next.backup[i],sizeof(next.backup[i]),v);
        }
    }
    fclose(f);next.valid=next.server[0]!=0;state=next;
}
static int fresh(void){long age=(long)time(NULL)-state.updated;return state.valid&&state.updated>0&&age>=-5&&age<30;}
static int busy(void){return helper_mutation||queued_command[0]!=0||state.operation[0]!=0;}
static int admin(const char *command,const char *arg){
    if(helper_pid){if(!helper_mutation&&strcmp(command,"state")&& !queued_command[0]){copy_value(queued_command,sizeof(queued_command),command);copy_value(queued_arg,sizeof(queued_arg),arg?arg:"");msg("Request queued after refresh.");return 1;}msg("Please wait for the current operation.");return 0;}
    pid_t pid=fork();if(pid<0){msg("Could not launch server helper.");return 0;}
    if(pid==0){
        if(drmfd>=0)close(drmfd);
        int nullfd=open("/dev/null",O_RDWR);if(nullfd>=0){dup2(nullfd,STDIN_FILENO);dup2(nullfd,STDOUT_FILENO);dup2(nullfd,STDERR_FILENO);if(nullfd>2)close(nullfd);}
        execl("/usr/local/sbin/n20-admin","n20-admin",command,arg,(char *)NULL);_exit(127);
    }
    helper_pid=pid;helper_mutation=strcmp(command,"state")!=0;
    if(helper_mutation)msg("Request sent. Waiting for confirmation...");
    return 1;
}
/* Watch the directory rather than a file: state writers replace files atomically. */
static int start_state_watch(const char *path){
    if(state_watch_fd>=0)return 1;
    int fd=inotify_init1(IN_NONBLOCK|IN_CLOEXEC);if(fd<0)return 0;
    if(inotify_add_watch(fd,path,IN_MOVED_TO|IN_CLOSE_WRITE|IN_DELETE|IN_DELETE_SELF|IN_MOVE_SELF)<0){close(fd);return 0;}
    state_watch_fd=fd;return 1;
}
static void stop_state_watch(void){if(state_watch_fd>=0)close(state_watch_fd);state_watch_fd=-1;}
static void drain_state_watch(void){
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));ssize_t n;
    while(state_watch_fd>=0&&(n=read(state_watch_fd,buf,sizeof(buf)))>0){
        for(char *p=buf;p<buf+n;){struct inotify_event *e=(struct inotify_event *)p;p+=sizeof(*e)+e->len;
            if(e->mask&(IN_Q_OVERFLOW|IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF)){stop_state_watch();state_read_due=0;return;}
            if(e->len&&!strcmp(e->name,"state"))state_read_due=0;
            if(e->len&&!strcmp(e->name,"wifi-state")&&(page==HOME||page==WIFI))invalidate_full();
        }
    }
}static void tick_helper(long long now){
    if(awake&&state_watch_fd<0)start_state_watch("/run/n20");
    if(awake&&now>=state_read_due){State previous=state;read_state();previous.updated=state.updated;if(memcmp(&previous,&state,sizeof(state)))invalidate_full();state_read_due=now+(state_watch_fd>=0?15000:1000);}
    if(helper_pid){int status=0;pid_t done=waitpid(helper_pid,&status,WNOHANG);if(done==helper_pid){
        if((!WIFEXITED(status)||WEXITSTATUS(status)!=0)&&helper_mutation)msg("Request failed. Check server log.");
        int changed=helper_mutation;helper_pid=0;helper_mutation=0;State previous=state;read_state();previous.updated=state.updated;if(changed){helper_due=0;invalidate_full();}else if(memcmp(&previous,&state,sizeof(state)))invalidate_full();
        if(queued_command[0]){char command[sizeof(queued_command)],arg[sizeof(queued_arg)];copy_value(command,sizeof(command),queued_command);copy_value(arg,sizeof(arg),queued_arg);queued_command[0]=0;queued_arg[0]=0;admin(command,arg[0]?arg:NULL);}
    }}
    if(awake&&!helper_pid&&now>=helper_due){admin("state",NULL);helper_due=now+5000;}
}
static void pixel_block(int x,int y,int scale){
    rect(x,y,8*scale,8*scale,0x5D4430);rect(x,y,8*scale,2*scale,0x629D45);
    rect(x+scale,y+2*scale,2*scale,scale,0x426F32);rect(x+5*scale,y+2*scale,2*scale,2*scale,0x426F32);
    rect(x+scale,y+5*scale,2*scale,scale,0x856344);rect(x+5*scale,y+6*scale,scale,scale,0x392B22);
}
static void card(int x,int y,int w,int h){rect(x,y,w,h,PANEL);rect(x,y,w,4,BORDER);rect(x,y+h-4,w,4,0x080D0B);}
static void add_hit(int x,int y,int w,int h,int action,int value,const char *arg){if(action&&hit_count<100){Hit *hit=&hits[hit_count++];*hit=(Hit){.x=x,.y=y,.w=w,.h=h,.action=action,.value=value};copy_value(hit->arg,sizeof(hit->arg),arg?arg:"");}}
static int control_font_size(int width,const char *label){int size=31;while(size>20&&text_width(bold,size,label)>width-16)size--;return size;}
static void control(int x,int y,int w,int h,uint32_t bg,uint32_t fg,const char *label,int action,int value,const char *arg){
    rect(x,y,w,h,bg);rect(x,y,w,4,blend(bg,WHITE,45));rect(x,y,4,h,blend(bg,WHITE,25));rect(x,y+h-6,w,6,blend(bg,BG,110));rect(x+w-4,y,4,h,blend(bg,BG,90));
    centered(bold,control_font_size(w,label),x,y+h/2+11,w,fg,label);
    add_hit(x,y,w,h,action,value,arg);
}
static void action_button(int x,int y,int w,int h,const char *label,int action,int value,const char *arg){
    int enabled=fresh()&&!busy();control(x,y,w,h,enabled?GREEN_DARK:0x151A17,enabled?GREEN:DIM,label,enabled?action:0,value,arg);
}
static int access_editable(void){return fresh()&&!busy()&&!strcmp(state.server,"online")&&state.allow_count>=0;}
static const char *access_unavailable_reason(void){if(!fresh())return "Waiting for current server data.";if(busy())return "Waiting for the current operation.";if(state.allow_count<0)return "Whitelist data unavailable.";return "Start the server to edit access.";}
static int start_ready(void){return fresh()&&!busy()&&!running()&&!strcmp(state.server,"offline")&&!strstr(state.error,"Battery low")&&!strstr(state.error,"Clock invalid")&&!strstr(state.error,"Server files unavailable");}
static int confirmation_ready(int c){
    if(!fresh()||busy())return 0;
    if(c==1||c==2){long pid;unsigned long long start;return server_identity(&pid,&start)&&pid==confirm_server_pid&&start==confirm_server_start;}
    if(c==3){if(!access_editable())return 0;for(int i=0;i<state.allow_count&&i<128;i++)if(!strcasecmp(state.allow[i],selected_name))return 1;return 0;}
    if(c==6)return access_editable()&&!strcmp(state.whitelist,"off")&&state.allow_count==0;
    if(c==7)return access_editable()&&!strcmp(state.whitelist,"on");
    if(c==4||c==5){if(!strcmp(state.server,"recovery")||strstr(state.error,"Battery low"))return 0;if(c==5){for(int i=0;i<state.backup_count&&i<64;i++)if(!strcmp(state.backup_id[i],selected_backup))return 1;return 0;}return 1;}
    return 0;
}
static void heading(const char *title){text(bold,58,48,290,WHITE,title,980);}
static void divider(int y){rect(48,y,984,2,BORDER);}
static void avatar(int x,int y,const char *name){
    unsigned hash=5381;for(const char *p=name;*p;p++)hash=hash*33+(unsigned char)*p;
    uint32_t c=0x4A633D+((hash&31)<<16)+((hash&15)<<8);
    rect(x,y,72,72,c);rect(x+12,y+25,13,13,BG);rect(x+47,y+25,13,13,BG);rect(x+25,y+45,22,12,BG);
}
static void player_row(int y,const char *name,int whitelist_row){
    avatar(60,y+20,name);text(bold,36,160,y+66,WHITE,name,560);
    if(page==PLAYERS&&whitelist_row)control(770,y+6,260,106,access_editable()?GREEN_DARK:PANEL,access_editable()?GREEN:DIM,"REMOVE ACCESS",access_editable()?A_REMOVE:0,0,name);
    divider(y+122);
}
static void sleep_duration_label(int seconds,char *out,size_t size){if(seconds<60)snprintf(out,size,"%d s",seconds);else if(seconds%60)snprintf(out,size,"%d min %d s",seconds/60,seconds%60);else snprintf(out,size,"%d min",seconds/60);}
static void stepper(int y,const char *label,int value,int action,const char *suffix){
    char b[80];text(regular,33,48,y+55,WHITE,label,480);if(value<0)snprintf(b,sizeof(b),"--");else if(action==A_TIMEOUT)sleep_duration_label(value,b,sizeof(b));else snprintf(b,sizeof(b),"%d%s",value,suffix?suffix:"");
    centered(mono,34,540,y+55,240,GREEN,b);if(action==A_BRIGHT||action==A_TIMEOUT){int down=action==A_BRIGHT?saved_brightness>205:idle_seconds>30,up=action==A_BRIGHT?saved_brightness<4095:idle_seconds<600;control(800,y,100,104,down?GREEN_DARK:PANEL,down?GREEN:DIM,"-",down?action:0,-1,NULL);control(930,y,100,104,up?GREEN_DARK:PANEL,up?GREEN:DIM,"+",up?action:0,1,NULL);}else{int lo=action==A_MAX?1:2,hi=action==A_MAX?20:12,enable=fresh()&&!busy()&&value>=lo&&value<=hi;control(800,y,100,104,enable&&value>lo?GREEN_DARK:PANEL,enable&&value>lo?GREEN:DIM,"-",enable&&value>lo?action:0,-1,NULL);control(930,y,100,104,enable&&value<hi?GREEN_DARK:PANEL,enable&&value<hi?GREEN:DIM,"+",enable&&value<hi?action:0,1,NULL);}
}
static int valid_backup_id(const char *s){size_t n=strlen(s);if(n<8||n>80)return 0;for(;*s;s++)if(!((*s>='0'&&*s<='9')||(*s>='a'&&*s<='f')||(*s>='A'&&*s<='F')))return 0;return 1;}
static void backup_space_label(const char *path,char *label,size_t size){struct statvfs info;if(statvfs(path,&info)!=0){snprintf(label,size,"Local storage: space unavailable");return;}double gib=(double)info.f_bavail*(double)info.f_frsize/(1024.0*1024.0*1024.0);snprintf(label,size,"Local storage: %.1f GiB free",gib);}
static int wifi_connected_network(const char *status,const char *current,const char *network){return !strcmp(status,"COMPLETED")&&current[0]&&!strcmp(current,network);}
static void wifi_ssh_label(const char *status,const char *ip,char *out,size_t size){if(wifi_address_valid(status,ip))snprintf(out,size,"%s:22",ip);else snprintf(out,size,"Wi-Fi unavailable");}
static int wifi_send_to(const char *path,const char *command){
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return 0;
    struct sockaddr_un addr={.sun_family=AF_UNIX};
    if(strlen(path)>=sizeof(addr.sun_path)){close(fd);return 0;}
    snprintf(addr.sun_path,sizeof(addr.sun_path),"%s",path);
    int ok=sendto(fd,command,strlen(command),0,(struct sockaddr*)&addr,sizeof(addr))>=0;close(fd);return ok;
}
static int wifi_send(const char *command){return wifi_send_to("/run/n20/wifi-control",command);}
static int (*wifi_sender)(const char *)=wifi_send;
static void hex_encode(const char *src,char *out,size_t cap){const char *hex="0123456789abcdef";size_t i=0;while(src[i]&&i*2+2<cap){unsigned char c=(unsigned char)src[i];out[2*i]=hex[c>>4];out[2*i+1]=hex[c&15];i++;}out[2*i]=0;}
static int hex_decode(const char *hex,char *out,size_t cap){size_t n=strlen(hex);if(n%2||n/2>=cap)return 0;for(size_t i=0;i<n/2;i++){unsigned v;if(sscanf(hex+2*i,"%2x",&v)!=1)return 0;out[i]=(char)v;}out[n/2]=0;return 1;}
static int wifi_connect(const char *pass,const char *kind){char passhex[131],cmd[260];hex_encode(pass,passhex,sizeof(passhex));snprintf(cmd,sizeof(cmd),"CONNECT %s %s %s",wifi_selected,*passhex?passhex:"00",kind);int ok=wifi_sender(cmd);if(ok)msg("Connecting... Check the Wi-Fi page.");else msg("Wi-Fi service unavailable.");explicit_bzero(passhex,sizeof(passhex));explicit_bzero(cmd,sizeof(cmd));return ok;}
static int wifi_password_valid(const char *password){
    size_t n=strlen(password);if(n<8||n>64)return 0;if(n<64)return 1;
    for(size_t i=0;i<n;i++)if(!((password[i]>='0'&&password[i]<='9')||(password[i]>='a'&&password[i]<='f')||(password[i]>='A'&&password[i]<='F')))return 0;
    return 1;
}
/* The keyboard accepts ASCII. Keep the insertion caret visible when scrolling. */
static void wifi_password_window(const char *input,size_t cursor,char *out,size_t cap){
    char raw[67],candidate[80];size_t n=strlen(input);if(cursor>n)cursor=n;
    memcpy(raw,input,cursor);raw[cursor]='|';memcpy(raw+cursor+1,input+cursor,n-cursor+1);
    size_t left=0,right=n+1;
    for(;;){
        snprintf(candidate,sizeof(candidate),"%s%.*s%s",left?"...":"",(int)(right-left),raw+left,right<n+1?"...":"");
        if(text_width(mono,40,candidate)<=650||right-left<=1)break;
        if(left<cursor)left++;else right--;
    }
    if(cap)snprintf(out,cap,"%s",candidate);
}
static void draw_wifi_keyboard(void){
    rect(0,0,W,H,BG);heading("Wi-Fi password");text(regular,30,48,350,MUTED,wifi_label,984);
    char masked[66],visible[80],hint[128];size_t len=strlen(wifi_password);memset(masked,'*',len);masked[len]=0;
    wifi_password_window(wifi_show_password?wifi_password:masked,wifi_cursor,visible,sizeof(visible));
    snprintf(hint,sizeof(hint),"%zu / 64  |  %s",len,len<8?"Enter at least 8 characters.":!wifi_password_valid(wifi_password)?"64 characters must be hexadecimal.":"Password ready to connect.");
    card(48,405,984,130);text(mono,40,76,490,WHITE,visible,650);control(760,425,240,90,PANEL,MUTED,wifi_show_password?"HIDE":"SHOW",A_WIFI_SHOW,0,NULL);text(regular,25,48,575,MUTED,hint,984);
    const char *lower[]={"1234567890","qwertyuiop","asdfghjkl","zxcvbnm"};
    const char *upper[]={"1234567890","QWERTYUIOP","ASDFGHJKL","ZXCVBNM"};
    const char *symbols[]={"!@#$%^&*()","-_=+[]{};:","'\",.<>/?\\|","`~"};
    for(int row=0;row<4;row++){
        const char *keys=wifi_keyboard_mode==0?lower[row]:wifi_keyboard_mode==1?upper[row]:symbols[row];
        int count=(int)strlen(keys),start=48+(984-(count*99-9))/2;
        for(int i=0;i<count;i++){char key[2]={keys[i],0};control(start+i*99,620+row*145,90,130,PANEL,WHITE,key,A_KEY,(unsigned char)keys[i],NULL);}
    }
    control(48,1220,300,120,PANEL,WHITE,wifi_keyboard_mode==0?"ABC":wifi_keyboard_mode==1?"SYMBOLS":"abc",A_SHIFT,0,NULL);
    control(366,1220,348,120,PANEL,WHITE,"SPACE",A_WIFI_SPACE,0,NULL);control(732,1220,300,120,PANEL,WHITE,"DELETE",A_ERASE,0,NULL);
    control(48,1370,477,110,PANEL,wifi_cursor?WHITE:DIM,"< CURSOR",wifi_cursor?A_WIFI_LEFT:0,0,NULL);
    control(555,1370,477,110,PANEL,wifi_cursor<len?WHITE:DIM,"CURSOR >",wifi_cursor<len?A_WIFI_RIGHT:0,0,NULL);
    control(48,1520,984,130,GREEN_DARK,GREEN,"CONNECT",A_SUBMIT,0,NULL);control(48,1690,984,130,PANEL,MUTED,"CANCEL",A_CANCEL,0,NULL);
    if(message_until>now_ms())text(regular,28,48,1880,AMBER,message,984);
    if(state.error[0]){text(regular,25,48,2040,RED,state.error,730);control(800,1990,232,90,PANEL,AMBER,"DETAILS",A_NOTICE,0,NULL);}
}
static void draw_keyboard(void){
    if(keyboard_open==2){draw_wifi_keyboard();return;}
    rect(0,0,W,H,BG);heading("Add to whitelist");text(regular,30,48,360,MUTED,"Minecraft username",960);
    card(48,402,984,140);text(mono,45,76,490,WHITE,typed_name[0]?typed_name:"_",920);
    const char *keys=keyboard_shift?"1234567890QWERTYUIOPASDFGHJKL_ZXCVBNM":"1234567890qwertyuiopasdfghjkl_zxcvbnm";
    for(int i=0;keys[i];i++){char key[2]={keys[i],0};control(48+(i%10)*99,660+(i/10)*150,90,132,PANEL,WHITE,key,A_KEY,keys[i],NULL);}
    control(48,1290,477,128,PANEL,WHITE,keyboard_shift?"LOWERCASE":"UPPERCASE",A_SHIFT,0,NULL);
    control(555,1290,477,128,PANEL,WHITE,"DELETE",A_ERASE,0,NULL);
    action_button(48,1510,984,140,"ADD PLAYER",A_SUBMIT,0,NULL);control(48,1690,984,130,PANEL,MUTED,"CANCEL",A_CANCEL,0,NULL);
    if(message_until>now_ms())text(regular,28,48,1930,AMBER,message,984);
    if(state.error[0]){text(regular,25,48,2100,RED,state.error,730);control(800,2050,232,90,PANEL,AMBER,"DETAILS",A_NOTICE,0,NULL);}
}
static void draw_confirmation(void){
    rect(0,0,W,H,BG);pixel_block(48,230,10);
    const char *title=confirm==1?"Restart server?":confirm==2?"Stop server?":confirm==3?"Remove player?":confirm==4?"Create backup?":confirm==6?"Enable empty whitelist?":confirm==7?"Disable whitelist?":"Restore backup?";
    text(bold,52,48,460,WHITE,title,984);
    if(confirm==3){text(mono,40,48,590,GREEN,selected_name,984);text(regular,31,48,690,MUTED,"This removes their whitelist access.",984);}
    else if(confirm==6){text(regular,31,48,590,RED,"An empty whitelist blocks everyone.",984);text(regular,30,48,665,MUTED,"No player names have been allowed yet.",984);text(regular,30,48,740,MUTED,"Add players first, or enable it anyway.",984);}
    else if(confirm==7){text(regular,31,48,590,RED,"Players will no longer need whitelist access.",984);text(regular,30,48,665,MUTED,"Anyone who can reach the server may try to join.",984);}
    else if(confirm==5){text(regular,32,48,590,RED,"The current world will be replaced.",984);text(regular,30,48,665,MUTED,"The server will restart; players disconnect.",984);text(regular,28,48,760,MUTED,"Selected backup",984);text(regular,30,48,815,WHITE,selected_backup_label,984);text(mono,23,48,870,DIM,selected_backup,984);}
    else if(confirm==4){text(regular,31,48,590,MUTED,"Copies the last files saved to disk.",984);text(regular,31,48,665,MUTED,"The server keeps running.",984);text(regular,31,48,740,MUTED,"Unsaved changes may not be included.",984);}
    else {text(regular,31,48,590,MUTED,"The world will be saved first.",984);text(regular,31,48,665,MUTED,"Players will be disconnected.",984);}
    int ready=confirmation_ready(confirm);control(48,1100,984,144,ready?(confirm==1||confirm==4?GREEN_DARK:RED_DARK):PANEL,ready?WHITE:DIM,confirm==7?"DISABLE WHITELIST":confirm==6?"ENABLE EMPTY WHITELIST":confirm==5?"RESTORE SELECTED BACKUP":confirm==4?"BACK UP SAVED FILES":confirm==3?"REMOVE ACCESS":confirm==1?"RESTART SERVER":"STOP SERVER",ready?A_CONFIRM:0,0,NULL);
    control(48,1290,984,144,PANEL,WHITE,"CANCEL",A_CANCEL,0,NULL);
    if(!ready)text(regular,27,48,1030,AMBER,!fresh()?"Waiting for current server data.":busy()?"Waiting for the current operation.":"State changed. Cancel and review again.",984);
    if(message_until>now_ms())text(regular,27,48,1650,AMBER,message,984);
    if(state.error[0]){text(regular,25,48,1510,RED,state.error,730);control(800,1460,232,90,PANEL,AMBER,"DETAILS",A_NOTICE,0,NULL);}
}
static void draw_difficulty(void){
    static const char *levels[]={"PEACEFUL","EASY","NORMAL","HARD"};
    static const char *values[]={"peaceful","easy","normal","hard"};
    heading("Difficulty");
    text(regular,30,48,360,MUTED,"Choose a difficulty for your world.",984);
    for(int i=0;i<4;i++){
        int selected=!strcmp(state.difficulty,values[i]);
        control(48,430+i*150,984,120,selected?GREEN_DARK:PANEL,(fresh()&&!busy())?(selected?GREEN:WHITE):DIM,levels[i],fresh()&&!busy()?A_DIFFICULTY:0,i,NULL);
    }
    control(48,1120,984,130,PANEL,MUTED,"CANCEL",A_CANCEL,0,NULL);
    if(message_until>now_ms())text(regular,27,48,1350,AMBER,message,984);
}
static void wrapped_notice(const char *s,int y,uint32_t color){
    while(*s&&y<1500){
        char line[160];size_t len=0,last_space=0;const char *cursor=s;
        while(*cursor){
            const char *next=cursor;next_codepoint(&next);size_t part=(size_t)(next-cursor);
            if(len+part>=sizeof(line))break;
            memcpy(line+len,cursor,part);line[len+part]=0;
            if(text_width(regular,31,line)>950&&len)break;
            len+=part;if(*cursor==' ')last_space=len;cursor=next;
        }
        if(*cursor&&last_space){len=last_space;cursor=s+last_space;}
        if(!len)break;
        line[len]=0;text(regular,31,48,y,color,line,984);y+=55;s=cursor;
        while(*s==' ')s++;
    }
}
static void draw_notice(void){
    heading("Server status");int y=370;
    if(state.error[0]){text(bold,28,48,y,RED,"NEEDS ATTENTION",984);wrapped_notice(state.error,y+65,WHITE);y=900;}
    if(state.operation[0]){text(bold,28,48,y,GREEN,"CURRENT OPERATION",984);wrapped_notice(state.operation,y+65,MUTED);}
    if(message_until>now_ms()&&(state.error[0]||state.operation[0])){text(bold,28,48,1550,AMBER,"LAST ACTION",984);text(regular,27,48,1610,WHITE,message,984);}
    if(!state.error[0]&&!state.operation[0])wrapped_notice(message_until>now_ms()?message:!fresh()?"Waiting for current server data.":"No current alerts.",y,WHITE);
    control(48,1690,984,130,PANEL,WHITE,"CLOSE",A_CANCEL,0,NULL);
}
static void draw(void){
    int active=running();last_server=active;last_fresh=fresh();last_message_visible=message_until>now_ms();hit_count=0;rect(0,0,W,H,BG);
    if(notice_open){draw_notice();return;}if(confirm){draw_confirmation();return;}if(keyboard_open){draw_keyboard();return;}if(difficulty_open){draw_difficulty();return;}
    pixel_block(48,65,7);text(bold,39,130,111,WHITE,"MINOS",650);
    const char *status=!fresh()?"UNKNOWN":!strcmp(state.server,"online")?"ONLINE":!strcmp(state.server,"starting")?"STARTING":!strcmp(state.server,"recovery")?"RECOVERY":"OFFLINE";
    uint32_t status_color=fresh()&&!strcmp(state.server,"online")?GREEN:AMBER;
    text(bold,26,800,106,status_color,status,245);divider(154);
    if(page==LOGS||page==BACKUPS||page==WIFI)control(800,185,232,90,PANEL,MUTED,"< TOOLS",A_PAGE,TOOLS,NULL);
    if(page==SERVER_SETTINGS||page==DEVICE_SETTINGS)control(760,185,272,90,PANEL,MUTED,"< SETTINGS",A_PAGE,SETTINGS,NULL);
    if(page==HOME){
        heading("Server overview");char address[100];network_addr(address,sizeof(address));text(mono,32,48,360,MUTED,address,984);
        card(48,430,984,540);
        int player_data=fresh()&&!strcmp(state.server,"online");char title[100];if(player_data&&(state.settings_pending||state.max_players<1))snprintf(title,sizeof(title),"Players online  %d",state.players);else if(player_data)snprintf(title,sizeof(title),"Players  %d / %d",state.players,state.max_players);else snprintf(title,sizeof(title),"Players unavailable");text(bold,42,72,505,WHITE,title,920);
        if(player_data&&state.players==0)text(regular,34,72,615,MUTED,"No players online",920);
        if(player_data)for(int i=0;i<state.players&&i<3;i++){if(state.player[i][0])player_row(550+i*130,state.player[i],0);else {text(regular,32,72,615+i*130,MUTED,"Player names unavailable",920);break;}}
        long bat=read_long("/sys/class/power_supply/battery/capacity",-1),temp=read_long("/sys/class/power_supply/battery/temp",-1),usb=read_power_flag("/sys/class/power_supply/usb/online"),ac=read_power_flag("/sys/class/power_supply/ac/online"),charge_enabled=read_power_flag("/sys/class/oplus_chg/battery/mmi_charging_enable");char u[40],health[160],charge[32]="";uptime_str(u,sizeof(u));read_line("/sys/class/power_supply/battery/status",charge,sizeof(charge));
        char battery_text[24],temperature_text[24];if(bat>=0&&bat<=100)snprintf(battery_text,sizeof(battery_text),"%ld%%",bat);else copy_value(battery_text,sizeof(battery_text),"--");if(temp>=0)snprintf(temperature_text,sizeof(temperature_text),"%.1f C",temp/10.0);else copy_value(temperature_text,sizeof(temperature_text),"--");card(48,1070,984,205);snprintf(health,sizeof(health),"Battery %s    %s",battery_text,battery_power_label(external_power(usb,ac),charge,charge_enabled));text(regular,27,72,1130,WHITE,health,920);snprintf(health,sizeof(health),"%s    Uptime %s",temperature_text,u);text(regular,27,72,1190,MUTED,health,920);if(fresh()&&state.charge_resume>=0&&state.charge_stop<=100&&state.charge_resume<state.charge_stop)snprintf(health,sizeof(health),"Charge target %d%% / resume at %d%%",state.charge_stop,state.charge_resume);else snprintf(health,sizeof(health),"Charge target unavailable");text(regular,25,72,1245,MUTED,health,920);
        if(fresh()&&!strcmp(state.server,"recovery"))control(48,1320,984,144,PANEL,AMBER,"RECOVERY REQUIRED",0,0,NULL);
        else if(fresh()&&strstr(state.error,"Battery low"))control(48,1320,984,144,PANEL,AMBER,"WAITING FOR CHARGE",0,0,NULL);
        else if(fresh()&&strstr(state.error,"Server files unavailable"))control(48,1320,984,144,PANEL,AMBER,"SERVER FILES UNAVAILABLE",A_NOTICE,0,NULL);
        else if(fresh()&&strstr(state.error,"Clock invalid"))control(48,1320,984,144,PANEL,AMBER,"WAITING FOR TRUSTED TIME",A_NOTICE,0,NULL);
        else if(fresh()&&!active&&!strcmp(state.server,"starting"))control(48,1320,984,144,PANEL,AMBER,"SERVER STARTING",0,0,NULL);
        else if(active){action_button(48,1320,477,144,"RESTART",A_RESTART,0,NULL);control(555,1320,477,144,(!fresh()||busy())?PANEL:RED_DARK,(!fresh()||busy())?DIM:RED,"STOP",(!fresh()||busy())?0:A_STOP,0,NULL);}
        else {int ready=start_ready();control(48,1320,984,144,ready?GREEN_DARK:PANEL,ready?GREEN:DIM,"START SERVER",ready?A_START:0,0,NULL);}
        text(bold,26,48,1565,GREEN,"LATEST LOCAL BACKUP",984);
        text(regular,27,48,1620,MUTED,!fresh()||state.backup_count<0?"Backup status unavailable":state.backup_count?state.backup[0]:"No saved backups",984);
    }else if(page==PLAYERS){
        heading("Players");control(48,355,477,120,list_mode?PANEL:GREEN_DARK,list_mode?MUTED:GREEN,"ONLINE",A_LIST,0,NULL);control(555,355,477,120,list_mode?GREEN_DARK:PANEL,list_mode?GREEN:MUTED,"WHITELIST",A_LIST,1,NULL);
        int list_available=fresh()&&(list_mode?state.allow_count>=0:!strcmp(state.server,"online"));int count=list_mode?bounded(state.allow_count,0,128):bounded(state.players,0,64);list_offset=page_offset(list_offset,count,9);char count_text[80];snprintf(count_text,sizeof(count_text),"%d %s  |  page %d / %d",count,list_mode?"allowed":"online",list_offset/9+1,count?(count-1)/9+1:1);text(regular,31,48,550,MUTED,list_available?count_text:list_mode?"Whitelist data unavailable":"Player data unavailable",984);
        if(list_available)for(int i=list_offset;i<count&&i<list_offset+9;i++){const char *name=list_mode?state.allow[i]:state.player[i];if(name[0])player_row(595+(i-list_offset)*132,name,list_mode);else if(!list_mode){text(regular,32,48,660+(i-list_offset)*132,MUTED,"Player names unavailable",984);break;}}
        if(list_available&&!count)text(regular,32,48,685,MUTED,list_mode?"No players on the whitelist":"No players online",984);
        control(48,1845,230,110,PANEL,list_offset?MUTED:DIM,"PREV",list_offset?A_SCROLL:0,-9,NULL);control(308,1845,230,110,PANEL,list_offset+9<count?MUTED:DIM,"NEXT",list_offset+9<count?A_SCROLL:0,9,NULL);
        if(list_mode){control(568,1845,464,110,access_editable()?GREEN_DARK:PANEL,access_editable()?GREEN:DIM,"ADD TO WHITELIST",access_editable()?A_ADD:0,0,NULL);
            char wl[80];snprintf(wl,sizeof(wl),"Whitelist: %s",state.whitelist[0]?state.whitelist:"unknown");text(regular,30,48,2045,MUTED,wl,550);
            int toggle_ready=access_editable()&&(!strcmp(state.whitelist,"on")||!strcmp(state.whitelist,"off"));control(650,1985,382,110,toggle_ready?GREEN_DARK:PANEL,toggle_ready?GREEN:DIM,!strcmp(state.whitelist,"on")?"TURN OFF":!strcmp(state.whitelist,"off")?"TURN ON":"UNAVAILABLE",toggle_ready?A_ALLOW:0,1,NULL);
            if(!access_editable())text(regular,24,48,2100,DIM,access_unavailable_reason(),600);
        }
    }else if(page==SETTINGS){
        heading("Settings");
        control(48,400,984,160,PANEL,GREEN,"SERVER SETTINGS  >",A_PAGE,SERVER_SETTINGS,NULL);
        control(48,600,984,160,PANEL,GREEN,"DEVICE SETTINGS  >",A_PAGE,DEVICE_SETTINGS,NULL);
    }else if(page==DEVICE_SETTINGS){
        heading("Device settings");
        stepper(435,"Brightness",bounded(saved_brightness*100/4095,1,100),A_BRIGHT,"%");
        stepper(575,"Sleep after",idle_seconds,A_TIMEOUT," s");
    }else if(page==SERVER_SETTINGS){
        heading("Server settings");
        text(regular,32,48,500,WHITE,"Difficulty",550);action_button(650,435,382,115,state.difficulty[0]?state.difficulty:"unknown",A_DIFFICULTY,-1,NULL);
        stepper(615,"Max players",state.max_players,A_MAX,NULL);stepper(755,"View distance",state.view_distance,A_VIEW," chunks");stepper(895,"Simulation distance",state.simulation_distance,A_SIM," chunks");
        if(state.settings_pending)text(regular,27,48,1080,AMBER,"Restart required to apply settings.",984);
    }else if(page==TOOLS){
        heading("Tools");
        list_offset=page_offset(list_offset,TOOL_COUNT,4);
        for(int i=list_offset;i<TOOL_COUNT&&i<list_offset+4;i++){
            int y=430+(i-list_offset)*320;const ToolEntry *entry=&tool_entries[i];
            card(48,y,984,275);text(bold,39,80,y+92,WHITE,entry->title,780);
            text(regular,28,80,y+160,MUTED,entry->description,780);
            text(bold,46,940,y+145,GREEN,">",60);add_hit(48,y,984,275,A_PAGE,entry->page,NULL);
        }
        if(TOOL_COUNT>4){control(48,1845,477,110,PANEL,list_offset?MUTED:DIM,"PREV",list_offset?A_SCROLL:0,-4,NULL);control(555,1845,477,110,PANEL,list_offset+4<TOOL_COUNT?MUTED:DIM,"NEXT",list_offset+4<TOOL_COUNT?A_SCROLL:0,4,NULL);}
    }else if(page==WIFI){
        heading("Wi-Fi");FILE*f=fopen("/run/n20/wifi-state","r");char line[512],status[80]="Starting",ip[64]="",current[80]="",note[120]="";int count=0,shown=0,available=f&&wifi_snapshot_recent(f);
        if(available){while(fgets(line,sizeof(line),f)){line[strcspn(line,"\r\n")]=0;char *cursor=line,*type=strsep(&cursor,"\t"),*value=strsep(&cursor,"\t");if(!value)continue;
            if(!strcmp(type,"STATE"))snprintf(status,sizeof(status),"%.79s",value);
            else if(!strcmp(type,"IP"))snprintf(ip,sizeof(ip),"%.63s",value);
            else if(!strcmp(type,"CURRENT"))hex_decode(value,current,sizeof(current));
            else if(!strcmp(type,"MESSAGE"))snprintf(note,sizeof(note),"%.119s",value);
            else if(!strcmp(type,"NETWORK")){char *kind=strsep(&cursor,"\t"),*signal=strsep(&cursor,"\t");if(!kind||!signal)continue;int idx=count++;if(idx<list_offset||shown>=8)continue;char name[80],arg[96],detail[80];if(!hex_decode(value,name,sizeof(name)))continue;
                int y=650+shown*135;snprintf(arg,sizeof(arg),"%.64s",value);snprintf(detail,sizeof(detail),"%.20s  %s dBm",kind,signal);text(bold,30,58,y+40,WHITE,name,660);text(regular,23,58,y+78,MUTED,detail,660);int supported=!strcmp(kind,"OPEN")?1:!strcmp(kind,"WPA2")?2:0;int connected=wifi_connected_network(status,current,name);control(760,y,270,100,PANEL,connected?MUTED:supported?GREEN:DIM,connected?"CONNECTED":supported?"CONNECT":"UNSUPPORTED",supported&&!connected?A_WIFI_SELECT:0,supported,arg);divider(y+113);shown++;}}
            }if(f)fclose(f);
        wifi_count=count;int clamped=page_offset(list_offset,count,8);if(clamped!=list_offset){list_offset=clamped;draw();return;}char pages[64];snprintf(pages,sizeof(pages),"Networks  |  page %d / %d",list_offset/8+1,count?(count-1)/8+1:1);text(regular,24,48,620,DIM,pages,570);
        int auto_power=wifi_power_policy_alive("/run/n20/wifi-power-policy.heartbeat",time(NULL));text(regular,22,700,620,auto_power?GREEN:DIM,auto_power?"Power policy: auto":"Power policy: unknown",332);
        char top[180];snprintf(top,sizeof(top),"%s%s%s",available?(current[0]?current:"Not connected"):"Wi-Fi status unavailable",ip[0]?"  ":"",ip);text(regular,30,48,355,WHITE,top,984);text(regular,26,48,410,MUTED,available?(note[0]?note:status):"Waiting for Wi-Fi service.",984);
        if(!count)text(regular,27,48,700,MUTED,!available?"Wi-Fi service unavailable.":strstr(note,"Scanning")?"Scanning for networks...":"No networks found.",984);
        control(48,465,477,110,PANEL,GREEN,"SCAN",A_WIFI_SCAN,0,NULL);control(555,465,477,110,PANEL,current[0]?MUTED:DIM,"DISCONNECT",current[0]?A_WIFI_DISCONNECT:0,0,NULL);
        control(48,1850,477,110,PANEL,list_offset?MUTED:DIM,"PREV",list_offset?A_SCROLL:0,-8,NULL);control(555,1850,477,110,PANEL,list_offset+8<count?MUTED:DIM,"NEXT",list_offset+8<count?A_SCROLL:0,8,NULL);
        char sshline[120];wifi_ssh_label(status,ip,sshline,sizeof(sshline));text(regular,25,48,2040,DIM,"SSH over Wi-Fi",450);text(mono,29,480,2040,MUTED,sshline,552);
    }else if(page==BACKUPS){
        list_offset=page_offset(list_offset,bounded(state.backup_count,0,64),8);heading("Backups");char space[80];backup_space_label("/srv",space,sizeof(space));text(regular,26,48,350,WHITE,space,984);text(regular,25,48,395,MUTED,"Snapshots may omit unsaved changes.",984);
        control(48,420,984,125,confirmation_ready(4)?GREEN_DARK:PANEL,confirmation_ready(4)?GREEN:DIM,"CREATE BACKUP",confirmation_ready(4)?A_BACKUP:0,0,NULL);
        char pages[64];snprintf(pages,sizeof(pages),"Page %d / %d",list_offset/8+1,state.backup_count>0?(state.backup_count-1)/8+1:1);text(regular,24,48,590,DIM,pages,984);
        if(!fresh()||state.backup_count<0)text(regular,31,48,660,MUTED,"Backup list unavailable",984);
        else if(!state.backup_count)text(regular,31,48,660,MUTED,"No backups yet",984);
        else for(int i=list_offset;i<state.backup_count&&i<list_offset+8;i++){int y=615+(i-list_offset)*165;char short_id[13];snprintf(short_id,sizeof(short_id),"%.12s",state.backup_id[i]);text(regular,30,48,y+43,WHITE,state.backup[i],710);text(mono,22,48,y+85,DIM,short_id,700);if(valid_backup_id(state.backup_id[i]))control(790,y,242,115,confirmation_ready(4)?GREEN_DARK:PANEL,confirmation_ready(4)?GREEN:DIM,"RESTORE",confirmation_ready(4)?A_RESTORE:0,0,state.backup_id[i]);divider(y+130);}
        control(48,1985,477,120,PANEL,list_offset?MUTED:DIM,"PREV",list_offset?A_SCROLL:0,-8,NULL);control(555,1985,477,120,PANEL,list_offset+8<state.backup_count?MUTED:DIM,"NEXT",list_offset+8<state.backup_count?A_SCROLL:0,8,NULL);
    }else{
        heading("Server log");char logs[12][160];recent_logs(logs,12);
        for(int i=0;i<12;i++){int y=400+i*136;size_t len=strlen(logs[i]);char first[61];snprintf(first,sizeof(first),"%.60s",logs[i]);text(mono,25,48,y,MUTED,first,984);if(len>60)text(mono,25,48,y+40,DIM,logs[i]+60,984);}
    }
    const char *notice=message_until>now_ms()?message:state.error[0]?state.error:state.operation[0]?state.operation:!fresh()?"Waiting for current server data...":"";
    if(*notice){text(regular,25,48,2160,AMBER,notice,730);control(800,2110,232,90,PANEL,AMBER,"DETAILS",A_NOTICE,0,NULL);}
    const char *tabs[]={"HOME","PLAYERS","TOOLS","SETTINGS"};
    const int destinations[]={HOME,PLAYERS,TOOLS,SETTINGS};
    int section=(page==LOGS||page==BACKUPS||page==WIFI)?TOOLS:(page==SERVER_SETTINGS||page==DEVICE_SETTINGS)?SETTINGS:page;
    for(int i=0;i<4;i++)control(24+i*264,2220,240,140,section==destinations[i]?GREEN_DARK:PANEL,section==destinations[i]?GREEN:MUTED,tabs[i],A_PAGE,destinations[i],NULL);
}
/* External telemetry on these pages changes without a backend state update.
   Other pages redraw only for input, state, freshness, or message changes. */
static int periodic_page(void){
    if(notice_open||confirm||keyboard_open||difficulty_open)return 0;
    return page==HOME||page==WIFI||page==BACKUPS||page==LOGS;
}
static int redraw_needed(long long now){
    return !last_draw || last_fresh!=fresh() || last_message_visible!=(message_until>now) ||
           last_server!=running() || (periodic_page() && now-last_draw>=5000);
}/* Cap awake waits at five seconds for process status and stale-state checks. */
static int poll_timeout(long long now){
    long long due=awake?now+5000:-1;
    if(awake){
        if(state_read_due<due)due=state_read_due;
        if(!helper_pid&&helper_due<due)due=helper_due;
        if(last_activity+(long long)idle_seconds*1000+1<due)due=last_activity+(long long)idle_seconds*1000+1;
        if(last_draw==0)due=now;
        if(periodic_page()&&last_draw&&last_draw+5000<due)due=last_draw+5000;
        if(message_until>now&&message_until<due)due=message_until;
    }
    if(display_save_due&&!display_save_pid&&(due<0||display_save_due<due))due=display_save_due;
    if(due<0)return -1;
    if(due<=now)return 0;
    return due-now>5000?5000:(int)(due-now);
}static int server_command(const char *cmd,int mark_stop){
    int fd=open("/run/minecraft-console",O_WRONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0){msg("Could not reach the server console.");return 0;}
    if(mark_stop){int m=open("/run/minecraft-stop",O_WRONLY|O_CREAT|O_CLOEXEC,0600);if(m<0){close(fd);msg("Could not stop the supervisor.");return 0;}close(m);}
    ssize_t n=write(fd,cmd,strlen(cmd));close(fd);if(n!=(ssize_t)strlen(cmd)){if(mark_stop)unlink("/run/minecraft-stop");msg("Server command failed.");return 0;}return 1;
}
static void activate(const Hit *hit){
    int a=hit->action,v=hit->value;const char *arg=hit->arg;char n[32];
    if(a==A_PAGE){page=v;list_offset=0;return;}
    if(a==A_CANCEL){if(notice_open){notice_open=0;return;}confirm=0;keyboard_open=0;difficulty_open=0;wifi_show_password=0;wifi_cursor=0;explicit_bzero(wifi_password,sizeof(wifi_password));return;}
    if(a==A_NOTICE){notice_open=1;return;}
    if(a==A_LIST){list_mode=v;list_offset=0;return;}
    if(a==A_SCROLL&&page==TOOLS){list_offset=page_offset(list_offset+v,TOOL_COUNT,4);return;}
    if(a==A_SCROLL&&page==WIFI){list_offset=bounded(list_offset+v,0,wifi_count?((wifi_count-1)/8)*8:0);return;}
    if(a==A_SCROLL){int count=page==BACKUPS?state.backup_count:list_mode?state.allow_count:bounded(state.players,0,64);int step=page==BACKUPS?8:9;list_offset=bounded(list_offset+v,0,count?((count-1)/step)*step:0);return;}
    if(a==A_KEY){char *target=keyboard_open==2?wifi_password:typed_name;size_t max=keyboard_open==2?64:16,len=strlen(target);if(len<max){size_t at=keyboard_open==2?wifi_cursor:len;if(at>len)at=len;memmove(target+at+1,target+at,len-at+1);target[at]=(char)v;if(keyboard_open==2)wifi_cursor=at+1;}return;}
    if(a==A_WIFI_SPACE&&keyboard_open==2){size_t len=strlen(wifi_password);if(len<64){size_t at=wifi_cursor>len?len:wifi_cursor;memmove(wifi_password+at+1,wifi_password+at,len-at+1);wifi_password[at]=' ';wifi_cursor=at+1;}return;}
    if(a==A_WIFI_SHOW&&keyboard_open==2){wifi_show_password=!wifi_show_password;return;}
    if(a==A_WIFI_LEFT&&keyboard_open==2){if(wifi_cursor)wifi_cursor--;return;}
    if(a==A_WIFI_RIGHT&&keyboard_open==2){if(wifi_cursor<strlen(wifi_password))wifi_cursor++;return;}
    if(a==A_WIFI_SCAN){if(wifi_sender("SCAN"))msg("Scanning...");else msg("Wi-Fi service unavailable.");return;}
    if(a==A_WIFI_DISCONNECT){if(!wifi_status_available())msg("Wi-Fi status unavailable.");else if(wifi_sender("DISCONNECT"))msg("Disconnecting...");else msg("Wi-Fi service unavailable.");return;}
    if(a==A_WIFI_SELECT){if(!wifi_status_available()){msg("Wi-Fi status unavailable.");return;}if(!v){msg("This network type is not supported yet.");return;}snprintf(wifi_selected,sizeof(wifi_selected),"%.64s",arg);hex_decode(arg,wifi_label,sizeof(wifi_label));explicit_bzero(wifi_password,sizeof(wifi_password));wifi_cursor=0;if(v==1)wifi_connect("","OPEN");else{keyboard_open=2;wifi_keyboard_mode=0;wifi_show_password=0;}return;}
    if(a==A_SUBMIT&&keyboard_open==2){if(!wifi_password_valid(wifi_password)){msg(strlen(wifi_password)<8?"Use at least 8 characters.":"A 64-character password must be hexadecimal.");return;}if(wifi_connect(wifi_password,"WPA2")){explicit_bzero(wifi_password,sizeof(wifi_password));wifi_show_password=0;wifi_cursor=0;keyboard_open=0;}return;}
    if(a==A_SHIFT){if(keyboard_open==2)wifi_keyboard_mode=(wifi_keyboard_mode+1)%3;else keyboard_shift=!keyboard_shift;return;}
    if(a==A_ERASE){char *target=keyboard_open==2?wifi_password:typed_name;size_t len=strlen(target),at=keyboard_open==2?wifi_cursor:len;if(at>len)at=len;if(at){memmove(target+at-1,target+at,len-at+1);if(keyboard_open==2)wifi_cursor=at-1;}return;}
    if(a==A_BRIGHT){saved_brightness=bounded(saved_brightness+v*205,205,4095);brightness(saved_brightness);schedule_display_save();return;}
    if(a==A_TIMEOUT){idle_seconds=bounded(idle_seconds+v*30,30,600);schedule_display_save();return;}
    if(busy()){msg("Please wait for the current operation.");return;}
    if(a==A_START){if(!start_ready()){msg("Server state changed; refresh and try again.");helper_due=0;return;}if(unlink("/run/minecraft-stop")==0||errno==ENOENT)msg("Starting server...");else msg("Could not start server.");helper_due=0;return;}
    if(a==A_RESTART||a==A_STOP){if(!fresh()||!server_identity(&confirm_server_pid,&confirm_server_start)){msg("Server state changed; refresh and try again.");return;}confirm=a==A_RESTART?1:2;return;}
    if(a==A_CONFIRM){if(!confirmation_ready(confirm)){msg("State changed. Cancel and review again.");return;}int c=confirm;confirm=0;input_block_until=now_ms()+600;
        if(c==1||c==2){if(server_command("stop\n",c==2))msg(c==1?"Saving world and restarting...":"Saving world and stopping...");}
        else if(c==3)admin("whitelist-remove",selected_name);
        else if(c==4)admin("backup",NULL);
        else if(c==6)admin("whitelist-set","on");
        else if(c==7)admin("whitelist-set","off");
        else if(c==5&&valid_backup_id(selected_backup))admin("restore",selected_backup);
        return;
    }
    if(!fresh()){msg("Wait for current server data.");return;}
    if(a==A_ADD){if(!access_editable()){msg(access_unavailable_reason());return;}typed_name[0]=0;keyboard_open=1;return;}
    if(a==A_REMOVE){if(!access_editable()){msg(access_unavailable_reason());return;}copy_value(selected_name,sizeof(selected_name),arg);confirm=3;return;}
    if(a==A_ALLOW){if(!access_editable()||(strcmp(state.whitelist,"on")&&strcmp(state.whitelist,"off"))){msg("Whitelist state unavailable.");return;}if(v){if(!strcmp(state.whitelist,"on")){confirm=7;return;}if(state.allow_count==0){confirm=6;return;}admin("whitelist-set","on");}else admin("whitelist-add",arg);return;}
    if(a==A_SUBMIT){if(!access_editable()){msg(access_unavailable_reason());return;}if(strlen(typed_name)<3){msg("Use a username with 3 to 16 characters.");return;}if(admin("whitelist-add",typed_name))keyboard_open=0;return;}
    if(a==A_DIFFICULTY){const char *levels[]={"peaceful","easy","normal","hard"};if(v<0){difficulty_open=1;return;}if(v<4&&difficulty_open){if(!strcmp(state.difficulty,levels[v])||admin("difficulty",levels[v]))difficulty_open=0;}return;}
    if(a==A_MAX||a==A_VIEW||a==A_SIM){int old=a==A_MAX?state.max_players:a==A_VIEW?state.view_distance:state.simulation_distance;int lo=a==A_MAX?1:2,hi=a==A_MAX?20:12;if(old<lo||old>hi||(v!=-1&&v!=1)||(v<0&&old==lo)||(v>0&&old==hi)){msg("Setting changed; refresh and try again.");helper_due=0;return;}snprintf(n,sizeof(n),"%d",old+v);admin(a==A_MAX?"max-players":a==A_VIEW?"view-distance":"simulation-distance",n);return;}
    if(a==A_BACKUP){confirm=4;return;}if(a==A_RESTORE&&valid_backup_id(arg)){copy_value(selected_backup,sizeof(selected_backup),arg);selected_backup_label[0]=0;for(int i=0;i<state.backup_count;i++)if(!strcmp(state.backup_id[i],arg)){copy_value(selected_backup_label,sizeof(selected_backup_label),state.backup[i]);break;}confirm=5;return;}
}
static int hit_at(int x,int y){for(int i=hit_count-1;i>=0;i--)if(x>=hits[i].x&&x<hits[i].x+hits[i].w&&y>=hits[i].y&&y<hits[i].y+hits[i].h)return i;return -1;}
static void resume_touch(void){
    int fd=open("/sys/kernel/oplus_display/notify_panel_blank",O_WRONLY|O_CLOEXEC);
    if(fd>=0){if(write(fd,"1\n",2)!=2)perror("touch resume notification");close(fd);}
}
static int display_power(int on){
    if(!dpms_id)return -1;
    int result=drmModeConnectorSetProperty(drmfd,conn_id,dpms_id,on?0:3);
    if(on && result==0)resume_touch();
    return result;
}
static int brightness(int value){
    int fd=open("/sys/class/backlight/panel0-backlight/brightness",O_WRONLY|O_CLOEXEC);
    if(fd<0){perror("brightness open");return -1;}
    char b[16];int n=snprintf(b,sizeof(b),"%d\n",value);int ok=write(fd,b,(size_t)n)==n;close(fd);
    if(!ok){perror("brightness write");return -1;}return 0;
}
static void darken(void){
    if(!awake)return;
    wifi_show_password=0;
    memset(mapped,0,map_size);
    if(brightness(0)!=0)fprintf(stderr,"using black framebuffer without brightness control\n");
    panel_off=display_power(0)==0;
    awake=0;touch_down=0;stop_state_watch();fprintf(stderr,"screen darkened; panel %s\n",panel_off?"off":"still active");
}
static void wake(void){
    long long now=now_ms();last_activity=now;
    if(!awake){if(panel_off&&!reset_display()){fprintf(stderr,"Display reinitialization failed; starting console fallback\n");display_failed=1;exiting=1;return;}if(panel_off&&touchfd>=0)resync_touch(touchfd);panel_off=0;awake=1;start_state_watch("/run/n20");state_read_due=0;touch_down=0;pressed_valid=0;helper_due=0;read_state();draw();memcpy(mapped,canvas,map_size);brightness(saved_brightness);input_block_until=now_ms()+150;fprintf(stderr,"screen awakened\n");}
    invalidate_full();
}
static void toggle_display(void){if(awake)darken();else wake();}
static void (*power_button_action)(void)=toggle_display;
static int open_input(const char*path){return open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);}
static int input_poll_failed(short revents){return (revents&(POLLERR|POLLHUP|POLLNVAL))!=0;}
/* Recover conservatively after kernel event loss; never activate an old tap. */
static void resync_touch(int fd){
    struct input_absinfo slot;int tracking_ok=0,key_ok=0,key_down=0;uint32_t slots=0;
    if(ioctl(fd,EVIOCGABS(ABS_MT_SLOT),&slot)==0&&slot.minimum==0&&slot.value>=0&&slot.value<=slot.maximum&&slot.maximum<32){
        touch_slot=slot.value;int32_t ids[33]={0};ids[0]=ABS_MT_TRACKING_ID;
        size_t len=(size_t)(slot.maximum+2)*sizeof(ids[0]);
        if(ioctl(fd,EVIOCGMTSLOTS(len),ids)==0){tracking_ok=1;for(int i=0;i<=slot.maximum;i++)if(ids[i+1]>=0)slots|=1u<<i;}
    }
    unsigned long keys[(KEY_MAX+8*sizeof(unsigned long))/(8*sizeof(unsigned long))]={0};
    if(ioctl(fd,EVIOCGKEY(sizeof(keys)),keys)==0){key_ok=1;key_down=(keys[BTN_TOUCH/(8*sizeof(unsigned long))]>>(BTN_TOUCH%(8*sizeof(unsigned long))))&1u;}
    active_slots=slots;primary_slot=-1;touch_down=pressed_valid=pending_touch_down=pending_touch_up=0;
    touch_blocked=slots||key_down||(!tracking_ok&&!key_ok);invalidate_full();
}/* Editing the current keyboard changes its field and pressed key only. */
static int keyboard_edit_action(int action){
    return keyboard_open&&!notice_open&&!confirm&&!difficulty_open&&
           (action==A_KEY||action==A_ERASE||action==A_WIFI_SPACE||action==A_WIFI_LEFT||action==A_WIFI_RIGHT);
}
static void invalidate_hit(const Hit *hit,int changed){
    /* Press feedback only adds an outline; release may change the page. */
    if(!changed){invalidate_rows(hit->y,hit->y+hit->h);return;}
    if(!keyboard_edit_action(hit->action)){invalidate_full();return;}
    int top=hit->y,bottom=hit->y+hit->h;
    if(changed){if(top>400)top=400;if(keyboard_open==2&&bottom<1480)bottom=1480;else if(keyboard_open!=2&&bottom<545)bottom=545;}
    invalidate_rows(top,bottom);
}static void events(int fd,int power){
    struct input_event ev[32];ssize_t n;
    while((n=read(fd,ev,sizeof(ev)))>0)for(size_t i=0;i<(size_t)n/sizeof(ev[0]);i++){
        struct input_event *e=&ev[i];
        if(power){if(e->type==EV_KEY&&e->code==KEY_POWER&&e->value==1)power_button_action();continue;}
        if(!awake&&panel_off)continue;
        if(e->type==EV_SYN&&e->code==SYN_DROPPED){input_sync_lost=1;touch_down=pressed_valid=pending_touch_down=pending_touch_up=0;touch_blocked=1;invalidate_full();continue;}
        if(input_sync_lost){if(e->type==EV_SYN&&e->code==SYN_REPORT){resync_touch(fd);input_sync_lost=0;}continue;}
        if(e->type==EV_KEY&&(e->code==KEY_WAKEUP||e->code==KEY_F4)&&e->value==1){wake();touch_down=0;pressed_valid=0;input_block_until=now_ms()+150;continue;}
        if(e->type==EV_ABS){
            if(e->code==ABS_MT_SLOT){if(e->value>=0&&e->value<32)touch_slot=e->value;else{touch_blocked=1;pressed_valid=0;}continue;}
            if(e->code==ABS_MT_TRACKING_ID){
                uint32_t bit=1u<<touch_slot;
                if(e->value>=0){if(active_slots&&!(active_slots&bit)){touch_blocked=1;pressed_valid=0;drag_cancel=1;}active_slots|=bit;if(primary_slot<0)primary_slot=touch_slot;}
                else {active_slots&=~bit;if(primary_slot==touch_slot)primary_slot=-1;}
                continue;
            }
            if((e->code==ABS_MT_POSITION_X||e->code==ABS_MT_POSITION_Y)&&primary_slot>=0&&touch_slot!=primary_slot)continue;
            if(e->code==ABS_MT_POSITION_X||e->code==ABS_X)touch_x=e->value;
            if(e->code==ABS_MT_POSITION_Y||e->code==ABS_Y)touch_y=e->value;
            if(touch_down){int x=(int)((long long)(touch_x-xmin)*(W-1)/(xmax-xmin));int y=(int)((long long)(touch_y-ymin)*(H-1)/(ymax-ymin));if(abs(x-down_x)>35||abs(y-down_y)>35)drag_cancel=1;}
        }
        if(e->type==EV_KEY&&e->code==BTN_TOUCH){if(e->value==1)pending_touch_down=1;else if(e->value==0)pending_touch_up=1;}
        if(e->type==EV_SYN&&e->code==SYN_REPORT){
            if(pending_touch_down){pending_touch_down=0;
                long long now=now_ms();last_activity=now;
                if(!awake){wake();touch_down=0;pressed_valid=0;continue;}
                if(now<input_block_until||touch_blocked){touch_down=0;pressed_valid=0;continue;}
                down_x=(int)((long long)(touch_x-xmin)*(W-1)/(xmax-xmin));down_y=(int)((long long)(touch_y-ymin)*(H-1)/(ymax-ymin));
                int index=hit_at(down_x,down_y);pressed_valid=index>=0;if(pressed_valid)pressed_hit=hits[index];touch_down=1;drag_cancel=0;if(pressed_valid)invalidate_hit(&pressed_hit,0);else invalidate_full();
            }else if(pending_touch_up){pending_touch_up=0;
                if(touch_down&&awake&&pressed_valid&&!drag_cancel&&now_ms()>=input_block_until){
                    int x=(int)((long long)(touch_x-xmin)*(W-1)/(xmax-xmin));int y=(int)((long long)(touch_y-ymin)*(H-1)/(ymax-ymin));int index=hit_at(x,y);
                    if(index>=0&&hits[index].action==pressed_hit.action&&hits[index].value==pressed_hit.value&&!strcmp(hits[index].arg,pressed_hit.arg)){activate(&pressed_hit);if(pressed_hit.action!=A_KEY&&pressed_hit.action!=A_ERASE&&pressed_hit.action!=A_WIFI_SPACE&&pressed_hit.action!=A_WIFI_LEFT&&pressed_hit.action!=A_WIFI_RIGHT){long long until=now_ms()+180;if(input_block_until<until)input_block_until=until;}}
                }
                if(pressed_valid)invalidate_hit(&pressed_hit,1);else invalidate_full();
                touch_down=0;pressed_valid=0;if(!active_slots)touch_blocked=0;last_activity=now_ms();
                if(metrics_enabled){long long stamp=(long long)e->time.tv_sec*1000+e->time.tv_usec/1000;metric_input_at=touch_monotonic&&stamp>0&&stamp<=last_activity?stamp:last_activity;}
            }
        }
    }
}
static int setup_display(void){
    drmfd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);if(drmfd<0){perror("drm open");return 0;}drmModeRes *res=drmModeGetResources(drmfd);if(!res)return 0;drmModeConnector*c=NULL;
    for(int i=0;i<res->count_connectors;i++){drmModeConnector*t=drmModeGetConnector(drmfd,res->connectors[i]);if(t&&t->connection==DRM_MODE_CONNECTED&&t->count_modes){c=t;break;}if(t)drmModeFreeConnector(t);}if(!c){drmModeFreeResources(res);return 0;}
    conn_id=c->connector_id;mode=c->modes[0];if(mode.hdisplay!=W||mode.vdisplay!=H){fprintf(stderr,"unsupported mode %dx%d\n",mode.hdisplay,mode.vdisplay);drmModeFreeConnector(c);drmModeFreeResources(res);return 0;}
    drmModeEncoder*enc=drmModeGetEncoder(drmfd,c->encoder_id);if(enc){crtc_id=enc->crtc_id;drmModeFreeEncoder(enc);}if(!crtc_id&&res->count_crtcs)crtc_id=res->crtcs[0];
    for(int i=0;i<c->count_props;i++){drmModePropertyRes*p=drmModeGetProperty(drmfd,c->props[i]);if(p){if(!strcmp(p->name,"DPMS"))dpms_id=p->prop_id;drmModeFreeProperty(p);}}
    drmModeFreeConnector(c);drmModeFreeResources(res);
    struct drm_mode_create_dumb create={0};create.width=W;create.height=H;create.bpp=32;if(drmIoctl(drmfd,DRM_IOCTL_MODE_CREATE_DUMB,&create)){perror("create dumb");return 0;}handle=create.handle;pitch=create.pitch;map_size=(size_t)create.size;
    if(drmModeAddFB(drmfd,W,H,24,32,pitch,handle,&fb_id)){perror("add fb");return 0;}struct drm_mode_map_dumb map={0};map.handle=handle;if(drmIoctl(drmfd,DRM_IOCTL_MODE_MAP_DUMB,&map)){perror("map dumb");return 0;}
    mapped=mmap(NULL,map_size,PROT_READ|PROT_WRITE,MAP_SHARED,drmfd,map.offset);if(mapped==MAP_FAILED){mapped=NULL;perror("mmap");return 0;}
    canvas=calloc(1,map_size);if(!canvas){perror("canvas");return 0;}
    draw();memcpy(mapped,canvas,map_size);if(drmModeSetCrtc(drmfd,crtc_id,fb_id,0,0,&conn_id,1,&mode)){perror("set crtc");return 0;}if(dpms_id&&display_power(1)!=0){perror("display power on");return 0;}return 1;
}
static void release_display(void){
    if(canvas){free(canvas);canvas=NULL;}
    if(mapped){munmap(mapped,map_size);mapped=NULL;}
    if(drmfd>=0&&fb_id)drmModeRmFB(drmfd,fb_id);
    if(drmfd>=0&&handle){struct drm_mode_destroy_dumb d={.handle=handle};drmIoctl(drmfd,DRM_IOCTL_MODE_DESTROY_DUMB,&d);}
    if(drmfd>=0)close(drmfd);
    drmfd=-1;crtc_id=conn_id=fb_id=handle=pitch=dpms_id=0;map_size=0;
}
/* This device needs a fresh DRM connection after powering its panel off. */
static int reset_display(void){release_display();return setup_display();}
static void range(int fd,int axis,int *lo,int *hi,int *position){struct input_absinfo a;if(ioctl(fd,EVIOCGABS(axis),&a)==0&&a.maximum>a.minimum){*lo=a.minimum;*hi=a.maximum;*position=a.value;}}
static void current_mt_position(int fd){
    struct input_absinfo slot;if(ioctl(fd,EVIOCGABS(ABS_MT_SLOT),&slot)||slot.minimum!=0||slot.value<0||slot.value>slot.maximum||slot.maximum>=32)return;
    int32_t positions[33]={0};size_t len=(size_t)(slot.maximum+2)*sizeof(positions[0]);
    positions[0]=ABS_MT_POSITION_X;if(ioctl(fd,EVIOCGMTSLOTS(len),positions)==0)touch_x=positions[slot.value+1];
    positions[0]=ABS_MT_POSITION_Y;if(ioctl(fd,EVIOCGMTSLOTS(len),positions)==0)touch_y=positions[slot.value+1];
}
int main(void){
    metrics_enabled=getenv("N20_UI_METRICS")!=NULL;metric_window=now_ms();
    if(pipe2(signal_pipe,O_NONBLOCK|O_CLOEXEC)){perror("signal pipe");return 1;}
    signal(SIGTERM,on_signal);signal(SIGINT,on_signal);signal(SIGUSR1,on_sleep_signal);signal(SIGCHLD,on_child_signal);if(FT_Init_FreeType(&ft)||FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",0,&regular)||FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",0,&bold)||FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",0,&mono)){fprintf(stderr,"font setup failed\n");return 1;}
    if(!setup_display()){fprintf(stderr,"display setup failed\n");return 1;}touchfd=open_input("/dev/input/event2");powerfd=open_input("/dev/input/event0");if(touchfd<0||powerfd<0){fprintf(stderr,"Required touch or power input unavailable; starting console fallback\n");return 1;}if(touchfd>=0){range(touchfd,ABS_MT_POSITION_X,&xmin,&xmax,&touch_x);range(touchfd,ABS_MT_POSITION_Y,&ymin,&ymax,&touch_y);int clock_id=CLOCK_MONOTONIC;touch_monotonic=ioctl(touchfd,EVIOCSCLOCKID,&clock_id)==0;current_mt_position(touchfd);resync_touch(touchfd);}fprintf(stderr,"input touch=%d power=%d range=%d..%d,%d..%d dpms=%u\n",touchfd,powerfd,xmin,xmax,ymin,ymax,dpms_id);
    long initial_brightness=read_long("/sys/class/backlight/panel0-backlight/brightness",1215);
    if(initial_brightness>0 && initial_brightness<=4095)saved_brightness=(int)initial_brightness;
    load_display_settings();
    brightness(saved_brightness);
    int gesture=open("/proc/touchpanel/double_tap_enable",O_WRONLY|O_CLOEXEC);
    if(gesture>=0){if(write(gesture,"0\n",2)!=2)perror("disable touch gesture mode");close(gesture);}
    read_state();last_activity=now_ms();invalidate_full();
    int input_failed=0;
    while(!exiting){
        struct pollfd p[4]={{touchfd,POLLIN,0},{powerfd,POLLIN,0},{signal_pipe[0],POLLIN,0},{state_watch_fd,POLLIN,0}};
        poll(p,4,poll_timeout(now_ms()));
        if(input_poll_failed(p[0].revents)||input_poll_failed(p[1].revents)){fprintf(stderr,"Input device failed; starting console fallback\n");input_failed=1;break;}
        if(p[2].revents&POLLIN){char bytes[64];while(read(signal_pipe[0],bytes,sizeof(bytes))>0){}}
        if(p[3].revents&(POLLERR|POLLHUP|POLLNVAL)){stop_state_watch();state_read_due=0;}
        else if(p[3].revents&POLLIN)drain_state_watch();
        if(touchfd>=0&&(p[0].revents&POLLIN))events(touchfd,0);
        if(powerfd>=0&&(p[1].revents&POLLIN))events(powerfd,1);
        long long now=now_ms();tick_helper(now);
        tick_display_save(now);
        if(sleep_requested){sleep_requested=0;darken();}
        if(awake&&now-last_activity>idle_seconds*1000LL)darken();
        if(awake&&redraw_needed(now)){
            long long render_start=metrics_enabled?now_ms():0;
            /* Time, status, and periodic refreshes require a complete frame. */
            if(last_draw||last_fresh!=fresh()||last_message_visible!=(message_until>now)||last_server!=running())invalidate_full();
            clip_top=dirty_top;clip_bottom=dirty_bottom;
            draw();
            if(touch_down&&pressed_valid&&!drag_cancel){Hit *h=&pressed_hit;rect(h->x,h->y,h->w,4,GREEN);rect(h->x,h->y+h->h-4,h->w,4,GREEN);}
            for(int y=clip_top;y<clip_bottom;y++)memcpy((char *)mapped+(size_t)y*pitch,(char *)canvas+(size_t)y*pitch,pitch);
            clip_top=0;clip_bottom=H;dirty_top=H;dirty_bottom=0;last_draw=now;
            if(metrics_enabled){
                long long finished=now_ms();metric_render_ms+=finished-render_start;metric_frames++;
                if(metric_input_at){long long latency=finished-metric_input_at;metric_latency_ms+=latency;if(latency>metric_latency_max)metric_latency_max=latency;metric_inputs++;metric_input_at=0;}
                if(finished-metric_window>=60000){struct rusage usage={0};getrusage(RUSAGE_SELF,&usage);
                    fprintf(stderr,"UI metrics: frames=%u mean_render_ms=%.1f inputs=%u mean_event_to_submit_ms=%.1f max_event_to_submit_ms=%lld timestamp=%s maxrss_kib=%ld\n",metric_frames,metric_frames?(double)metric_render_ms/metric_frames:0,metric_inputs,metric_inputs?(double)metric_latency_ms/metric_inputs:0,metric_latency_max,touch_monotonic?"kernel":"userspace",usage.ru_maxrss);
                    metric_window=finished;metric_render_ms=metric_latency_ms=metric_latency_max=0;metric_frames=metric_inputs=0;}
            }
        }
    }
    if(display_save_pid){int status;pid_t done;do{done=waitpid(display_save_pid,&status,0);}while(done<0&&errno==EINTR);if(done!=display_save_pid||!WIFEXITED(status)||WEXITSTATUS(status)!=0)display_save_due=1;}
    if(display_save_due&&!save_display_settings())fprintf(stderr,"Could not save display settings\n");
    brightness(saved_brightness);
    display_power(0);
    if(touchfd>=0) close(touchfd);
    if(powerfd>=0) close(powerfd);
    stop_state_watch();close(signal_pipe[0]);close(signal_pipe[1]);
    release_display();
    return (input_failed||display_failed)?1:0;
}
