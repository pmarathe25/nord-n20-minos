#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
static int touch_x=0,touch_y=0,touch_down=0;
static int xmin=0,xmax=1079,ymin=0,ymax=2399;
static long long last_activity=0, last_draw=0, message_until=0;
static char message[128];
static volatile sig_atomic_t exiting=0, sleep_requested=0;
enum { HOME, PLAYERS, SETTINGS, LOGS, BACKUPS, WIFI };
enum { A_PAGE=1, A_START, A_RESTART, A_STOP, A_CONFIRM, A_CANCEL,
    A_LIST, A_ADD, A_REMOVE, A_ALLOW, A_DIFFICULTY, A_MAX, A_VIEW, A_SIM,
    A_BRIGHT, A_TIMEOUT, A_BACKUP, A_RESTORE, A_KEY, A_ERASE, A_SUBMIT,
    A_SHIFT, A_SCROLL, A_WIFI_SELECT, A_WIFI_SCAN, A_WIFI_DISCONNECT, A_WIFI_SPACE };
typedef struct { int x,y,w,h,action,value; char arg[96]; } Hit;
static Hit hits[100], pressed_hit;
static int hit_count, pressed_valid, down_x, down_y, drag_cancel;
static int list_mode, list_offset, keyboard_open, keyboard_shift, idle_seconds=120;
static char wifi_password[65], wifi_selected[65], wifi_label[80];
static int wifi_keyboard_mode;
static char typed_name[17], selected_name[32], selected_backup[96], selected_backup_label[96];
static long long input_block_until, wake_started, helper_due, state_read_due;
static int pending_touch_down, pending_touch_up;
static pid_t helper_pid;
static int helper_mutation;
typedef struct {
    int valid, players, allow_count, backup_count, max_players, view_distance, simulation_distance;
    long updated;
    char server[24], whitelist[12], difficulty[20], error[160], operation[160];
    char player[64][32], allow[128][32], backup[64][96], backup_id[64][96];
} State;
static State state;

