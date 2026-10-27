/* A controller front end: game/network simulation starts only after selection. */
#include <stdio.h>
#include <string.h>
#include "xbox_menu.h"
#include "xbox_netplay.h"
#include "../core/osd.h"
#define WHITE 0xffeff5ffu
#define GOLD  0xffffd475u
#define GREY  0xffa2b6ceu
#define WIDTH 256
#define HEIGHT 240
static void background(uint32_t *fb)
{
    int x,y;
    for(y=0;y<HEIGHT;++y)
        for(x=0;x<WIDTH;++x) fb[y*WIDTH+x]=y<60?0xff172b48u:0xff0b1628u;
}
static void centre(uint32_t *fb,int y,const char *text,uint32_t colour,int scale)
{
    int width=((int)strlen(text)*6-1)*scale;
    osd_text(fb,WIDTH,HEIGHT,(WIDTH-width)/2,y,text,colour,scale);
}
static void row(uint32_t *fb,int y,const char *text,int selected)
{
    int x,k;
    if(selected)for(k=y-5;k<y+13;++k)for(x=18;x<238;++x)fb[k*WIDTH+x]=0xff24445fu;
    centre(fb,y,text,selected?GOLD:WHITE,1);
}
void xb_menu_init(XbMenu *m,uint8_t held)
{
    memset(m,0,sizeof(*m));m->previous=held;memcpy(m->passcode,"0000",5);
}
const char *xb_menu_passcode(const XbMenu *m) { return m->protected_room?m->passcode:NULL; }
static void pin_screen(uint32_t *fb,const char *title,const char *code,unsigned digit,const char *message)
{
    char number[2]={0,0};unsigned i;
    background(fb);centre(fb,25,title,WHITE,2);
    centre(fb,64,"FOUR DIGIT ROOM PASSCODE",GREY,1);
    for(i=0;i<4;++i) {
        int x=77+(int)i*27,y,k;
        if(i==digit)for(y=95;y<127;++y)for(k=x-6;k<x+16;++k)fb[y*WIDTH+k]=0xff24445fu;
        number[0]=code[i];osd_text(fb,WIDTH,HEIGHT,x,104,number,i==digit?GOLD:WHITE,2);
    }
    centre(fb,146,message,GOLD,1);
    centre(fb,172,"LEFT RIGHT - SELECT DIGIT",GREY,1);
    centre(fb,188,"UP DOWN - CHANGE DIGIT",GREY,1);
    centre(fb,212,"A CONFIRM   B BACK",WHITE,1);
}
int xb_menu_poll(XbMenu *m,uint8_t pad)
{
    unsigned pressed=pad & ~m->previous;
    int count=m->page==4?4:m->page?3:5;
    if(m->page==5) {
        if(pressed&PAD_LEFT)m->digit=(m->digit+3)%4;
        if(pressed&PAD_RIGHT)m->digit=(m->digit+1)%4;
        if(pressed&PAD_UP)m->passcode[m->digit]=(char)('0'+(m->passcode[m->digit]-'0'+1)%10);
        if(pressed&PAD_DOWN)m->passcode[m->digit]=(char)('0'+(m->passcode[m->digit]-'0'+9)%10);
        if(pressed&(PAD_B|PAD_SELECT)) { m->page=4;m->selected=1; }
        if(pressed&(PAD_A|PAD_START)) { m->protected_room=1;m->page=4;m->selected=2; }
        m->previous=pad;return XB_MENU_NONE;
    }
    m->previous=pad;
    if(pressed&PAD_UP)m->selected=(m->selected+count-1)%count;
    if(pressed&PAD_DOWN)m->selected=(m->selected+1)%count;
    if((pressed&(PAD_B|PAD_SELECT)) && m->page) {
        if(m->page==4) { m->page=1;m->selected=0; }
        else { m->selected=m->page;m->page=0; }
        return XB_MENU_NONE;
    }
    if(m->page==4) {
        if(m->selected==0 && (pressed&(PAD_LEFT|PAD_RIGHT|PAD_A|PAD_START)))m->protected_room^=1;
        if(pressed&(PAD_A|PAD_START)) {
            if(m->selected==1) { m->page=5;m->digit=0; }
            else if(m->selected==2)return XB_MENU_ONLINE_HOST;
            else if(m->selected==3) { m->page=1;m->selected=0; }
        }
        return XB_MENU_NONE;
    }
    if(m->page==3 && m->selected<2) {
        int direction=(pressed&PAD_LEFT)?-1:(pressed&(PAD_RIGHT|PAD_A|PAD_START))?1:0;
        int delay=xb_net_delay_setting();
        unsigned window=xb_net_rollback_setting();
        if(direction) {
            if(m->selected==0) { delay+=direction;if(delay < -1)delay=12;if(delay>12)delay=-1; }
            else window=(unsigned)((int)window+direction+4)%4;
            xb_net_configure(delay,window);
        }
        return XB_MENU_NONE;
    }
    if(!(pressed&(PAD_A|PAD_START)))return XB_MENU_NONE;
    if(!m->page) {
        if(!m->selected)return XB_MENU_LOCAL;
        if(m->selected==4)return XB_MENU_QUIT;
        m->page=m->selected;m->selected=0;return XB_MENU_NONE;
    }
    if(m->page==1 && m->selected==0) { m->page=4;m->selected=0;return XB_MENU_NONE; }
    if(m->selected==2) { m->selected=m->page;m->page=0;return XB_MENU_NONE; }
    return m->page==1?(m->selected?XB_MENU_ONLINE_JOIN:XB_MENU_ONLINE_HOST):
                       (m->selected?XB_MENU_LAN_JOIN:XB_MENU_LAN_HOST);
}
static void settings_line(char *line,size_t size)
{
    int delay=xb_net_delay_setting();
    if(delay<0)snprintf(line,size,"AUTO DELAY - ROLLBACK %uF",xb_net_rollback_setting());
    else snprintf(line,size,"DELAY %dF - ROLLBACK %uF",delay,xb_net_rollback_setting());
}
void xb_menu_draw(const XbMenu *m,uint32_t *fb)
{
    static const char *home[]={"LOCAL PLAY","ONLINE PLAY","LAN PLAY","NETPLAY SETTINGS","EXIT TO DASHBOARD"};
    static const char *online[]={"HOST ROOM","BROWSE ROOMS","BACK"};
    static const char *lan[]={"HOST LAN GAME","JOIN LAN GAME","BACK"};
    const char *const *rows=m->page==1?online:m->page==2?lan:home;
    char line[40];
    int i,count=m->page?3:5;
    if(m->page==5) { pin_screen(fb,"SET PASSCODE",m->passcode,m->digit,"PUBLIC ROOM WITH PASSCODE");return; }
    background(fb);
    if(m->page==4) {
        centre(fb,25,"CREATE ROOM",WHITE,2);
        centre(fb,49,"LISTED IN PUBLIC ROOM BROWSER",GREY,1);
        row(fb,80,m->protected_room?"ACCESS: PASSCODE":"ACCESS: OPEN",m->selected==0);
        snprintf(line,sizeof(line),"SET PASSCODE: %s",m->passcode);row(fb,103,line,m->selected==1);
        row(fb,126,"CREATE PUBLIC ROOM",m->selected==2);row(fb,149,"BACK",m->selected==3);
        centre(fb,184,"LEFT RIGHT - CHANGE ACCESS",GREY,1);
        centre(fb,202,"A SELECT   B BACK",WHITE,1);
        return;
    }
    centre(fb,25,m->page==1?"ONLINE PLAY":m->page==2?"LAN PLAY":m->page==3?"NET SETTINGS":"CONTRA",WHITE,2);
    centre(fb,49,m->page==3?"HOST SETTINGS - NEXT SESSION":m->page?"TWO PLAYER CO-OP":"NES XBOX",GREY,1);
    if(m->page==3) {
        if(xb_net_delay_setting()<0)snprintf(line,sizeof(line),"FRAME DELAY: AUTO");
        else snprintf(line,sizeof(line),"FRAME DELAY: %d",xb_net_delay_setting());
        row(fb,86,line,m->selected==0);
        snprintf(line,sizeof(line),"ROLLBACK LIMIT: %u",xb_net_rollback_setting());
        row(fb,109,line,m->selected==1);row(fb,132,"BACK",m->selected==2);
        centre(fb,157,"0 ROLLBACK WAITS FOR REAL INPUT",GREY,1);
        centre(fb,173,"LOW DELAY MAY INCREASE STALLS",GREY,1);
        centre(fb,195,"LEFT RIGHT CHANGE   B BACK",WHITE,1);
        centre(fb,215,"GUEST USES THE HOST SETTINGS",GREY,1);
        return;
    }
    for(i=0;i<count;++i)row(fb,78+i*21,rows[i],i==m->selected);
    centre(fb,191,m->page?"A SELECT   B BACK":"UP DOWN SELECT   A CONFIRM",GREY,1);
    settings_line(line,sizeof(line));
    centre(fb,211,m->page?line:"BACK START - RETURN TO THIS MENU",GREY,1);
}
void xb_menu_lobby(uint32_t *fb,int online,int host)
{
    const char *status=xb_net_status();
    char line[40];size_t length=strlen(status),first=length;
    unsigned i;
    background(fb);
    if(xb_net_editing_code()) {
        pin_screen(fb,"JOIN ROOM",xb_net_join_code(),xb_net_code_digit(),xb_net_code_error());return;
    }
    if(xb_net_browsing() && xb_net_room_count()) {
        centre(fb,25,"ONLINE ROOMS",WHITE,2);
        for(i=0;i<xb_net_room_count();++i) {
            snprintf(line,sizeof(line),"%08lX  %s  1/2",(unsigned long)xb_net_room_id(i),xb_net_room_locked(i)?"CODE":"OPEN");
            row(fb,65+(int)i*16,line,i==xb_net_room_selected());
        }
        centre(fb,199,"UP DOWN SELECT   A JOIN",GREY,1);
        centre(fb,215,"B BACK",GREY,1);return;
    }
    centre(fb,25,online?(host?"HOST ONLINE":"JOIN ONLINE"):(host?"HOST LAN":"JOIN LAN"),WHITE,2);
    if(first>36) {
        first=36;
        while(first>0 && status[first]!=' ')--first;
        if(!first)first=36;
    }
    memcpy(line,status,first);line[first]=0;centre(fb,89,line,GOLD,1);
    if(first<length) {
        size_t start=first,rest;
        while(status[start]==' ')++start;
        rest=length-start;if(rest>36)rest=36;
        memcpy(line,status+start,rest);line[rest]=0;centre(fb,104,line,GOLD,1);
    }
    settings_line(line,sizeof(line));
    centre(fb,155,host?line:"HOST SETS DELAY AND ROLLBACK",GREY,1);
    centre(fb,195,xb_net_can_start()?"A START GAME   B LEAVE ROOM":"B CANCEL AND GO BACK",WHITE,1);
    centre(fb,215,"HOST SELECTS 2 PLAYERS AFTER CONNECT",GREY,1);
}
void xb_menu_error(uint32_t *fb,int problem)
{
    background(fb);
    centre(fb,25,"CARTRIDGE",WHITE,2);
    centre(fb,58,problem==2?"UNSUPPORTED OR DAMAGED ROM":problem==3?"ROM COULD NOT BE READ":"ROM NOT FOUND",GOLD,1);
    centre(fb,92,"PLACE CONTRA.NES NEXT TO DEFAULT.XBE",WHITE,1);
    centre(fb,110,"OR USE DATA/CONTRA.NES",WHITE,1);
    centre(fb,140,"BASEROM.NES IS ALSO ACCEPTED",GREY,1);
    centre(fb,157,"USE YOUR CONTRA USA CARTRIDGE",GREY,1);
    centre(fb,196,"A RETRY   B DASHBOARD",WHITE,1);
    centre(fb,215,"COPY THE WHOLE XBOX GAME FOLDER",GREY,1);
}
