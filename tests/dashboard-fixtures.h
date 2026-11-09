/* Synthetic, non-private telemetry for headless rendering and input tests.
 * No dashboard screenshot needs the live Wi-Fi, log, battery or process state. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int fixture_running=1,fixture_wifi_available=1;
static FILE *fixture_file(const char *contents){
    FILE *f=tmpfile();if(!f)return NULL;
    size_t length=strlen(contents);
    if(fwrite(contents,1,length,f)!=length){fclose(f);return NULL;}
    rewind(f);return f;
}
static FILE *fixture_fopen(const char *path,const char *mode){
    if(!strcmp(mode,"r")){
        if(!strcmp(path,"/run/minecraft.pid")){char pid[32];snprintf(pid,sizeof(pid),"%ld\n",fixture_running?(long)getpid():0L);return fixture_file(pid);}
        if(!strcmp(path,"/run/n20/wifi-state"))return fixture_wifi_available?fixture_file(
            "STATE\tCOMPLETED\nIP\t192.0.2.40\nCURRENT\t486f6d65\nMESSAGE\tConnected\n"
            "NETWORK\t486f6d65\tWPA2\t-42\n"
            "NETWORK\t4775657374\tOPEN\t-67\n"
            "NETWORK\t53747564696f\tWPA2\t-78\n"
            "NETWORK\t456e7465727072697365\tUNSUPPORTED\t-80\n"):NULL;
        if(!strcmp(path,"/sys/class/power_supply/battery/capacity"))return fixture_file("54\n");
        if(!strcmp(path,"/sys/class/power_supply/battery/temp"))return fixture_file("286\n");
        if(!strcmp(path,"/sys/class/power_supply/battery/status"))return fixture_file("Not charging\n");
        if(!strcmp(path,"/sys/class/power_supply/usb/online"))return fixture_file("1\n");
        if(!strcmp(path,"/sys/class/power_supply/ac/online"))return fixture_file("0\n");
        if(!strcmp(path,"/sys/class/oplus_chg/battery/mmi_charging_enable"))return fixture_file("0\n");
        if(!strcmp(path,"/proc/uptime"))return fixture_file("30600.00 0.00\n");
        if(!strcmp(path,"/var/log/minecraft.log"))return fixture_file(
            "[10:20:00] Starting Minecraft server\n"
            "[10:20:12] Preparing world\n"
            "[10:20:18] Done! The world is ready.\n"
            "[10:24:10] Alex joined the game\n"
            "[10:25:32] Robin joined the game\n"
            "[10:30:00] Saving the game\n"
            "[10:30:01] Saved the game\n");
    }
    return fopen(path,mode);
}