static long long now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static void on_signal(int sig){(void)sig;exiting=1;}
static void on_sleep_signal(int sig){(void)sig;sleep_requested=1;}
static void msg(const char *s){snprintf(message,sizeof(message),"%s",s);message_until=now_ms()+8000;}
static void rect(int x,int y,int w,int h,uint32_t c){if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}if(x+w>W)w=W-x;if(y+h>H)h=H-y;for(int j=0;j<h;j++){uint32_t*p=(uint32_t*)((char*)canvas+(size_t)(y+j)*pitch)+x;for(int i=0;i<w;i++)p[i]=c;}}
static uint32_t blend(uint32_t bg,uint32_t fg,unsigned a){unsigned inv=255-a;unsigned r=(((bg>>16)&255)*inv+((fg>>16)&255)*a)/255;unsigned g=(((bg>>8)&255)*inv+((fg>>8)&255)*a)/255;unsigned b=((bg&255)*inv+(fg&255)*a)/255;return (r<<16)|(g<<8)|b;}
static int text_width(FT_Face face,int size,const char *s){FT_Set_Pixel_Sizes(face,0,size);int w=0;for(;*s;s++){unsigned char ch=(unsigned char)*s;if(FT_Load_Char(face,ch,FT_LOAD_DEFAULT)==0)w+=(int)(face->glyph->advance.x>>6);}return w;}
static void text(FT_Face face,int size,int x,int baseline,uint32_t color,const char*s,int maxwidth){
    FT_Set_Pixel_Sizes(face,0,size);int start=x;
    for(;*s;s++){unsigned char ch=(unsigned char)*s;if(ch<32||ch>126)ch='?';if(FT_Load_Char(face,ch,FT_LOAD_RENDER))continue;FT_GlyphSlot g=face->glyph;if(maxwidth>0&&x-start+(int)(g->advance.x>>6)>maxwidth)break;int px=x+g->bitmap_left,py=baseline-g->bitmap_top;
        for(unsigned yy=0;yy<g->bitmap.rows;yy++)for(unsigned xx=0;xx<g->bitmap.width;xx++){int dx=px+(int)xx,dy=py+(int)yy;if(dx<0||dx>=W||dy<0||dy>=H)continue;unsigned a=g->bitmap.buffer[yy*g->bitmap.pitch+xx];if(a){uint32_t*dst=(uint32_t*)((char*)canvas+(size_t)dy*pitch)+dx;*dst=blend(*dst,color,a);}}
        x+=(int)(g->advance.x>>6);
    }
}
static void centered(FT_Face f,int size,int x,int y,int w,uint32_t c,const char*s){text(f,size,x+(w-text_width(f,size,s))/2,y,c,s,w);}
static int read_line(const char *p,char *out,size_t n){FILE*f=fopen(p,"r");if(!f)return 0;if(!fgets(out,(int)n,f)){fclose(f);return 0;}fclose(f);out[strcspn(out,"\r\n")]=0;return 1;}
static long read_long(const char *p,long fallback){char s[80];return read_line(p,s,sizeof(s))?strtol(s,NULL,10):fallback;}
static int running(void){long pid=read_long("/run/minecraft.pid",0);return pid>1&&(kill((pid_t)pid,0)==0||errno==EPERM);}
static void uptime_str(char *s,size_t n){FILE*f=fopen("/proc/uptime","r");double secs=0;if(f){(void)fscanf(f,"%lf",&secs);fclose(f);}unsigned m=(unsigned)secs/60;snprintf(s,n,"%uh %02um",m/60,m%60);}
static void recent_logs(char lines[][160],int wanted){
    for(int i=0;i<wanted;i++)lines[i][0]=0;
    FILE*f=fopen("/var/log/minecraft.log","r");if(!f)return;fseek(f,0,SEEK_END);long len=ftell(f);if(len>24000)fseek(f,len-24000,SEEK_SET);else rewind(f);
    char buf[1024];int k=0;while(fgets(buf,sizeof(buf),f)){buf[strcspn(buf,"\r\n")]=0;if(!*buf)continue;snprintf(lines[k%wanted],160,"%.155s",buf);k++;}fclose(f);
    if(k>wanted){char tmp[12][160];for(int i=0;i<wanted;i++)memcpy(tmp[i],lines[(k+i)%wanted],160);for(int i=0;i<wanted;i++)memcpy(lines[i],tmp[i],160);}
}
static void network_addr(char*s,size_t n){char ip[80],line[160]; FILE*f=fopen("/run/n20/wifi-state","r");if(f){while(fgets(line,sizeof(line),f)){if(!strncmp(line,"IP\t",3)){line[strcspn(line,"\r\n")]=0;if(line[3]){snprintf(s,n,"Wi-Fi: %.15s:25565",line+3);fclose(f);return;}}}fclose(f);}struct in_addr a;if(!read_line("/etc/n20-lan-address",ip,sizeof(ip))||inet_pton(AF_INET,ip,&a)!=1)snprintf(s,n,"PC Wi-Fi IP:25566");else snprintf(s,n,"LAN (last known): %.15s:25566",ip);}
/* old address function replaced */
#if 0
static void old_network_addr(char*s,size_t n){char ip[80];struct in_addr a;if(!read_line("/etc/n20-lan-address",ip,sizeof(ip))||inet_pton(AF_INET,ip,&a)!=1)snprintf(s,n,"PC Wi-Fi IP:25566");else snprintf(s,n,"LAN (last known): %.15s:25566",ip);}
#endif
static int brightness(int value);
static void wake(void);
static void draw(void);
static int bounded(int n,int lo,int hi){return n<lo?lo:n>hi?hi:n;}
static void save_display_settings(void){
    if(mkdir("/var/lib/n20",0700)<0 && errno!=EEXIST){msg("Could not save display settings.");return;}
    FILE*f=fopen("/var/lib/n20/display.tmp","w");if(!f){msg("Could not save display settings.");return;}
    int ok=fprintf(f,"%d %d\n",saved_brightness,idle_seconds)>0 && fflush(f)==0 && fsync(fileno(f))==0;
    if(fclose(f)!=0)ok=0;
    if(!ok||rename("/var/lib/n20/display.tmp","/var/lib/n20/display.conf")!=0){unlink("/var/lib/n20/display.tmp");msg("Could not save display settings.");}
}
static void load_display_settings(void){
    FILE*f=fopen("/var/lib/n20/display.conf","r");if(!f)return;
    int b=0,t=0;if(fscanf(f,"%d %d",&b,&t)==2 && b>=205 && b<=4095 && t>=30 && t<=600){saved_brightness=b;idle_seconds=t;}
    fclose(f);
}
static void copy_value(char *dst,size_t n,const char *src){if(!n)return;size_t len=strnlen(src,n-1);memcpy(dst,src,len);dst[len]=0;}
static void read_state(void){
    FILE *f=fopen("/run/n20/state","r");if(!f)return;
    State next={0};char line[512];
    while(fgets(line,sizeof(line),f)){
        line[strcspn(line,"\r\n")]=0;char *v=strchr(line,'=');if(!v)continue;*v++=0;
        if(!strcmp(line,"server"))copy_value(next.server,sizeof(next.server),v);
        else if(!strcmp(line,"players"))next.players=bounded(atoi(v),0,10000);
        else if(!strcmp(line,"allow_count"))next.allow_count=bounded(atoi(v),0,128);
        else if(!strcmp(line,"backup_count"))next.backup_count=bounded(atoi(v),0,64);
        else if(!strcmp(line,"max_players"))next.max_players=atoi(v);
        else if(!strcmp(line,"view_distance"))next.view_distance=atoi(v);
        else if(!strcmp(line,"simulation_distance"))next.simulation_distance=atoi(v);
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
static int busy(void){return helper_mutation||state.operation[0]!=0;}
static int admin(const char *command,const char *arg){
    if(helper_pid){msg("Refreshing server. Try again shortly.");return 0;}
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
static void tick_helper(long long now){
    if(awake&&now>=state_read_due){State previous=state;read_state();if(memcmp(&previous,&state,sizeof(state)))last_draw=0;state_read_due=now+1000;}
    if(helper_pid){int status=0;pid_t done=waitpid(helper_pid,&status,WNOHANG);if(done==helper_pid){
        if((!WIFEXITED(status)||WEXITSTATUS(status)!=0)&&helper_mutation)msg("Request failed. Check server log.");
        int changed=helper_mutation;helper_pid=0;helper_mutation=0;read_state();if(changed)helper_due=0;last_draw=0;
    }}
    if(awake&&!helper_pid&&now>=helper_due){admin("state",NULL);helper_due=now+5000;}
}
static void pixel_block(int x,int y,int scale){
    rect(x,y,8*scale,8*scale,0x5D4430);rect(x,y,8*scale,2*scale,0x629D45);
    rect(x+scale,y+2*scale,2*scale,scale,0x426F32);rect(x+5*scale,y+2*scale,2*scale,2*scale,0x426F32);
    rect(x+scale,y+5*scale,2*scale,scale,0x856344);rect(x+5*scale,y+6*scale,scale,scale,0x392B22);
}
static void card(int x,int y,int w,int h){rect(x,y,w,h,PANEL);rect(x,y,w,2,BORDER);}
static void control(int x,int y,int w,int h,uint32_t bg,uint32_t fg,const char *label,int action,int value,const char *arg){
    rect(x,y,w,h,bg);rect(x,y,w,3,blend(bg,WHITE,35));rect(x,y+h-5,w,5,blend(bg,BG,90));
    centered(bold,31,x,y+h/2+11,w,fg,label);
    if(action&&hit_count<100){Hit *hit=&hits[hit_count++];*hit=(Hit){.x=x,.y=y,.w=w,.h=h,.action=action,.value=value};copy_value(hit->arg,sizeof(hit->arg),arg?arg:"");}
}
static void action_button(int x,int y,int w,int h,const char *label,int action,int value,const char *arg){
    int enabled=!busy();control(x,y,w,h,enabled?GREEN_DARK:0x151A17,enabled?GREEN:DIM,label,enabled?action:0,value,arg);
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
    if(page==PLAYERS)action_button(770,y+6,260,106,whitelist_row?"REMOVE":"ALLOW",whitelist_row?A_REMOVE:A_ALLOW,0,name);
    divider(y+122);
}
static void stepper(int y,const char *label,int value,int action,const char *suffix){
    char b[80];text(regular,33,48,y+55,WHITE,label,480);snprintf(b,sizeof(b),"%d%s",value,suffix?suffix:"");
    centered(mono,34,540,y+55,240,GREEN,b);action_button(800,y,100,104,"-",action,-1,NULL);action_button(930,y,100,104,"+",action,1,NULL);
}
static int valid_backup_id(const char *s){size_t n=strlen(s);if(n<8||n>80)return 0;for(;*s;s++)if(!((*s>='0'&&*s<='9')||(*s>='a'&&*s<='f')||(*s>='A'&&*s<='F')))return 0;return 1;}
static int wifi_send(const char *command){
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return 0;
    struct sockaddr_un addr={.sun_family=AF_UNIX};snprintf(addr.sun_path,sizeof(addr.sun_path),"/run/n20/wifi-control");
    int ok=sendto(fd,command,strlen(command),0,(struct sockaddr*)&addr,sizeof(addr))>=0;close(fd);return ok;
}
static void hex_encode(const char *src,char *out,size_t cap){const char *hex="0123456789abcdef";size_t i=0;while(src[i]&&i*2+2<cap){unsigned char c=(unsigned char)src[i];out[2*i]=hex[c>>4];out[2*i+1]=hex[c&15];i++;}out[2*i]=0;}
static int hex_decode(const char *hex,char *out,size_t cap){size_t n=strlen(hex);if(n%2||n/2>=cap)return 0;for(size_t i=0;i<n/2;i++){unsigned v;if(sscanf(hex+2*i,"%2x",&v)!=1)return 0;out[i]=(char)v;}out[n/2]=0;return 1;}
static void wifi_connect(const char *pass,const char *kind){char passhex[131],cmd[260];hex_encode(pass,passhex,sizeof(passhex));snprintf(cmd,sizeof(cmd),"CONNECT %s %s %s",wifi_selected,*passhex?passhex:"00",kind);if(wifi_send(cmd))msg("Connecting... Check the Wi-Fi page.");else msg("Wi-Fi service unavailable.");explicit_bzero(passhex,sizeof(passhex));explicit_bzero(cmd,sizeof(cmd));}
static void draw_wifi_keyboard(void){
    rect(0,0,W,H,BG);heading("Wi-Fi password");text(regular,30,48,350,MUTED,wifi_label,984);
    char masked[66];size_t len=strlen(wifi_password);memset(masked,'*',len);masked[len]=0;card(48,405,984,130);text(mono,40,76,490,WHITE,len?masked:"_",920);
    const char *keys=wifi_keyboard_mode==0?"1234567890qwertyuiopasdfghjklzxcvbnm":wifi_keyboard_mode==1?"1234567890QWERTYUIOPASDFGHJKLZXCVBNM":"!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~";
    for(int i=0;keys[i];i++){char key[2]={keys[i],0};control(48+(i%10)*99,620+(i/10)*145,90,130,PANEL,WHITE,key,A_KEY,(unsigned char)keys[i],NULL);}
    control(48,1220,300,120,PANEL,WHITE,wifi_keyboard_mode==0?"ABC":wifi_keyboard_mode==1?"SYMBOLS":"abc",A_SHIFT,0,NULL);
    control(366,1220,348,120,PANEL,WHITE,"SPACE",A_WIFI_SPACE,0,NULL);control(732,1220,300,120,PANEL,WHITE,"DELETE",A_ERASE,0,NULL);
    action_button(48,1420,984,140,"CONNECT",A_SUBMIT,0,NULL);control(48,1600,984,130,PANEL,MUTED,"CANCEL",A_CANCEL,0,NULL);
    if(message_until>now_ms())text(regular,28,48,1830,AMBER,message,984);
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
}
static void draw_confirmation(void){
    rect(0,0,W,H,BG);pixel_block(48,230,10);
    const char *title=confirm==1?"Restart server?":confirm==2?"Stop server?":confirm==3?"Remove player?":confirm==4?"Create backup?":confirm==6?"Enable empty whitelist?":"Restore backup?";
    text(bold,52,48,460,WHITE,title,984);
    if(confirm==3){text(mono,40,48,590,GREEN,selected_name,984);text(regular,31,48,690,MUTED,"This removes their whitelist access.",984);}
    else if(confirm==6){text(regular,31,48,590,RED,"An empty whitelist blocks everyone.",984);text(regular,30,48,665,MUTED,"No player names have been allowed yet.",984);text(regular,30,48,740,MUTED,"Add players first, or enable it anyway.",984);}
    else if(confirm==5){text(regular,32,48,590,RED,"The current world will be replaced.",984);text(regular,30,48,665,MUTED,"The server will restart; players disconnect.",984);text(regular,28,48,760,MUTED,"Selected backup",984);text(regular,30,48,815,WHITE,selected_backup_label,984);text(mono,23,48,870,DIM,selected_backup,984);}
    else if(confirm==4){text(regular,31,48,590,MUTED,"Copies the last files saved to disk.",984);text(regular,31,48,665,MUTED,"The server keeps running.",984);text(regular,31,48,740,MUTED,"Unsaved changes may not be included.",984);}
    else {text(regular,31,48,590,MUTED,"The world will be saved first.",984);text(regular,31,48,665,MUTED,"Players will be disconnected.",984);}
    control(48,1100,984,144,confirm==1||confirm==4?GREEN_DARK:RED_DARK,WHITE,confirm==6?"ENABLE EMPTY WHITELIST":confirm==5?"RESTORE SELECTED BACKUP":confirm==4?"BACK UP SAVED FILES":confirm==3?"REMOVE ACCESS":confirm==1?"RESTART SERVER":"STOP SERVER",A_CONFIRM,0,NULL);
    control(48,1290,984,144,PANEL,WHITE,"CANCEL",A_CANCEL,0,NULL);
}
static void draw(void){
    int active=running();last_server=active;hit_count=0;rect(0,0,W,H,BG);
    if(confirm){draw_confirmation();return;}if(keyboard_open){draw_keyboard();return;}
    pixel_block(48,65,7);text(bold,39,130,111,WHITE,"MINECRAFT",650);
    const char *status=!fresh()?"UNKNOWN":!strcmp(state.server,"online")?"ONLINE":!strcmp(state.server,"starting")?"STARTING":"OFFLINE";
    uint32_t status_color=fresh()&&!strcmp(state.server,"online")?GREEN:AMBER;
    text(bold,26,800,106,status_color,status,245);divider(154);
    if(page==HOME){
        heading("Your world");char address[100];network_addr(address,sizeof(address));text(mono,32,48,360,MUTED,address,984);
        char title[100];if(fresh())snprintf(title,sizeof(title),"Players  %d / %d",state.players,state.max_players);else snprintf(title,sizeof(title),"Players unavailable");text(bold,42,48,505,WHITE,title,984);
        if(fresh()&&state.players==0)text(regular,34,48,615,MUTED,"No players online",984);
        if(fresh())for(int i=0;i<state.players&&i<7;i++){if(state.player[i][0])player_row(570+i*130,state.player[i],0);else {text(regular,32,48,635+i*130,MUTED,"Player names unavailable",984);break;}}
        control(48,1520,984,120,PANEL,GREEN,"PLAYERS & WHITELIST",A_PAGE,PLAYERS,NULL);
        long bat=read_long("/sys/class/power_supply/battery/capacity",-1),temp=read_long("/sys/class/power_supply/battery/temp",-1);char u[40],health[160];uptime_str(u,sizeof(u));
        char battery_text[24],temperature_text[24];if(bat>=0)snprintf(battery_text,sizeof(battery_text),"%ld%%",bat);else copy_value(battery_text,sizeof(battery_text),"--");if(temp>=0)snprintf(temperature_text,sizeof(temperature_text),"%.1f C",temp/10.0);else copy_value(temperature_text,sizeof(temperature_text),"--");snprintf(health,sizeof(health),"Battery %s    %s    Phone %s",battery_text,temperature_text,u);text(regular,27,48,1740,MUTED,health,984);
        if(active){action_button(48,1840,477,144,"RESTART",A_RESTART,0,NULL);control(555,1840,477,144,busy()?PANEL:RED_DARK,busy()?DIM:RED,"STOP",busy()?0:A_STOP,0,NULL);}
        else action_button(48,1840,984,144,"START SERVER",A_START,0,NULL);
        control(760,2030,272,90,PANEL,MUTED,"SERVER LOG",A_PAGE,LOGS,NULL);
    }else if(page==PLAYERS){
        heading("Players");control(48,355,477,120,list_mode?PANEL:GREEN_DARK,list_mode?MUTED:GREEN,"ONLINE",A_LIST,0,NULL);control(555,355,477,120,list_mode?GREEN_DARK:PANEL,list_mode?GREEN:MUTED,"WHITELIST",A_LIST,1,NULL);
        int count=list_mode?state.allow_count:bounded(state.players,0,64);char count_text[80];snprintf(count_text,sizeof(count_text),"%d %s",count,list_mode?"allowed players":"online players");text(regular,31,48,550,MUTED,fresh()?count_text:"Player data unavailable",984);
        if(fresh())for(int i=list_offset;i<count&&i<list_offset+9;i++){const char *name=list_mode?state.allow[i]:state.player[i];if(name[0])player_row(595+(i-list_offset)*132,name,list_mode);else if(!list_mode){text(regular,32,48,660+(i-list_offset)*132,MUTED,"Player names unavailable",984);break;}}
        if(fresh()&&!count)text(regular,32,48,685,MUTED,list_mode?"No players on the whitelist":"No players online",984);
        control(48,1845,230,110,PANEL,MUTED,"PREV",A_SCROLL,-9,NULL);control(308,1845,230,110,PANEL,MUTED,"NEXT",A_SCROLL,9,NULL);
        action_button(568,1845,464,110,"ADD PLAYER",A_ADD,0,NULL);
        char wl[80];snprintf(wl,sizeof(wl),"Whitelist: %s",state.whitelist[0]?state.whitelist:"unknown");text(regular,30,48,2050,MUTED,wl,750);
    }else if(page==SETTINGS){
        heading("Settings");text(bold,26,48,390,GREEN,"DISPLAY",984);
        stepper(435,"Brightness",bounded(saved_brightness*100/4095,1,100),A_BRIGHT,"%");stepper(575,"Sleep after",idle_seconds,A_TIMEOUT," s");
        text(bold,26,48,790,GREEN,"SERVER",984);text(regular,32,48,880,WHITE,"Whitelist",550);action_button(650,815,382,115,state.whitelist[0]?state.whitelist:"unknown",A_ALLOW,1,NULL);
        text(regular,32,48,1030,WHITE,"Difficulty",550);action_button(650,965,382,115,state.difficulty[0]?state.difficulty:"unknown",A_DIFFICULTY,0,NULL);
        stepper(1145,"Max players",state.max_players,A_MAX,NULL);stepper(1285,"View distance",state.view_distance,A_VIEW,NULL);stepper(1425,"Simulation distance",state.simulation_distance,A_SIM,NULL);
        text(regular,27,48,1590,MUTED,"Player limit and distances apply after restart.",984);
        control(48,1630,477,125,PANEL,GREEN,"BACKUPS",A_PAGE,BACKUPS,NULL);
        control(555,1630,477,125,PANEL,GREEN,"WI-FI",A_PAGE,WIFI,NULL);
        char sship[80]="192.168.137.2",sshline[120],row[160];FILE *wf=fopen("/run/n20/wifi-state","r");if(wf){while(fgets(row,sizeof(row),wf)){if(!strncmp(row,"IP\t",3)){row[strcspn(row,"\r\n")]=0;if(row[3])snprintf(sship,sizeof(sship),"%.15s",row+3);break;}}fclose(wf);}snprintf(sshline,sizeof(sshline),"%s:22",sship);text(regular,25,48,1910,DIM,"SSH address",984);text(mono,31,48,1970,MUTED,sshline,984);
    }else if(page==WIFI){
        heading("Wi-Fi");FILE*f=fopen("/run/n20/wifi-state","r");char line[512],status[80]="Starting",ip[64]="",current[80]="",note[120]="";int count=0,shown=0;
        if(f){while(fgets(line,sizeof(line),f)){line[strcspn(line,"\r\n")]=0;char *cursor=line,*type=strsep(&cursor,"\t"),*value=strsep(&cursor,"\t");if(!value)continue;
            if(!strcmp(type,"STATE"))snprintf(status,sizeof(status),"%.79s",value);
            else if(!strcmp(type,"IP"))snprintf(ip,sizeof(ip),"%.63s",value);
            else if(!strcmp(type,"CURRENT"))hex_decode(value,current,sizeof(current));
            else if(!strcmp(type,"MESSAGE"))snprintf(note,sizeof(note),"%.119s",value);
            else if(!strcmp(type,"NETWORK")){char *kind=strsep(&cursor,"\t"),*signal=strsep(&cursor,"\t");if(!kind||!signal)continue;int idx=count++;if(idx<list_offset||shown>=8)continue;char name[80],arg[96],detail[80];if(!hex_decode(value,name,sizeof(name)))continue;
                int y=650+shown*135;snprintf(arg,sizeof(arg),"%.64s",value);snprintf(detail,sizeof(detail),"%.20s  %s dBm",kind,signal);text(bold,30,58,y+40,WHITE,name,660);text(regular,23,58,y+78,MUTED,detail,660);control(760,y,270,100,PANEL,GREEN,"SELECT",A_WIFI_SELECT,!strcmp(kind,"OPEN")?1:!strcmp(kind,"WPA2")?2:0,arg);divider(y+113);shown++;}}
            fclose(f);}
        char top[180];snprintf(top,sizeof(top),"%s%s%s",current[0]?current:"Not connected",ip[0]?"  ":"",ip);text(regular,30,48,355,WHITE,top,984);text(regular,26,48,410,MUTED,note[0]?note:status,984);
        control(48,465,477,110,PANEL,GREEN,"SCAN",A_WIFI_SCAN,0,NULL);control(555,465,477,110,PANEL,MUTED,"DISCONNECT",A_WIFI_DISCONNECT,0,NULL);
        control(48,1850,477,110,PANEL,MUTED,"PREV",A_SCROLL,-8,NULL);control(555,1850,477,110,PANEL,MUTED,"NEXT",A_SCROLL,8,NULL);
    }else if(page==BACKUPS){
        heading("Backups");text(regular,30,48,365,MUTED,"Select a saved world to restore.",984);
        action_button(48,420,984,125,"CREATE BACKUP",A_BACKUP,0,NULL);
        if(!fresh())text(regular,31,48,660,MUTED,"Backup list unavailable",984);
        else if(!state.backup_count)text(regular,31,48,660,MUTED,"No backups yet",984);
        else for(int i=list_offset;i<state.backup_count&&i<list_offset+8;i++){int y=615+(i-list_offset)*165;text(regular,30,48,y+43,WHITE,state.backup[i],710);text(mono,22,48,y+85,DIM,state.backup_id[i],700);if(valid_backup_id(state.backup_id[i]))action_button(790,y,242,115,"RESTORE",A_RESTORE,0,state.backup_id[i]);divider(y+130);}
        control(48,1985,477,120,PANEL,MUTED,"PREV",A_SCROLL,-8,NULL);control(555,1985,477,120,PANEL,MUTED,"NEXT",A_SCROLL,8,NULL);
    }else{
        heading("Server log");char logs[12][160];recent_logs(logs,12);
        for(int i=0;i<12;i++){int y=400+i*136;size_t len=strlen(logs[i]);char first[61];snprintf(first,sizeof(first),"%.60s",logs[i]);text(mono,25,48,y,MUTED,first,984);if(len>60)text(mono,25,48,y+40,DIM,logs[i]+60,984);}
    }
    const char *notice=state.operation[0]?state.operation:state.error[0]?state.error:message_until>now_ms()?message:!fresh()?"Waiting for current server data...":"";
    if(*notice)text(regular,25,48,2160,AMBER,notice,984);
    const char *tabs[]={"SERVER","PLAYERS","SETTINGS"};for(int i=0;i<3;i++)control(24+i*352,2220,328,140,page==i?GREEN_DARK:PANEL,page==i?GREEN:MUTED,tabs[i],A_PAGE,i,NULL);
}
static int server_command(const char *cmd,int mark_stop){
    int fd=open("/run/minecraft-console",O_WRONLY|O_NONBLOCK|O_CLOEXEC);if(fd<0){msg("Could not reach the server console.");return 0;}
    if(mark_stop){int m=open("/run/minecraft-stop",O_WRONLY|O_CREAT|O_CLOEXEC,0600);if(m<0){close(fd);msg("Could not stop the supervisor.");return 0;}close(m);}
    ssize_t n=write(fd,cmd,strlen(cmd));close(fd);if(n!=(ssize_t)strlen(cmd)){if(mark_stop)unlink("/run/minecraft-stop");msg("Server command failed.");return 0;}return 1;
}
static void activate(const Hit *hit){
    int a=hit->action,v=hit->value;const char *arg=hit->arg;char n[32];
    if(a==A_PAGE){page=v;list_offset=0;return;}
    if(a==A_CANCEL){confirm=0;keyboard_open=0;explicit_bzero(wifi_password,sizeof(wifi_password));return;}
    if(a==A_LIST){list_mode=v;list_offset=0;return;}
    if(a==A_SCROLL&&page==WIFI){list_offset=bounded(list_offset+v,0,24);return;}
    if(a==A_SCROLL){int count=page==BACKUPS?state.backup_count:list_mode?state.allow_count:bounded(state.players,0,64);int step=page==BACKUPS?8:9;list_offset=bounded(list_offset+v,0,count?((count-1)/step)*step:0);return;}
    if(a==A_KEY){char *target=keyboard_open==2?wifi_password:typed_name;size_t max=keyboard_open==2?64:16,len=strlen(target);if(len<max){target[len]=(char)v;target[len+1]=0;}return;}
    if(a==A_WIFI_SPACE&&keyboard_open==2){size_t len=strlen(wifi_password);if(len<64){wifi_password[len]=' ';wifi_password[len+1]=0;}return;}
    if(a==A_WIFI_SCAN){if(wifi_send("SCAN"))msg("Scanning...");else msg("Wi-Fi service unavailable.");return;}
    if(a==A_WIFI_DISCONNECT){if(wifi_send("DISCONNECT"))msg("Disconnecting...");return;}
    if(a==A_WIFI_SELECT){if(!v){msg("This network type is not supported yet.");return;}snprintf(wifi_selected,sizeof(wifi_selected),"%.64s",arg);hex_decode(arg,wifi_label,sizeof(wifi_label));explicit_bzero(wifi_password,sizeof(wifi_password));if(v==1)wifi_connect("","OPEN");else{keyboard_open=2;wifi_keyboard_mode=0;}return;}
    if(a==A_SUBMIT&&keyboard_open==2){size_t len=strlen(wifi_password);if(len<8){msg("Use at least 8 characters.");return;}wifi_connect(wifi_password,"WPA2");explicit_bzero(wifi_password,sizeof(wifi_password));keyboard_open=0;return;}
    if(a==A_SHIFT){if(keyboard_open==2)wifi_keyboard_mode=(wifi_keyboard_mode+1)%3;else keyboard_shift=!keyboard_shift;return;}
    if(a==A_ERASE){char *target=keyboard_open==2?wifi_password:typed_name;size_t len=strlen(target);if(len)target[len-1]=0;return;}
    if(a==A_BRIGHT){saved_brightness=bounded(saved_brightness+v*205,205,4095);brightness(saved_brightness);save_display_settings();return;}
    if(a==A_TIMEOUT){idle_seconds=bounded(idle_seconds+v*30,30,600);save_display_settings();return;}
    if(busy()){msg("Please wait for the current operation.");return;}
    if(a==A_START){if(unlink("/run/minecraft-stop")==0||errno==ENOENT)msg("Starting server...");else msg("Could not start server.");helper_due=0;return;}
    if(a==A_RESTART){confirm=1;return;}if(a==A_STOP){confirm=2;return;}
    if(a==A_CONFIRM){int c=confirm;confirm=0;input_block_until=now_ms()+600;
        if(c==1||c==2){if(server_command("stop\n",c==2))msg(c==1?"Saving world and restarting...":"Saving world and stopping...");}
        else if(c==3)admin("whitelist-remove",selected_name);
        else if(c==4)admin("backup",NULL);
        else if(c==6)admin("whitelist-set","on");
        else if(c==5&&valid_backup_id(selected_backup))admin("restore",selected_backup);
        return;
    }
    if(!fresh()){msg("Wait for current server data.");return;}
    if(a==A_ADD){typed_name[0]=0;keyboard_open=1;return;}
    if(a==A_REMOVE){copy_value(selected_name,sizeof(selected_name),arg);confirm=3;return;}
    if(a==A_ALLOW){if(v){if(strcmp(state.whitelist,"on")&&state.allow_count==0){confirm=6;return;}admin("whitelist-set",!strcmp(state.whitelist,"on")?"off":"on");}else admin("whitelist-add",arg);return;}
    if(a==A_SUBMIT){if(strlen(typed_name)<3){msg("Use a username with 3 to 16 characters.");return;}if(admin("whitelist-add",typed_name))keyboard_open=0;return;}
    if(a==A_DIFFICULTY){const char *levels[]={"peaceful","easy","normal","hard"};int i=0;for(;i<4;i++)if(!strcmp(levels[i],state.difficulty))break;admin("difficulty",levels[(i+1)%4]);return;}
    if(a==A_MAX||a==A_VIEW||a==A_SIM){int old=a==A_MAX?state.max_players:a==A_VIEW?state.view_distance:state.simulation_distance;snprintf(n,sizeof(n),"%d",bounded(old+v,a==A_MAX?1:2,a==A_MAX?20:12));admin(a==A_MAX?"max-players":a==A_VIEW?"view-distance":"simulation-distance",n);return;}
    if(a==A_BACKUP){confirm=4;return;}if(a==A_RESTORE&&valid_backup_id(arg)){copy_value(selected_backup,sizeof(selected_backup),arg);selected_backup_label[0]=0;for(int i=0;i<state.backup_count;i++)if(!strcmp(state.backup_id[i],arg)){copy_value(selected_backup_label,sizeof(selected_backup_label),state.backup[i]);break;}confirm=5;return;}
}
static int hit_at(int x,int y){for(int i=hit_count-1;i>=0;i--)if(x>=hits[i].x&&x<hits[i].x+hits[i].w&&y>=hits[i].y&&y<hits[i].y+hits[i].h)return i;return -1;}
static void wake_animation(long long now){
    long long age=now-wake_started;if(age<0||age>=360)return;
    int r=20+(int)age/3,cx=W/2,cy=H/2;uint32_t c=blend(BG,GREEN,(unsigned)(100*(360-age)/360));
    rect(cx-r,cy-r,30,6,c);rect(cx-r,cy-r,6,30,c);rect(cx+r-30,cy-r,30,6,c);rect(cx+r-6,cy-r,6,30,c);
    rect(cx-r,cy+r-6,30,6,c);rect(cx-r,cy+r-30,6,30,c);rect(cx+r-30,cy+r-6,30,6,c);rect(cx+r-6,cy+r-30,6,30,c);
}
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
    memset(mapped,0,map_size);
    if(brightness(0)!=0)fprintf(stderr,"using black framebuffer without brightness control\n");
    awake=0;touch_down=0;fprintf(stderr,"screen darkened; touch remains active\n");
}
static void wake(void){
    long long now=now_ms();last_activity=now;
    if(!awake){awake=1;wake_started=now;input_block_until=now+850;touch_down=0;pressed_valid=0;helper_due=0;read_state();draw();wake_animation(now);memcpy(mapped,canvas,map_size);brightness(saved_brightness);fprintf(stderr,"screen awakened\n");}
    last_draw=0;
}
static int open_input(const char*path){return open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC);}
static void events(int fd,int power){
    struct input_event ev[32];ssize_t n;
    while((n=read(fd,ev,sizeof(ev)))>0)for(size_t i=0;i<(size_t)n/sizeof(ev[0]);i++){
        struct input_event *e=&ev[i];
        if(power){if(e->type==EV_KEY&&e->code==KEY_POWER&&e->value==1)wake();continue;}
        if(e->type==EV_KEY&&(e->code==KEY_WAKEUP||e->code==KEY_F4)&&e->value==1){wake();touch_down=0;pressed_valid=0;input_block_until=now_ms()+850;continue;}
        if(e->type==EV_ABS){
            if(e->code==ABS_MT_POSITION_X||e->code==ABS_X)touch_x=e->value;
            if(e->code==ABS_MT_POSITION_Y||e->code==ABS_Y)touch_y=e->value;
            if(touch_down){int x=(int)((long long)(touch_x-xmin)*(W-1)/(xmax-xmin));int y=(int)((long long)(touch_y-ymin)*(H-1)/(ymax-ymin));if(abs(x-down_x)>35||abs(y-down_y)>35)drag_cancel=1;}
        }
        if(e->type==EV_KEY&&e->code==BTN_TOUCH){if(e->value==1)pending_touch_down=1;else if(e->value==0)pending_touch_up=1;}
        if(e->type==EV_SYN&&e->code==SYN_REPORT){
            if(pending_touch_down){pending_touch_down=0;
                long long now=now_ms();last_activity=now;
                if(!awake){wake();touch_down=0;pressed_valid=0;continue;}
                if(now<input_block_until){touch_down=0;pressed_valid=0;continue;}
                down_x=(int)((long long)(touch_x-xmin)*(W-1)/(xmax-xmin));down_y=(int)((long long)(touch_y-ymin)*(H-1)/(ymax-ymin));
                int index=hit_at(down_x,down_y);pressed_valid=index>=0;if(pressed_valid)pressed_hit=hits[index];touch_down=1;drag_cancel=0;last_draw=0;
            }else if(pending_touch_up){pending_touch_up=0;
                if(touch_down&&awake&&pressed_valid&&!drag_cancel&&now_ms()>=input_block_until){
                    int x=(int)((long long)(touch_x-xmin)*(W-1)/(xmax-xmin));int y=(int)((long long)(touch_y-ymin)*(H-1)/(ymax-ymin));int index=hit_at(x,y);
                    if(index>=0&&hits[index].action==pressed_hit.action&&hits[index].value==pressed_hit.value&&!strcmp(hits[index].arg,pressed_hit.arg)){activate(&pressed_hit);input_block_until=now_ms()+180;}
                }
                touch_down=0;pressed_valid=0;last_activity=now_ms();last_draw=0;
            }
        }
    }
}
static int setup_display(void){
    drmfd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);if(drmfd<0){perror("drm open");return 0;}drmModeRes *res=drmModeGetResources(drmfd);if(!res)return 0;drmModeConnector*c=NULL;
    for(int i=0;i<res->count_connectors;i++){drmModeConnector*t=drmModeGetConnector(drmfd,res->connectors[i]);if(t&&t->connection==DRM_MODE_CONNECTED&&t->count_modes){c=t;break;}if(t)drmModeFreeConnector(t);}if(!c){drmModeFreeResources(res);return 0;}
    conn_id=c->connector_id;mode=c->modes[0];if(mode.hdisplay!=W||mode.vdisplay!=H){fprintf(stderr,"unsupported mode %dx%d\n",mode.hdisplay,mode.vdisplay);return 0;}
    drmModeEncoder*enc=drmModeGetEncoder(drmfd,c->encoder_id);if(enc){crtc_id=enc->crtc_id;drmModeFreeEncoder(enc);}if(!crtc_id&&res->count_crtcs)crtc_id=res->crtcs[0];
    for(int i=0;i<c->count_props;i++){drmModePropertyRes*p=drmModeGetProperty(drmfd,c->props[i]);if(p){if(!strcmp(p->name,"DPMS"))dpms_id=p->prop_id;drmModeFreeProperty(p);}}
    drmModeFreeConnector(c);drmModeFreeResources(res);
    struct drm_mode_create_dumb create={0};create.width=W;create.height=H;create.bpp=32;if(drmIoctl(drmfd,DRM_IOCTL_MODE_CREATE_DUMB,&create)){perror("create dumb");return 0;}handle=create.handle;pitch=create.pitch;map_size=(size_t)create.size;
    if(drmModeAddFB(drmfd,W,H,24,32,pitch,handle,&fb_id)){perror("add fb");return 0;}struct drm_mode_map_dumb map={0};map.handle=handle;if(drmIoctl(drmfd,DRM_IOCTL_MODE_MAP_DUMB,&map)){perror("map dumb");return 0;}
    mapped=mmap(NULL,map_size,PROT_READ|PROT_WRITE,MAP_SHARED,drmfd,map.offset);if(mapped==MAP_FAILED){mapped=NULL;perror("mmap");return 0;}
    canvas=calloc(1,map_size);if(!canvas){perror("canvas");return 0;}
    draw();memcpy(mapped,canvas,map_size);if(drmModeSetCrtc(drmfd,crtc_id,fb_id,0,0,&conn_id,1,&mode)){perror("set crtc");return 0;}if(dpms_id)display_power(1);return 1;
}
static void range(int fd,int axis,int *lo,int *hi){struct input_absinfo a;if(ioctl(fd,EVIOCGABS(axis),&a)==0&&a.maximum>a.minimum){*lo=a.minimum;*hi=a.maximum;}}
int main(void){
    signal(SIGTERM,on_signal);signal(SIGINT,on_signal);signal(SIGUSR1,on_sleep_signal);if(FT_Init_FreeType(&ft)||FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",0,&regular)||FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",0,&bold)||FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",0,&mono)){fprintf(stderr,"font setup failed\n");return 1;}
    if(!setup_display()){fprintf(stderr,"display setup failed\n");return 1;}touchfd=open_input("/dev/input/event2");powerfd=open_input("/dev/input/event0");if(touchfd>=0){range(touchfd,ABS_MT_POSITION_X,&xmin,&xmax);range(touchfd,ABS_MT_POSITION_Y,&ymin,&ymax);}fprintf(stderr,"input touch=%d power=%d range=%d..%d,%d..%d dpms=%u\n",touchfd,powerfd,xmin,xmax,ymin,ymax,dpms_id);
    long initial_brightness=read_long("/sys/class/backlight/panel0-backlight/brightness",1215);
    if(initial_brightness>0 && initial_brightness<=4095)saved_brightness=(int)initial_brightness;
    load_display_settings();
    brightness(saved_brightness);
    int gesture=open("/proc/touchpanel/double_tap_enable",O_WRONLY|O_CLOEXEC);
    if(gesture>=0){if(write(gesture,"0\n",2)!=2)perror("disable touch gesture mode");close(gesture);}
    read_state();last_activity=now_ms();last_draw=0;
    while(!exiting){
        long long before=now_ms();int animating=awake&&before-wake_started<400;
        struct pollfd p[2]={{touchfd,POLLIN,0},{powerfd,POLLIN,0}};poll(p,2,animating?33:250);
        if(touchfd>=0&&(p[0].revents&POLLIN))events(touchfd,0);
        if(powerfd>=0&&(p[1].revents&POLLIN))events(powerfd,1);
        long long now=now_ms();tick_helper(now);
        if(sleep_requested){sleep_requested=0;darken();}
        if(awake&&now-last_activity>idle_seconds*1000LL)darken();
        if(awake&&(last_draw==0||animating||now-last_draw>=5000||last_server!=running())){
            draw();wake_animation(now);
            if(touch_down&&pressed_valid&&!drag_cancel){Hit *h=&pressed_hit;rect(h->x,h->y,h->w,4,GREEN);rect(h->x,h->y+h->h-4,h->w,4,GREEN);}
            memcpy(mapped,canvas,map_size);last_draw=now;
        }
    }
    brightness(saved_brightness);
    display_power(0);
    if(touchfd>=0) close(touchfd);
    if(powerfd>=0) close(powerfd);
    if(canvas) free(canvas);
    if(mapped) munmap(mapped,map_size);
    if(fb_id) drmModeRmFB(drmfd,fb_id);
    if(handle){struct drm_mode_destroy_dumb d={.handle=handle};drmIoctl(drmfd,DRM_IOCTL_MODE_DESTROY_DUMB,&d);}
    if(drmfd>=0) close(drmfd);
    return 0;
}
