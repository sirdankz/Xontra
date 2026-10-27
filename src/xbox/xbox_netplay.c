/* Contra v3: Xbox LAN + internet rendezvous/relay, auto delay, <=3F rollback.
 * Directory packets and game packets are separate bounded binary protocols.
 */
#include "xbox_xtl.h"
#include <winsockx.h>
#include <stdio.h>
#include <string.h>
#include "xbox_netplay.h"
#include "netplay_build.h"
#include "../core/netplay.h"
#define NET_PORT 6474
#define DIRECTORY_PORT 6475
#define DIRECTORY_IP "172.233.145.244"
#define PACKET_SIZE 144
#define CONTROL_SIZE 96
#define REDUNDANCY 64
#define TIMEOUT_MS 10000u
#define LOBBY_MS 120000u

enum { OFF, HOSTING, SEARCHING, BROWSING, ENTERING_CODE, JOINING, CALIBRATING, PLAYING, FAILED };
enum { HELLO=1, OFFER, ACCEPT, START, INPUT, LEAVE, REJECT, PING, PONG };
enum { CHALLENGE=1, REGISTER, REGISTERED, LIST, ROOMS, JOIN, MATCH, KEEP, DIR_LEAVE, DIR_ERROR };
enum { ROUTE_AUTO, ROUTE_DIRECT, ROUTE_RELAY };
static SOCKET sSocket = INVALID_SOCKET;
static int sStack, sMode, sHost, sOnline, sPeerKnown, sReset, sPeerStarted, sMatched;
static struct sockaddr_in sPeer, sDirectory;
static uint32_t sSession, sNonce, sRom, sDirectoryNonce, sRoom, sRooms[8];
static uint8_t sCookie[16], sToken[16], sUiPrev;
static unsigned sRoomCount, sSelected, sDelay, sInitialDelay, sDelayRevision;
static DWORD sBegin, sLastSend, sProgress, sLastDirectory, sLastRelay, sCalStart;
static DWORD sLastDirectPong, sPingSent, sLastPing, sLastPeer;
static uint32_t sPingId;
static int sPingOutstanding, sHaveDirectPong;
static uint8_t sRoomLocked[8];
static int sRoomProtected, sStartRequested;
static char sPasscode[5], sJoinCode[5];
static unsigned sCodeDigit;
static uint32_t sJoinAttempt;
static const char *sCodeError = "";
static int sConfiguredDelay = -1, sAuto;
static unsigned sConfiguredWindow = NP_MAX_ROLLBACK, sWindow;
void xb_net_configure(int delay, unsigned window)
{
    if(sMode!=OFF)return;
    sConfiguredDelay=delay < -1?-1:delay>NP_MAX_DELAY?NP_MAX_DELAY:delay;
    sConfiguredWindow=window>NP_MAX_ROLLBACK?NP_MAX_ROLLBACK:window;
}
int xb_net_delay_setting(void) { return sConfiguredDelay; }
unsigned xb_net_rollback_setting(void) { return sConfiguredWindow; }
static NpSession sCore;
static NpDelay sFilter;
static const char *sStatus = "";
static char sLine[40];
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void put32(uint8_t *p,uint32_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static int equal(const struct sockaddr_in *a,const struct sockaddr_in *b) { return a->sin_addr.s_addr==b->sin_addr.s_addr && a->sin_port==b->sin_port; }
static int direct_ok(void) { return !sOnline || (sHaveDirectPong && GetTickCount()-sLastDirectPong<1000u); }
static void send_raw(const uint8_t *p,int size,const struct sockaddr_in *to)
{
    int n=sendto(sSocket,(const char *)p,size,0,(const struct sockaddr *)to,sizeof(*to));
    if(n==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK) xb_log("net: send error %d",WSAGetLastError());
}
static void directory_send(int type)
{
    uint8_t p[CONTROL_SIZE];
    if(!sOnline || sSocket==INVALID_SOCKET) return;
    memset(p,0,sizeof(p));memcpy(p,"CDR3",4);p[4]=(uint8_t)type;p[5]=3;
    put32(p+8,sDirectoryNonce);put32(p+12,sRoom);put32(p+16,sRom);
    put32(p+20,CNX_NET_BUILD);put32(p+24,(uint32_t)sizeof(Nes));memcpy(p+28,sCookie,16);
    if(type==REGISTER && sRoomProtected) { p[84]=1;memcpy(p+85,sPasscode,4); }
    if(type==JOIN) { p[84]=1;memcpy(p+85,sJoinCode,4);put32(p+92,sJoinAttempt); }
    send_raw(p,sizeof(p),&sDirectory);
}
static void packet(uint8_t *p,int type)
{
    memset(p,0,PACKET_SIZE);memcpy(p,"CNX2",4);p[4]=(uint8_t)type;p[5]=(uint8_t)sDelay;p[6]=(uint8_t)sWindow;p[7]=(uint8_t)sAuto;
    put32(p+8,sSession);put32(p+12,sNonce);put32(p+16,sRom);put32(p+20,CNX_NET_BUILD);
    put32(p+24,sMode==PLAYING?sCore.frame:0);put32(p+100,NP_NONE);put32(p+116,(uint32_t)sizeof(Nes));
    memcpy(p+120,sToken,16);put32(p+136,sDelayRevision);
    if(type==START) { p[5]=(uint8_t)sInitialDelay;put32(p+136,0); }
}
static void send_game(const uint8_t *p,int route)
{
    DWORD now=GetTickCount();
    if(!sPeerKnown) return;
    if(route!=ROUTE_RELAY) send_raw(p,PACKET_SIZE,&sPeer);
    if(sOnline && (route==ROUTE_RELAY || (route==ROUTE_AUTO &&
       (!direct_ok() || p[4]!=INPUT || now-sLastRelay>=100u)))) {
        uint8_t envelope[20+PACKET_SIZE];memcpy(envelope,"CRL2",4);memcpy(envelope+4,sCookie,16);
        memcpy(envelope+20,p,PACKET_SIZE);send_raw(envelope,sizeof(envelope),&sDirectory);sLastRelay=now;
    }
}
static void send_control(int type)
{
    uint8_t p[PACKET_SIZE];packet(p,type);send_game(p,ROUTE_AUTO);
}
static void close_network(void)
{
    if(sSocket!=INVALID_SOCKET) { closesocket(sSocket);sSocket=INVALID_SOCKET; }
    if(sStack) { WSACleanup();XNetCleanup();sStack=0; }
}
static void fail(const char *why)
{
    sStatus=why;xb_log("net: %s frame %lu",why,(unsigned long)sCore.frame);
    if(sSocket!=INVALID_SOCKET) { if(sPeerKnown)send_control(LEAVE);directory_send(DIR_LEAVE); }
    sMode=FAILED;close_network();
}
void xb_net_stop(void)
{
    int i;
    if(sSocket!=INVALID_SOCKET) for(i=0;i<3;++i) { if(sPeerKnown)send_control(LEAVE);directory_send(DIR_LEAVE); }
    close_network();sMode=OFF;sPeerKnown=sMatched=sReset=0;sStatus="";
}
void xb_net_begin(int host,int online,uint32_t rom_crc,const char *passcode)
{
    XNetStartupParams xp;WSADATA wsa;struct sockaddr_in local;unsigned i;
    BOOL yes=TRUE;u_long nonblock=1;uint32_t random[3];
    xb_net_stop();sHost=host;sOnline=online;sRom=rom_crc;sSession=sNonce=sRoom=0;
    sPeerStarted=sHaveDirectPong=sPingOutstanding=0;sPingId=0;sDelayRevision=0;
    sAuto=sConfiguredDelay<0;sWindow=sConfiguredWindow;
    sDelay=sInitialDelay=sAuto?1:(unsigned)sConfiguredDelay;
    sRoomProtected=host && online && passcode!=NULL;sStartRequested=0;sCodeDigit=0;sCodeError="";sJoinAttempt=0;
    memset(sPasscode,0,sizeof(sPasscode));memcpy(sJoinCode,"0000",5);
    if(sRoomProtected) {
        if(strlen(passcode)!=4) { fail("PASSCODE MUST HAVE FOUR DIGITS");return; }
        for(i=0;i<4;++i)if(passcode[i]<'0' || passcode[i]>'9') { fail("PASSCODE MUST HAVE FOUR DIGITS");return; }
        memcpy(sPasscode,passcode,4);
    }
    memset(sRoomLocked,0,sizeof(sRoomLocked));
    sRoomCount=sSelected=0;sUiPrev=0;memset(sCookie,0,16);memset(sToken,0,16);
    memset(&sCore,0,sizeof(sCore));np_delay_init(&sFilter,sWindow);
    memset(&xp,0,sizeof(xp));xp.cfgSizeOfStruct=sizeof(xp);xp.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
    if(XNetStartup(&xp)) { fail("NETWORK INIT FAILED");return; }
    if(WSAStartup(MAKEWORD(2,2),&wsa)) { XNetCleanup();fail("WINSOCK FAILED");return; }
    sStack=1;
    if(XNetRandom((BYTE *)random,sizeof(random))) { fail("NETWORK RANDOM FAILED");return; }
    sDirectoryNonce=random[0]?random[0]:1;sNonce=host?0:(random[1]?random[1]:1);
    sSession=host?(random[2]?random[2]:1):0;
    sSocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(sSocket==INVALID_SOCKET) { fail("NETWORK SOCKET FAILED");return; }
    setsockopt(sSocket,SOL_SOCKET,0x5801,(const char *)&yes,sizeof(yes));
    if(setsockopt(sSocket,SOL_SOCKET,SO_BROADCAST,(const char *)&yes,sizeof(yes))) { fail("BROADCAST FAILED");return; }
    memset(&local,0,sizeof(local));local.sin_family=AF_INET;local.sin_port=htons(NET_PORT);local.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(sSocket,(struct sockaddr *)&local,sizeof(local))==SOCKET_ERROR || ioctlsocket(sSocket,FIONBIO,&nonblock)==SOCKET_ERROR) { fail("NETWORK BIND FAILED");return; }
    memset(&sDirectory,0,sizeof(sDirectory));sDirectory.sin_family=AF_INET;
    sDirectory.sin_port=htons(DIRECTORY_PORT);sDirectory.sin_addr.s_addr=inet_addr(DIRECTORY_IP);
    sMode=host?HOSTING:(online?BROWSING:SEARCHING);
    sStatus=host?"HOST WAITING":"SEARCHING";
    sBegin=sProgress=GetTickCount();sLastSend=sBegin-250;sLastDirectory=sBegin-1000;sLastPing=sBegin-100;sLastRelay=sBegin-100;
}
static void start_game(unsigned delay)
{
    sDelay=sInitialDelay=delay;sDelayRevision=0;
    np_init(&sCore,sHost?0:1,delay,sWindow);
    sMode=PLAYING;sReset=1;sProgress=GetTickCount();
}
static void directory_packet(const uint8_t *p)
{
    int type=p[4];
    if(p[5]!=3 || get32(p+8)!=sDirectoryNonce || get32(p+16)!=sRom ||
       get32(p+20)!=CNX_NET_BUILD || get32(p+24)!=sizeof(Nes)) return;
    if(type==CHALLENGE) {
        /* Stateless challenges can arrive twice; never replace a live cookie. */
        if(!sRoom && !sMatched) { memcpy(sCookie,p+28,16);sLastDirectory=GetTickCount()-1000; }
        return;
    }
    if(memcmp(p+28,sCookie,16)) return;
    if(type==REGISTERED && sHost && sMode==HOSTING) sRoom=get32(p+12);
    if(type==ROOMS && sMode==BROWSING) {
        unsigned i,count=p[6];uint32_t selected=sSelected<sRoomCount?sRooms[sSelected]:0;
        if(count>8) return;
        sRoomCount=count;sSelected=0;
        for(i=0;i<count;++i) { sRooms[i]=get32(p+52+4*i);sRoomLocked[i]=p[84+i]!=0;if(sRooms[i]==selected)sSelected=i; }
    }
    if(type==DIR_ERROR) {
        if(sMode==ENTERING_CODE && (p[6]==2 || p[6]==3))return;
        if(sMode==JOINING && get32(p+92)!=sJoinAttempt)return;
        if(sMode==JOINING && (p[6]==2 || p[6]==3)) {
            sMode=ENTERING_CODE;sCodeError=p[6]==3?"TOO MANY TRIES - WAIT 10 SEC":"INCORRECT PASSCODE";
        } else if(sMode==JOINING) {
            sMode=BROWSING;sRoom=0;sRoomCount=0;sLastDirectory=GetTickCount()-1000;
        }
        else if(sMode!=BROWSING) fail("ROOM CLOSED OR SERVER FULL");
    }
    if(type==MATCH) {
        if(!sHost && get32(p+92)!=sJoinAttempt)return;
        struct sockaddr_in peer;uint32_t room=get32(p+12);
        if(p[7]!=(sHost?0:1) || !room || (sRoom && room!=sRoom)) return;
        memset(&peer,0,sizeof(peer));peer.sin_family=AF_INET;memcpy(&peer.sin_addr.s_addr,p+44,4);
        peer.sin_port=htons((u_short)(p[48]|(p[49]<<8)));
        if(!peer.sin_port || !peer.sin_addr.s_addr) return;
        if(sMatched) {
            if(!equal(&peer,&sPeer) || memcmp(p+64,sToken,16)) fail("SERVER SESSION CHANGED");
            return;
        }
        if(sMode!=HOSTING && sMode!=JOINING) return;
        sRoom=room;sPeer=peer;sPeerKnown=sMatched=1;sBegin=GetTickCount();memcpy(sToken,p+64,16);
        if(!sHost) sMode=SEARCHING;
        sLastSend=GetTickCount()-250;
    }
}
static void send_inputs(void)
{
    uint8_t p[PACKET_SIZE];uint32_t first,i,count;
    if(sCore.latest==NP_NONE) return;
    first=sCore.latest>=REDUNDANCY-1?sCore.latest-(REDUNDANCY-1):0;count=sCore.latest-first+1;
    packet(p,INPUT);put32(p+28,first);put32(p+32,count);
    for(i=0;i<count;++i) p[36+i]=sCore.inputs[sCore.local_slot][(first+i)&(NP_HISTORY-1)].pad;
    put32(p+100,sCore.hash_frame);put32(p+104,sCore.hash);
    send_game(p,ROUTE_AUTO);
}
static void game_packet(const uint8_t *p,const struct sockaddr_in *from,int relayed)
{
    uint32_t session=get32(p+8),nonce=get32(p+12);int type=p[4];
    int compatible=get32(p+16)==sRom && get32(p+20)==CNX_NET_BUILD && get32(p+116)==sizeof(Nes);
    if(p[6]>NP_MAX_ROLLBACK || p[5]>NP_MAX_DELAY || p[7]>1) return;
    if(sOnline && (!sMatched || memcmp(p+120,sToken,16) || (!relayed && !equal(from,&sPeer)))) return;
    if(!sOnline && sPeerKnown && !equal(from,&sPeer)) return;
    if(sMode==HOSTING && type==HELLO && !session && nonce) {
        uint8_t reply[PACKET_SIZE];packet(reply,compatible?OFFER:REJECT);put32(reply+12,nonce);
        if(sOnline) send_game(reply,ROUTE_AUTO);else send_raw(reply,PACKET_SIZE,from);
        return;
    }
    if(sMode==SEARCHING && type==REJECT && nonce==sNonce) { fail("DIFFERENT ROM OR BUILD");return; }
    if(!compatible) return;
    if(sMode==SEARCHING && type==OFFER && nonce==sNonce && session) {
        if(!sOnline) { sPeer=*from;sPeerKnown=1; }
        /* Host settings override local guest preferences for this session. */
        sWindow=p[6];sAuto=p[7];sDelay=p[5];
        sSession=session;sMode=CALIBRATING;sCalStart=sLastPeer=GetTickCount();send_control(ACCEPT);return;
    }
    if(sMode==HOSTING && type==ACCEPT && session==sSession && nonce && p[6]==sWindow && p[7]==sAuto) {
        if(!sOnline) { sPeer=*from;sPeerKnown=1; }
        sNonce=nonce;sMode=CALIBRATING;sCalStart=sLastPeer=GetTickCount();return;
    }
    if(!sPeerKnown || session!=sSession || nonce!=sNonce || (!relayed && !equal(from,&sPeer))) return;
    if(p[6]!=sWindow || p[7]!=sAuto)return;
    sLastPeer=GetTickCount();
    if(type==LEAVE) { fail("PEER LEFT");return; }
    if(type==PING) {
        uint8_t reply[PACKET_SIZE];packet(reply,PONG);memcpy(reply+108,p+108,8);
        send_game(reply,relayed?ROUTE_RELAY:ROUTE_DIRECT);return;
    }
    if(type==PONG && get32(p+108)==sPingId && get32(p+112)==sPingSent) {
        DWORD now=GetTickCount();
        if(!relayed) { sHaveDirectPong=1;sLastDirectPong=now; }
        if(sPingOutstanding) {
            sPingOutstanding=0;
            if(sHost) {
                unsigned candidate=np_delay_sample(&sFilter,now-sPingSent);
                if(sAuto && sMode==CALIBRATING) sDelay=candidate;
                else if(sAuto && sPeerStarted && candidate!=sDelay) {
                    sDelay=candidate;++sDelayRevision;np_set_delay(&sCore,candidate);
                }
            }
        }
        return;
    }
    if(sMode==CALIBRATING && !sHost && type==START) { start_game(p[5]);return; }
    if(sMode==PLAYING && sHost && type==ACCEPT) { send_control(START);return; }
    if(sMode==PLAYING && type==INPUT) {
        uint32_t first=get32(p+28),count=get32(p+32),i;
        if(!count || count>REDUNDANCY || first>0xffffffffu-count) return;
        if(first+count-1>sCore.frame+2*NP_MAX_DELAY+2*NP_MAX_ROLLBACK) return;
        for(i=0;i<count;++i) np_receive(&sCore,1-sCore.local_slot,first+i,p[36+i]);
        np_receive_hash(&sCore,get32(p+100),get32(p+104));
        if(!sHost && sAuto && get32(p+136)>sDelayRevision) {
            sDelayRevision=get32(p+136);sDelay=p[5];np_set_delay(&sCore,sDelay);
        }
        sPeerStarted=1;
    }
}
static void receive_packets(void)
{
    int tries;
    for(tries=0;tries<64 && sMode!=FAILED;++tries) {
        uint8_t p[20+PACKET_SIZE+1];struct sockaddr_in from;int len=sizeof(from);
        int n=recvfrom(sSocket,(char *)p,sizeof(p),0,(struct sockaddr *)&from,&len);
        if(n==SOCKET_ERROR) {
            int e=WSAGetLastError();if(e!=WSAEWOULDBLOCK && e!=WSAEMSGSIZE)fail("NETWORK RECEIVE FAILED");return;
        }
        if(sOnline && equal(&from,&sDirectory)) {
            if(n==CONTROL_SIZE && !memcmp(p,"CDR3",4)) directory_packet(p);
            else if(n==20+PACKET_SIZE && !memcmp(p,"CRL2",4) && !memcmp(p+4,sCookie,16) && !memcmp(p+20,"CNX2",4))
                game_packet(p+20,&sPeer,1);
        } else if(n==PACKET_SIZE && !memcmp(p,"CNX2",4)) game_packet(p,&from,0);
    }
}
void xb_net_poll(void)
{
    DWORD now;
    if(sMode==OFF || sMode==FAILED) return;
    receive_packets();if(sMode==FAILED)return;now=GetTickCount();
    if(sMode==CALIBRATING && now-sLastPeer>TIMEOUT_MS) { fail("PEER DISCONNECTED");return; }
    if(sOnline && now-sLastDirectory>=1000u) {
        int type=sMatched?KEEP:sHost?REGISTER:sMode==JOINING?JOIN:LIST;
        directory_send(type);sLastDirectory=now;
    }
    if(sPeerKnown && (sMode==CALIBRATING || sMode==PLAYING) && now-sLastPing>=100u &&
       (!sPingOutstanding || now-sPingSent>=1000u)) {
        uint8_t p[PACKET_SIZE];++sPingId;sPingSent=now;sPingOutstanding=1;sLastPing=now;
        packet(p,PING);put32(p+108,sPingId);put32(p+112,sPingSent);send_game(p,ROUTE_AUTO);
    }
    if(sMode==CALIBRATING && sHost && sStartRequested && sFilter.count>=9 && now-sCalStart>=(sOnline?2500u:1000u)) {
        start_game(sDelay);send_control(START);
    }
    if(sMode==PLAYING) {
        if(now-sProgress>TIMEOUT_MS) { fail("NET TIMEOUT");return; }
        if(now-sLastSend>=16u) {
            if(sHost && !sPeerStarted)send_control(START);
            send_inputs();sLastSend=now;
        }
    } else {
        if(sMode!=HOSTING && sMode!=BROWSING && sMode!=ENTERING_CODE && sMode!=CALIBRATING && now-sBegin>LOBBY_MS) { fail("NO CONNECTION");return; }
        if(now-sLastSend>=250u) {
            if(sMode==SEARCHING) {
                uint8_t p[PACKET_SIZE];packet(p,HELLO);
                if(sOnline)send_game(p,ROUTE_AUTO);
                else {
                    struct sockaddr_in broadcast;memset(&broadcast,0,sizeof(broadcast));broadcast.sin_family=AF_INET;
                    broadcast.sin_port=htons(NET_PORT);broadcast.sin_addr.s_addr=htonl(INADDR_BROADCAST);
                    send_raw(p,PACKET_SIZE,&broadcast);
                }
            } else if(sMode==CALIBRATING && !sHost)send_control(ACCEPT);
            sLastSend=now;
        }
    }
}
int xb_net_can_start(void)
{
    return sMode==CALIBRATING && sHost && sFilter.count>=9 &&
           GetTickCount()-sCalStart>=(sOnline?2500u:1000u);
}
void xb_net_ui(uint8_t pad)
{
    unsigned pressed=pad & ~sUiPrev;sUiPrev=pad;
    if(xb_net_can_start() && (pressed&(PAD_A|PAD_START))) { sStartRequested=1;return; }
    if(sMode==ENTERING_CODE) {
        if(pressed&PAD_LEFT)sCodeDigit=(sCodeDigit+3)%4;
        if(pressed&PAD_RIGHT)sCodeDigit=(sCodeDigit+1)%4;
        if(pressed&PAD_UP)sJoinCode[sCodeDigit]=(char)('0'+(sJoinCode[sCodeDigit]-'0'+1)%10);
        if(pressed&PAD_DOWN)sJoinCode[sCodeDigit]=(char)('0'+(sJoinCode[sCodeDigit]-'0'+9)%10);
        if(pressed&(PAD_A|PAD_START)) {
            sMode=JOINING;++sJoinAttempt;sBegin=GetTickCount();sLastDirectory=sBegin-1000;sCodeError="";
        }
        return;
    }
    if(sMode!=BROWSING || !sRoomCount) return;
    if(pressed&PAD_DOWN)sSelected=(sSelected+1)%sRoomCount;
    if(pressed&PAD_UP)sSelected=(sSelected+sRoomCount-1)%sRoomCount;
    if(pressed&(PAD_A|PAD_START)) {
        sRoom=sRooms[sSelected];memcpy(sJoinCode,"0000",5);sCodeDigit=0;sCodeError="";
        sMode=sRoomLocked[sSelected]?ENTERING_CODE:JOINING;
        if(sMode==JOINING)++sJoinAttempt;
        sBegin=GetTickCount();sLastDirectory=sBegin-1000;
    }
}
int xb_net_back(void)
{
    if(sMode!=ENTERING_CODE)return 0;
    sMode=BROWSING;sRoom=0;sCodeError="";sLastDirectory=GetTickCount()-1000;return 1;
}
int xb_net_editing_code(void) { return sMode==ENTERING_CODE; }
const char *xb_net_join_code(void) { return sJoinCode; }
unsigned xb_net_code_digit(void) { return sCodeDigit; }
const char *xb_net_code_error(void) { return sCodeError; }
int xb_net_room_locked(unsigned index) { return index<sRoomCount && sRoomLocked[index]; }
int xb_net_browsing(void) { return sMode==BROWSING; }
unsigned xb_net_room_count(void) { return sRoomCount; }
unsigned xb_net_room_selected(void) { return sSelected; }
uint32_t xb_net_room_id(unsigned index) { return index<sRoomCount?sRooms[index]:0; }
int xb_net_busy(void) { return sMode!=OFF; }
int xb_net_running(void) { return sMode==PLAYING; }
int xb_net_take_reset(void) { int r=sReset;sReset=0;return r; }
const char *xb_net_status(void)
{
    if(sMode==PLAYING)return "";
    if(sMode==BROWSING) {
        if(!sRoomCount)return "NO ROOMS - REFRESHING";
        snprintf(sLine,sizeof(sLine),"ROOM %08lX %u/%u A JOIN",(unsigned long)sRooms[sSelected],sSelected+1,sRoomCount);return sLine;
    }
    if(sMode==HOSTING && sOnline && sRoom) { snprintf(sLine,sizeof(sLine),"HOST ROOM %08lX - WAITING",(unsigned long)sRoom);return sLine; }
    if(xb_net_can_start())return "BOTH PLAYERS READY - A START GAME";
    if(sMode==CALIBRATING && !sHost)return "CONNECTED - WAITING FOR HOST";
    if(sMode==CALIBRATING)return sAuto?"MEASURING CONNECTION - AUTO DELAY":"MEASURING CONNECTION - FIXED DELAY";
    if(sMode==JOINING)return "JOINING INTERNET ROOM";
    return sStatus;
}
void xb_net_reconcile(Nes *nes)
{
    uint32_t emitted;
    if(sMode!=PLAYING)return;
    emitted=sCore.emitted;np_reconcile(&sCore,nes,xb_audio_push);
    if(sCore.emitted!=emitted)sProgress=GetTickCount();
    if(sCore.fault)fail(sCore.fault==2?"NET DESYNC":"NET TIMELINE FAULT");
}
int xb_net_advance(Nes *nes,uint8_t local)
{
    int advanced;
    if(sMode!=PLAYING)return 0;
    if(np_sample(&sCore,local)) { send_inputs();sLastSend=GetTickCount(); }
    xb_net_reconcile(nes);if(sMode!=PLAYING)return 0;
    advanced=np_step(&sCore,nes);xb_net_reconcile(nes);return advanced;
}
