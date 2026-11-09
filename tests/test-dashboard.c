/* Headless checks: compile on the target with the dashboard's libraries. */
#define main dashboard_main
#include "../dashboard/n20-dashboard.c"
#undef main
#include <assert.h>

static void event_write(int fd,unsigned type,unsigned code,int value){
    struct input_event e={.type=type,.code=code,.value=value};assert(write(fd,&e,sizeof(e))==(ssize_t)sizeof(e));
}
static void tap(int fd,int x,int y){
    event_write(fd,EV_ABS,ABS_MT_POSITION_X,x);event_write(fd,EV_ABS,ABS_MT_POSITION_Y,y);
    event_write(fd,EV_KEY,BTN_TOUCH,1);event_write(fd,EV_SYN,SYN_REPORT,0);
    event_write(fd,EV_KEY,BTN_TOUCH,0);event_write(fd,EV_SYN,SYN_REPORT,0);
}
static void check_input(void){
    int p[2];assert(pipe(p)==0);assert(fcntl(p[0],F_SETFL,O_NONBLOCK)==0);
    keyboard_open=1;typed_name[0]=0;awake=1;input_block_until=0;draw();
    tap(p[1],60,680);tap(p[1],60,680);events(p[0],0);assert(!strcmp(typed_name,"11"));
    event_write(p[1],EV_ABS,ABS_MT_POSITION_X,60);event_write(p[1],EV_ABS,ABS_MT_POSITION_Y,680);
    event_write(p[1],EV_KEY,BTN_TOUCH,1);event_write(p[1],EV_SYN,SYN_REPORT,0);
    event_write(p[1],EV_ABS,ABS_MT_POSITION_X,220);event_write(p[1],EV_SYN,SYN_REPORT,0);
    event_write(p[1],EV_KEY,BTN_TOUCH,0);event_write(p[1],EV_SYN,SYN_REPORT,0);events(p[0],0);
    assert(!strcmp(typed_name,"11"));
    /* A second contact cancels the first tap rather than moving its target. */
    event_write(p[1],EV_ABS,ABS_MT_SLOT,0);event_write(p[1],EV_ABS,ABS_MT_TRACKING_ID,101);
    event_write(p[1],EV_ABS,ABS_MT_POSITION_X,60);event_write(p[1],EV_ABS,ABS_MT_POSITION_Y,680);
    event_write(p[1],EV_KEY,BTN_TOUCH,1);event_write(p[1],EV_SYN,SYN_REPORT,0);
    event_write(p[1],EV_ABS,ABS_MT_SLOT,1);event_write(p[1],EV_ABS,ABS_MT_TRACKING_ID,102);
    event_write(p[1],EV_ABS,ABS_MT_POSITION_X,220);event_write(p[1],EV_SYN,SYN_REPORT,0);
    event_write(p[1],EV_ABS,ABS_MT_TRACKING_ID,-1);event_write(p[1],EV_ABS,ABS_MT_SLOT,0);
    event_write(p[1],EV_ABS,ABS_MT_TRACKING_ID,-1);event_write(p[1],EV_KEY,BTN_TOUCH,0);event_write(p[1],EV_SYN,SYN_REPORT,0);
    events(p[0],0);assert(!strcmp(typed_name,"11"));
    tap(p[1],60,680);events(p[0],0);assert(!strcmp(typed_name,"111"));
    /* Lost input cancels a press and requires release before accepting more. */
    event_write(p[1],EV_KEY,BTN_TOUCH,1);event_write(p[1],EV_SYN,SYN_REPORT,0);
    event_write(p[1],EV_SYN,SYN_DROPPED,0);event_write(p[1],EV_SYN,SYN_REPORT,0);
    event_write(p[1],EV_KEY,BTN_TOUCH,0);event_write(p[1],EV_SYN,SYN_REPORT,0);events(p[0],0);assert(!strcmp(typed_name,"111"));
    tap(p[1],60,680);events(p[0],0);assert(!strcmp(typed_name,"1111"));
    /* A powered-off panel must not wake or edit text on a touch event. */
    awake=0;panel_off=1;tap(p[1],60,680);events(p[0],0);
    assert(!awake&&!strcmp(typed_name,"1111"));awake=1;panel_off=0;
    keyboard_open=0;close(p[0]);close(p[1]);
}
static void check_signal_wakeup(void){
    assert(pipe2(signal_pipe,O_NONBLOCK|O_CLOEXEC)==0);
    void (*old)(int)=signal(SIGUSR1,on_sleep_signal);sleep_requested=0;
    assert(raise(SIGUSR1)==0);struct pollfd p={signal_pipe[0],POLLIN,0};assert(poll(&p,1,1000)==1);
    assert(sleep_requested);char byte;assert(read(signal_pipe[0],&byte,1)==1);
    signal(SIGUSR1,old);close(signal_pipe[0]);close(signal_pipe[1]);signal_pipe[0]=signal_pipe[1]=-1;sleep_requested=0;
}
static int test_power_presses;
static int fake_wifi_failure(const char *command){(void)command;return 0;}
static void fake_power_button(void){test_power_presses++;awake=!awake;}
static void check_power_button(void){
    int p[2];assert(pipe(p)==0);assert(fcntl(p[0],F_SETFL,O_NONBLOCK)==0);
    power_button_action=fake_power_button;test_power_presses=0;awake=1;
    event_write(p[1],EV_KEY,KEY_POWER,1);event_write(p[1],EV_KEY,KEY_POWER,2);event_write(p[1],EV_KEY,KEY_POWER,0);
    events(p[0],1);assert(test_power_presses==1&&!awake);
    event_write(p[1],EV_KEY,KEY_POWER,1);events(p[0],1);assert(test_power_presses==2&&awake);
    power_button_action=toggle_display;close(p[0]);close(p[1]);
}
static void check_state_watch(void){
    char dir[]="/tmp/n20-ui-watch-XXXXXX";assert(mkdtemp(dir));
    assert(start_state_watch(dir));char tmp[256],dst[256];
    snprintf(tmp,sizeof(tmp),"%s/tmp",dir);snprintf(dst,sizeof(dst),"%s/state",dir);
    int f=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);assert(f>=0);assert(write(f,"x",1)==1);close(f);
    state_read_due=12345;last_draw=12345;drain_state_watch();assert(state_read_due==12345&&last_draw==12345);
    assert(rename(tmp,dst)==0);struct pollfd p={state_watch_fd,POLLIN,0};assert(poll(&p,1,1000)==1);
    drain_state_watch();assert(state_read_due==0);assert(unlink(dst)==0);
    snprintf(dst,sizeof(dst),"%s/wifi-state",dir);f=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);assert(f>=0);close(f);
    assert(rename(tmp,dst)==0);page=WIFI;last_draw=12345;drain_state_watch();assert(last_draw==0);
    stop_state_watch();assert(state_watch_fd<0);assert(unlink(dst)==0);assert(rmdir(dir)==0);
}
static void check_poll_deadlines(void){
    assert(input_poll_failed(POLLERR)&&input_poll_failed(POLLHUP)&&input_poll_failed(POLLNVAL));assert(!input_poll_failed(POLLIN));
    long long now=1000;awake=1;page=TOOLS;last_draw=1000;last_activity=1000;idle_seconds=120;
    state_read_due=10000;helper_due=11000;helper_pid=0;display_save_due=0;display_save_pid=0;message_until=0;
    assert(poll_timeout(now)==5000);state_read_due=1300;assert(poll_timeout(now)==300);
    display_save_due=1100;assert(poll_timeout(now)==100);display_save_pid=123;assert(poll_timeout(now)==300);
    awake=0;display_save_due=0;assert(poll_timeout(now)==-1);
    awake=1;display_save_pid=0;state_read_due=helper_due=0;last_draw=0;
}
static void check_display_save_failure(void){
    for(int pending=0;pending<=1;pending++){
        pid_t child=fork();assert(child>=0);if(!child)_exit(1);
        siginfo_t info={0};assert(waitid(P_PID,(id_t)child,&info,WEXITED|WNOWAIT)==0);
        display_save_pid=child;display_save_due=pending?2000:0;
        tick_display_save(1000);
        assert(display_save_pid==0);assert(display_save_due==(pending?2000:6000));
    }
    display_save_due=0;
}
static void check_wifi_stalled_receiver(void){
    char dir[]="/tmp/n20-ui-socket-XXXXXX";assert(mkdtemp(dir));char path[108];snprintf(path,sizeof(path),"%s/socket",dir);
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(fd>=0);
    struct sockaddr_un addr={.sun_family=AF_UNIX};snprintf(addr.sun_path,sizeof(addr.sun_path),"%s",path);
    assert(bind(fd,(struct sockaddr *)&addr,sizeof(addr))==0);
    int sent=0;long long start=now_ms();while(sent<100&&wifi_send_to(path,"SCAN"))sent++;
    assert(sent>0&&sent<100);assert(now_ms()-start<1000);
    close(fd);assert(unlink(path)==0);assert(rmdir(dir)==0);
}static void check_text(void){
    assert(external_power(1,0)==1&&external_power(0,1)==1&&external_power(0,0)==0);
    assert(external_power(0,-1)==-1&&external_power(1,-1)==1);
    char powerflag[]="/tmp/n20-power-flag-XXXXXX";int powerfd=mkstemp(powerflag);assert(powerfd>=0);
    assert(write(powerfd,"invalid\n",8)==8);close(powerfd);assert(read_power_flag(powerflag)==-1);assert(unlink(powerflag)==0);
    assert(setting_value("?")==-1&&setting_value("")==-1&&setting_value("12x")==-1);
    assert(setting_value("12")==12&&setting_value("10001")==-1);
    assert(!strcmp(battery_power_label(0,"Not charging",0),"On battery"));
    assert(!strcmp(battery_power_label(1,"Charging",0),"Plugged in, charging"));
    assert(!strcmp(battery_power_label(1,"Not charging",0),"Plugged in, charge paused"));
    assert(!strcmp(battery_power_label(1,"Not charging",1),"Plugged in, not charging"));
    assert(!strcmp(battery_power_label(1,"Not charging",-1),"Plugged in, not charging"));
    assert(!strcmp(battery_power_label(1,"Full",0),"Plugged in, full"));
    assert(!strcmp(battery_power_label(-1,"Charging",0),"Power status unavailable"));
    const char *s="A\xc3\xa9\xe4\xb8\xad";
    assert(next_codepoint(&s)=='A');assert(next_codepoint(&s)==0xe9);assert(next_codepoint(&s)==0x4e2d);assert(!*s);
    s="\xf0\x80\x80\x80";assert(next_codepoint(&s)=='?');
    s="\xe2";assert(next_codepoint(&s)=='?');assert(!*s);
    assert(page_offset(24,1,8)==0);assert(page_offset(24,9,8)==8);assert(page_offset(9,0,9)==0);
    char heartbeat[]="/tmp/n20-ui-heartbeat-XXXXXX";int fd=mkstemp(heartbeat);assert(fd>=0);close(fd);
    struct stat st;assert(stat(heartbeat,&st)==0);
    assert(wifi_power_policy_alive(heartbeat,st.st_mtime));
    assert(!wifi_power_policy_alive(heartbeat,st.st_mtime+7));
    assert(!wifi_power_policy_alive(heartbeat,st.st_mtime-3));
    assert(unlink(heartbeat)==0);assert(!wifi_power_policy_alive(heartbeat,st.st_mtime));
}
static void check_partial_keyboard(void){
    void *partial=malloc(map_size);assert(partial);confirm=notice_open=difficulty_open=0;message_until=0;
    for(int mode=1;mode<=2;mode++){
        keyboard_open=mode;strcpy(typed_name,"abc");strcpy(wifi_password,"abc12345");wifi_cursor=strlen(wifi_password);wifi_show_password=0;
        clip_top=0;clip_bottom=H;draw();
        Hit key={.x=48,.y=mode==2?620:660,.w=90,.h=130,.action=A_KEY,.value='1'};
        dirty_top=H;dirty_bottom=0;last_draw=1;invalidate_hit(&key,0);
        assert(dirty_top==key.y&&dirty_bottom==key.y+key.h);
        clip_top=dirty_top;clip_bottom=dirty_bottom;draw();rect(key.x,key.y,key.w,4,GREEN);rect(key.x,key.y+key.h-4,key.w,4,GREEN);
        memcpy(partial,canvas,map_size);clip_top=0;clip_bottom=H;draw();rect(key.x,key.y,key.w,4,GREEN);rect(key.x,key.y+key.h-4,key.w,4,GREEN);
        assert(!memcmp(partial,canvas,map_size));
        activate(&key);dirty_top=H;dirty_bottom=0;last_draw=1;invalidate_hit(&key,1);
        assert(dirty_top==400&&dirty_bottom<H);
        clip_top=dirty_top;clip_bottom=dirty_bottom;draw();memcpy(partial,canvas,map_size);
        clip_top=0;clip_bottom=H;draw();assert(!memcmp(partial,canvas,map_size));
    }
    explicit_bzero(wifi_password,sizeof(wifi_password));wifi_cursor=0;keyboard_open=0;dirty_top=0;dirty_bottom=H;free(partial);
}static void check_partial_buttons(void){
    void *partial=malloc(map_size);assert(partial);
    const int pages[]={PLAYERS,TOOLS,BACKUPS};
    confirm=keyboard_open=difficulty_open=notice_open=0;message_until=0;
    for(size_t p=0;p<sizeof(pages)/sizeof(pages[0]);p++){
        page=pages[p];list_offset=0;clip_top=0;clip_bottom=H;draw();int count=hit_count;
        for(int i=0;i<count;i++){
            draw();Hit hit=hits[i];dirty_top=H;dirty_bottom=0;last_draw=1;invalidate_hit(&hit,0);
            assert(dirty_top==hit.y&&dirty_bottom==hit.y+hit.h);
            clip_top=dirty_top;clip_bottom=dirty_bottom;draw();rect(hit.x,hit.y,hit.w,4,GREEN);rect(hit.x,hit.y+hit.h-4,hit.w,4,GREEN);
            memcpy(partial,canvas,map_size);clip_top=0;clip_bottom=H;draw();rect(hit.x,hit.y,hit.w,4,GREEN);rect(hit.x,hit.y+hit.h-4,hit.w,4,GREEN);
            assert(!memcmp(partial,canvas,map_size));
        }
    }
    dirty_top=0;dirty_bottom=H;free(partial);
}static void check_page(int target){
    page=target;list_offset=0;confirm=keyboard_open=0;draw();
    assert(hit_count>0 && hit_count<=100);
    for(int i=0;i<hit_count;i++){Hit *h=&hits[i];assert(h->x>=0&&h->y>=0&&h->x+h->w<=W&&h->y+h->h<=H);}
    int found=0;for(int i=0;i<hit_count;i++)if(hits[i].action==A_PAGE&&hits[i].value==TOOLS)found=1;
    assert(found);
}
int main(int argc,char **argv){
    check_text();check_signal_wakeup();check_power_button();check_state_watch();check_poll_deadlines();check_display_save_failure();check_wifi_stalled_receiver();assert(FT_Init_FreeType(&ft)==0);
    assert(FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",0,&regular)==0);
    assert(FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",0,&bold)==0);
    assert(FT_New_Face(ft,"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",0,&mono)==0);
    assert(text_width(bold,control_font_size(260,"REMOVE ACCESS"),"REMOVE ACCESS")<=244);
    char duration[80];sleep_duration_label(30,duration,sizeof(duration));assert(!strcmp(duration,"30 s"));
    sleep_duration_label(90,duration,sizeof(duration));assert(!strcmp(duration,"1 min 30 s"));
    sleep_duration_label(600,duration,sizeof(duration));assert(!strcmp(duration,"10 min"));
    int expected=0;assert(FT_Set_Pixel_Sizes(regular,0,31)==0);for(const char *p="Cache test";*p;p++){assert(FT_Load_Char(regular,(unsigned char)*p,FT_LOAD_DEFAULT)==0);expected+=(int)(regular->glyph->advance.x>>6);}assert(text_width(regular,31,"Cache test")==expected);
    for(int size=20;size<=60;size++)for(unsigned ch=32;ch<127;ch++)assert(load_glyph(regular,size,ch));
    assert(glyph_bytes<=GLYPH_BYTES);
    /* The target's dumb buffer has 32 bytes of padding per scanline. */
    pitch=W*4+32;map_size=(size_t)pitch*H;canvas=calloc(1,map_size);assert(canvas);
    state.valid=1;state.updated=(long)time(NULL);state.max_players=10;strcpy(state.server,"online");
    check_page(HOME);check_page(PLAYERS);check_page(TOOLS);check_page(SETTINGS);check_page(SERVER_SETTINGS);check_page(DEVICE_SETTINGS);check_page(WIFI);check_page(BACKUPS);check_page(LOGS);
    page=HOME;draw();assert(hit_at(70,450)<0);
    page=SETTINGS;draw();int server_settings=0,device_settings=0;
    for(int i=0;i<hit_count;i++){server_settings+=hits[i].action==A_PAGE&&hits[i].value==SERVER_SETTINGS;device_settings+=hits[i].action==A_PAGE&&hits[i].value==DEVICE_SETTINGS;assert(hits[i].action!=A_BRIGHT&&hits[i].action!=A_MAX);}
    assert(server_settings==1&&device_settings==1);
    page=SERVER_SETTINGS;draw();int max_setting=0;for(int i=0;i<hit_count;i++){max_setting+=hits[i].action==A_MAX;assert(hits[i].action!=A_BRIGHT&&hits[i].action!=A_RESTART);}assert(max_setting>0);
    page=DEVICE_SETTINGS;draw();int brightness=0;for(int i=0;i<hit_count;i++){brightness+=hits[i].action==A_BRIGHT;assert(hits[i].action!=A_MAX&&hits[i].action!=A_DIFFICULTY);}assert(brightness>0);
    page=HOME;
    strcpy(state.server,"offline");strcpy(state.error,"Server files unavailable: install protected bundle");draw();
    for(int i=0;i<hit_count;i++){assert(hits[i].action!=A_START);}assert(!start_ready());state.error[0]=0;strcpy(state.server,"online");
    /* A previously drawn Start hit cannot act on stale or changed state. */
    Hit stale_start={.action=A_START};state.updated=0;activate(&stale_start);
    assert(strstr(message,"state changed"));state.updated=(long)time(NULL);
    strcpy(state.server,"starting");message[0]=0;activate(&stale_start);
    assert(strstr(message,"state changed"));strcpy(state.server,"online");message_until=0;
    Hit stale_setting={.action=A_MAX,.value=1};state.max_players=-1;message[0]=0;activate(&stale_setting);
    assert(strstr(message,"Setting changed")&&!helper_pid);state.max_players=20;message[0]=0;activate(&stale_setting);
    assert(strstr(message,"Setting changed")&&!helper_pid);state.max_players=10;message_until=0;
    page=PLAYERS;list_mode=0;state.players=1;strcpy(state.player[0],"Alex");draw();
    for(int i=0;i<hit_count;i++)assert(hits[i].action!=A_ALLOW&&hits[i].action!=A_ADD&&hits[i].action!=A_REMOVE);
    list_mode=1;state.allow_count=1;strcpy(state.allow[0],"Alex");draw();
    int add_player=0,remove_player=0;for(int i=0;i<hit_count;i++){add_player+=hits[i].action==A_ADD;remove_player+=hits[i].action==A_REMOVE;}
    assert(add_player==1&&remove_player==1);state.allow_count=-1;draw();
    assert(!access_editable());for(int i=0;i<hit_count;i++)assert(hits[i].action!=A_ADD&&hits[i].action!=A_REMOVE&&hits[i].action!=A_ALLOW);
    state.allow_count=1;strcpy(state.server,"offline");draw();
    for(int i=0;i<hit_count;i++)assert(hits[i].action!=A_ADD&&hits[i].action!=A_REMOVE&&hits[i].action!=A_ALLOW);
    page=BACKUPS;state.backup_count=-1;draw();assert(!confirmation_ready(5));
    for(int i=0;i<hit_count;i++)assert(hits[i].action!=A_RESTORE);
    state.backup_count=1;strcpy(state.backup_id[0],"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");draw();
    int restore_hit=0;for(int i=0;i<hit_count;i++)if(hits[i].action==A_RESTORE){restore_hit=1;assert(!strcmp(hits[i].arg,state.backup_id[0]));}assert(restore_hit);
    state.backup_count=0;page=PLAYERS;
    strcpy(state.server,"online");list_mode=0;state.players=state.allow_count=0;
    if(server_identity(&confirm_server_pid,&confirm_server_start)){assert(confirmation_ready(1));confirm_server_start++;assert(!confirmation_ready(1));confirm_server_start--;}
    strcpy(state.whitelist,"on");assert(confirmation_ready(7));state.updated=0;assert(!confirmation_ready(7));state.updated=(long)time(NULL);
    strcpy(state.whitelist,"off");assert(confirmation_ready(6));strcpy(state.whitelist,"unknown");assert(!confirmation_ready(6));strcpy(state.whitelist,"on");
    wifi_sender=fake_wifi_failure;keyboard_open=2;strcpy(wifi_selected,"74657374");strcpy(wifi_password,"password123");wifi_cursor=strlen(wifi_password);
    Hit failed_connect={.action=A_SUBMIT};activate(&failed_connect);assert(keyboard_open==2&&!strcmp(wifi_password,"password123"));
    Hit failed_disconnect={.action=A_WIFI_DISCONNECT};activate(&failed_disconnect);assert(strstr(message,"unavailable"));
    explicit_bzero(wifi_password,sizeof(wifi_password));keyboard_open=0;wifi_sender=wifi_send;message_until=0;
    page=TOOLS;draw();for(int i=0;i<TOOL_COUNT&&i<4;i++){int found=0;for(int j=0;j<hit_count;j++)if(hits[j].action==A_PAGE&&hits[j].value==tool_entries[i].page){found=1;assert(hits[j].w==984&&hits[j].h==275);}assert(found);}
    /* Static pages and modals do not repaint merely because time passed. */
    page=TOOLS;confirm=keyboard_open=difficulty_open=notice_open=0;message_until=0;draw();last_draw=1000;
    assert(!redraw_needed(7000));
    state.updated=0;assert(redraw_needed(7000));state.updated=(long)time(NULL);draw();last_draw=1000;
    long long test_now=now_ms();message_until=test_now+8000;draw();last_draw=test_now;assert(!redraw_needed(test_now+7000));assert(redraw_needed(test_now+9000));
    message_until=0;page=HOME;draw();last_draw=1000;assert(redraw_needed(7000));
    keyboard_open=1;draw();last_draw=1000;assert(!redraw_needed(7000));keyboard_open=0;
    check_partial_buttons();
    check_partial_keyboard();
    check_input();
    page=WIFI;wifi_count=9;list_offset=0;Hit next={.action=A_SCROLL,.value=8};activate(&next);assert(list_offset==8);activate(&next);assert(list_offset==8);
    Hit prev={.action=A_SCROLL,.value=-8};activate(&prev);assert(list_offset==0);
    assert(!wifi_password_valid("short"));assert(wifi_password_valid("validpass"));
    assert(wifi_connected_network("COMPLETED","My network","My network"));
    assert(!wifi_connected_network("ASSOCIATING","My network","My network"));
    assert(!wifi_connected_network("COMPLETED","My network","Other network"));
    assert(!wifi_connected_network("COMPLETED","",""));
    char ssh_label[80];wifi_ssh_label("COMPLETED","10.0.0.2",ssh_label,sizeof(ssh_label));assert(!strcmp(ssh_label,"10.0.0.2:22"));
    wifi_ssh_label("DISCONNECTED","10.0.0.2",ssh_label,sizeof(ssh_label));assert(!strcmp(ssh_label,"Wi-Fi unavailable"));
    wifi_ssh_label("COMPLETED","0.0.0.0",ssh_label,sizeof(ssh_label));assert(!strcmp(ssh_label,"Wi-Fi unavailable"));
    wifi_ssh_label("COMPLETED","bad-address",ssh_label,sizeof(ssh_label));assert(!strcmp(ssh_label,"Wi-Fi unavailable"));
    char wifi_file[]="/tmp/n20-wifi-state-test-XXXXXX",wifi_status[80],wifi_ip[80];int wifi_fd=mkstemp(wifi_file);assert(wifi_fd>=0);
    const char wifi_data[]="STATE\tCOMPLETED\nIP\t10.0.0.2\n";assert(write(wifi_fd,wifi_data,sizeof(wifi_data)-1)==sizeof(wifi_data)-1);
    FILE *wifi_snapshot=fopen(wifi_file,"r");assert(wifi_snapshot&&wifi_snapshot_recent(wifi_snapshot));fclose(wifi_snapshot);
    read_wifi_connection(wifi_file,wifi_status,sizeof(wifi_status),wifi_ip,sizeof(wifi_ip));assert(wifi_address_valid(wifi_status,wifi_ip));
    struct timespec stale[2]={{.tv_sec=time(NULL)-90},{.tv_sec=time(NULL)-90}};assert(futimens(wifi_fd,stale)==0);close(wifi_fd);
    wifi_snapshot=fopen(wifi_file,"r");assert(wifi_snapshot&&!wifi_snapshot_recent(wifi_snapshot));fclose(wifi_snapshot);
    read_wifi_connection(wifi_file,wifi_status,sizeof(wifi_status),wifi_ip,sizeof(wifi_ip));assert(!wifi_address_valid(wifi_status,wifi_ip));assert(unlink(wifi_file)==0);
    char storage[80];backup_space_label("/",storage,sizeof(storage));assert(strstr(storage,"GiB free"));
    backup_space_label("/nonexistent-n20-storage-test",storage,sizeof(storage));assert(strstr(storage,"unavailable"));
    memset(wifi_password,'g',64);wifi_password[64]=0;assert(!wifi_password_valid(wifi_password));
    keyboard_open=2;Hit wifi_submit={.action=A_SUBMIT};activate(&wifi_submit);assert(keyboard_open==2&&strlen(wifi_password)==64);
    memset(wifi_password,'a',64);assert(wifi_password_valid(wifi_password));char tail[80];wifi_password_window(wifi_password,64,tail,sizeof(tail));
    assert(!strncmp(tail,"...",3)&&strlen(tail)<64&&tail[strlen(tail)-1]=='|'&&text_width(mono,40,tail)<=650);
    strcpy(wifi_password,"abde");wifi_cursor=2;Hit key_c={.action=A_KEY,.value='c'};activate(&key_c);assert(!strcmp(wifi_password,"abcde")&&wifi_cursor==3);
    Hit left={.action=A_WIFI_LEFT};activate(&left);Hit erase={.action=A_ERASE};activate(&erase);assert(!strcmp(wifi_password,"acde")&&wifi_cursor==1);
    Hit right={.action=A_WIFI_RIGHT};activate(&right);assert(wifi_cursor==2);Hit space={.action=A_WIFI_SPACE};activate(&space);assert(!strcmp(wifi_password,"ac de")&&wifi_cursor==3);
    wifi_password_window(wifi_password,2,tail,sizeof(tail));assert(strchr(tail,'|')&&text_width(mono,40,tail)<=650);
    explicit_bzero(wifi_password,sizeof(wifi_password));keyboard_open=0;
    keyboard_open=2;wifi_show_password=0;Hit show={.action=A_WIFI_SHOW};activate(&show);assert(wifi_show_password);Hit cancel={.action=A_CANCEL};activate(&cancel);assert(!keyboard_open&&!wifi_show_password);
    page=SERVER_SETTINGS;Hit difficulty={.action=A_DIFFICULTY,.value=-1};activate(&difficulty);assert(difficulty_open);draw();
    int choices=0;for(int i=0;i<hit_count;i++)if(hits[i].action==A_DIFFICULTY)choices++;assert(choices==4);activate(&cancel);assert(!difficulty_open);
    difficulty_open=1;draw();void *reference=malloc(map_size);assert(reference);memcpy(reference,canvas,map_size);glyph_cache_enabled=0;draw();assert(!memcmp(reference,canvas,map_size));glyph_cache_enabled=1;difficulty_open=0;free(reference);
    state.settings_pending=1;draw();for(int i=0;i<hit_count;i++)assert(hits[i].action!=A_RESTART);state.settings_pending=0;
    strcpy(state.error,"Recovery requires attention");Hit notice={.action=A_NOTICE};activate(&notice);assert(notice_open);draw();activate(&cancel);assert(!notice_open);state.error[0]=0;
    strcpy(state.whitelist,"on");Hit toggle={.action=A_ALLOW,.value=1};activate(&toggle);assert(confirm==7);activate(&cancel);assert(!confirm);
    strcpy(state.server,"recovery");page=HOME;draw();for(int i=0;i<hit_count;i++)assert(hits[i].action!=A_START&&hits[i].action!=A_RESTART&&hits[i].action!=A_STOP);
    strcpy(state.server,"starting");strcpy(state.error,"Clock invalid: set time using USB or SSH");draw();for(int i=0;i<hit_count;i++)assert(hits[i].action!=A_START&&hits[i].action!=A_RESTART&&hits[i].action!=A_STOP);
    strcpy(state.server,"offline");strcpy(state.error,"Battery low: waiting for charge");draw();for(int i=0;i<hit_count;i++)assert(hits[i].action!=A_START&&hits[i].action!=A_RESTART&&hits[i].action!=A_STOP);state.error[0]=0;strcpy(state.server,"online");
    long fresh_time=state.updated;state.updated=0;Hit add={.action=A_ADD};activate(&add);assert(!keyboard_open);state.updated=fresh_time;
    helper_pid=12345;helper_mutation=0;assert(admin("whitelist-set","on"));assert(busy()&&!strcmp(queued_command,"whitelist-set")&&!strcmp(queued_arg,"on"));helper_pid=0;queued_command[0]=queued_arg[0]=0;
    if(argc>1&&!strcmp(argv[1],"--benchmark")){
        difficulty_open=1;for(int mode=0;mode<2;mode++){glyph_cache_enabled=mode;long long start=now_ms();for(int i=0;i<20;i++)draw();printf("%s: %.2f ms/frame\n",mode?"cached":"uncached",(now_ms()-start)/20.0);}glyph_cache_enabled=1;difficulty_open=0;
        keyboard_open=2;strcpy(wifi_password,"example-password");wifi_cursor=strlen(wifi_password);void *output=calloc(1,map_size);assert(output);
        for(int mode=0;mode<2;mode++){
            clip_top=mode?400:0;clip_bottom=mode?1480:H;long long start=now_ms();
            for(int i=0;i<40;i++){draw();for(int y=clip_top;y<clip_bottom;y++)memcpy((char *)output+(size_t)y*pitch,(char *)canvas+(size_t)y*pitch,pitch);}
            printf("keyboard_%s: %.2f ms/frame; bytes_per_frame=%zu\n",mode?"partial":"full",(now_ms()-start)/40.0,(size_t)(clip_bottom-clip_top)*pitch);
        }
        clip_top=0;clip_bottom=H;keyboard_open=0;explicit_bzero(wifi_password,sizeof(wifi_password));free(output);
    }else if(argc>1){page=argc>2?atoi(argv[2]):TOOLS;draw();FILE *f=fopen(argv[1],"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",W,H);for(int y=0;y<H;y++)for(int x=0;x<W;x++){uint32_t c=((uint32_t *)((char *)canvas+(size_t)y*pitch))[x];unsigned char rgb[3]={(unsigned char)(c>>16),(unsigned char)(c>>8),(unsigned char)c};assert(fwrite(rgb,1,3,f)==3);}assert(fclose(f)==0);}
    puts("dashboard headless checks passed");free(canvas);FT_Done_Face(mono);FT_Done_Face(bold);FT_Done_Face(regular);FT_Done_FreeType(ft);return 0;
}
