/* Native rollback regression tests run by setup.py against the NES core. */
#include "../../src/core/netplay.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"netplay test failed: line %d: %s\n",__LINE__,#c);return 1; } } while(0)
#define TEST_FRAMES 600
static NpSession net;
static Nes machine, baseline;
static uint32_t pixels[NES_W*NES_H], expected_pixels[NES_W*NES_H];
static uint8_t delivered[TEST_FRAMES], local_truth[TEST_FRAMES+32];
static uint32_t audio_hash, audio_frames, reference_audio;
static uint32_t audio_digest(uint32_t hash,const int16_t *data,int n)
{
    int i;
    for(i=0;i<n;++i) {
        uint16_t v=(uint16_t)data[i];hash=(hash^(v&255))*16777619u;hash=(hash^(v>>8))*16777619u;
    }
    return hash;
}
static void collect_audio(const int16_t *data,int n)
{
    audio_hash=audio_digest(audio_hash,data,n);++audio_frames;
}
static uint8_t scripted(unsigned frame,unsigned player)
{
    if(frame<400) return (uint8_t)((frame*37+player*19)&3); /* force early prediction corrections */
    if(frame==420 || frame==421) return player?0:PAD_SELECT;
    if(frame==430 || frame==431) return player?0:PAD_START;
    return (uint8_t)((frame%7<3?PAD_A:0)|(frame%4<2?PAD_B:0)|(player?PAD_LEFT:PAD_RIGHT));
}
static unsigned arrival(unsigned frame,unsigned scenario)
{
    if(scenario==0)return frame;
    if(scenario==1)return frame+3;
    /* Loss until retransmission, out-of-order data and long gaps. */
    return frame+(frame%37==0?20:frame%5);
}
static int run_session(const uint8_t *rom,size_t length,unsigned window,unsigned scenario,int adapt)
{
    unsigned tick,reference_frame=0,initial_delay=adapt?2:0;
    static int16_t audio[NP_AUDIO_MAX];
    memset(&machine,0,sizeof(machine));memset(&baseline,0,sizeof(baseline));
    memset(pixels,0,sizeof(pixels));memset(expected_pixels,0,sizeof(expected_pixels));
    CHECK(!nes_init(&machine,rom,length,pixels,48000));
    CHECK(!nes_init(&baseline,rom,length,expected_pixels,48000));
    np_init(&net,0,initial_delay,window);
    memset(delivered,0,sizeof(delivered));memset(local_truth,0,sizeof(local_truth));
    audio_hash=reference_audio=2166136261u;audio_frames=0;
    for(tick=0;tick<20000 && net.emitted<TEST_FRAMES;++tick) {
        unsigned f;int k;
        if(adapt)np_set_delay(&net,net.frame<120?2:net.frame<240?8:net.frame<360?2:net.frame<480?12:1);
        if(net.frame<TEST_FRAMES)np_sample(&net,scripted(net.frame,0));
        for(f=net.frame;f<=net.frame+NP_MAX_DELAY && f<sizeof(local_truth);++f) {
            NpInput *v=&net.inputs[0][f&(NP_HISTORY-1)];
            if(v->valid && v->frame==f)local_truth[f]=v->pad;
        }
        /* Descending delivery intentionally reverses each available batch. */
        for(k=TEST_FRAMES-1;k>=0;--k) {
            uint8_t value=k<(int)initial_delay?0:scripted((unsigned)k-initial_delay,1);
            if(!delivered[k] && arrival((unsigned)k,scenario)<=tick && np_receive(&net,1,(uint32_t)k,value)) {
                delivered[k]=1;CHECK(np_receive(&net,1,(uint32_t)k,value)); /* duplicate */
            }
        }
        np_reconcile(&net,&machine,collect_audio);
        if(net.frame<TEST_FRAMES)np_step(&net,&machine);
        np_reconcile(&net,&machine,collect_audio);
        CHECK(!net.fault);CHECK(net.frame<=net.confirmed+window);
        CHECK(net.max_depth<=window);CHECK(audio_frames==net.emitted);
        while(reference_frame<net.emitted) {
            int count;
            baseline.pads[0]=local_truth[reference_frame];
            baseline.pads[1]=reference_frame<initial_delay?0:scripted(reference_frame-initial_delay,1);
            count=nes_frame(&baseline,audio,NP_AUDIO_MAX);
            reference_audio=audio_digest(reference_audio,audio,count);++reference_frame;
            if(reference_frame%60==0)np_receive_hash(&net,reference_frame,np_state_hash(&baseline));
        }
        CHECK(audio_hash==reference_audio);CHECK(!net.fault);
        if(net.frame==net.emitted)CHECK(np_state_hash(&machine)==np_state_hash(&baseline));
    }
    CHECK(net.emitted==TEST_FRAMES);CHECK(audio_frames==TEST_FRAMES);
    CHECK(np_state_hash(&machine)==np_state_hash(&baseline));
    CHECK(!memcmp(pixels,expected_pixels,sizeof(pixels)));
    if(window==3 && scenario==1)CHECK(net.max_depth==3);
    printf("netplay test: window %u, scenario %u, adaptive %d, depth %lu, replays %lu: OK\n",
           window,scenario,adapt,(unsigned long)net.max_depth,(unsigned long)net.replays);
    return 0;
}
static int timeline_tests(const uint8_t *rom,size_t length)
{
    unsigned i,window,delay;NpDelay filter;
    np_delay_init(&filter,3);for(i=0;i<9;++i)np_delay_sample(&filter,200);CHECK(filter.delay==4);
    for(i=0;i<40;++i)np_delay_sample(&filter,400);CHECK(filter.delay==10);
    for(i=0;i<60;++i)np_delay_sample(&filter,20);CHECK(filter.delay==0);
    for(i=0;i<40;++i)np_delay_sample(&filter,2000);CHECK(filter.delay==12);
    CHECK(np_delay_sample(&filter,9000)==12);
    /* Auto budgets for each selected rollback cap, including zero. */
    for(window=0;window<=3;++window) {
        np_delay_init(&filter,window);
        for(i=0;i<9;++i)np_delay_sample(&filter,200);
        CHECK(filter.delay==7-window);
    }
    /* Every manual delay/cap combination seeds both players identically,
     * then assigns fresh local input exactly once at its delayed frame. */
    for(window=0;window<=NP_MAX_ROLLBACK;++window) {
        for(delay=0;delay<=NP_MAX_DELAY;++delay) {
            unsigned slot=(delay+window)&1;
            np_init(&net,slot,delay,window);
            CHECK(net.window==window && net.delay==delay);
            CHECK(net.latest==(delay?delay-1:NP_NONE));
            for(i=0;i<delay;++i) {
                CHECK(net.inputs[0][i].valid && net.inputs[1][i].valid);
                CHECK(!net.inputs[0][i].pad && !net.inputs[1][i].pad);
            }
            CHECK(np_sample(&net,PAD_A));
            CHECK(net.latest==delay && net.inputs[slot][delay].pad==PAD_A);
            CHECK(!np_sample(&net,PAD_B));
            CHECK(net.inputs[slot][delay].pad==PAD_A && !net.fault);
        }
    }
    /* Zero is a valid initialized setting, not a sentinel. */
    np_delay_init(&filter,3);
    for(i=0;i<9;++i)np_delay_sample(&filter,20);
    CHECK(filter.delay==0 && filter.initialized);
    np_delay_sample(&filter,200);CHECK(filter.delay==0);
    for(i=0;i<40;++i)np_delay_sample(&filter,200);
    CHECK(filter.delay==4);
    memset(&machine,0,sizeof(machine));CHECK(!nes_init(&machine,rom,length,pixels,48000));
    np_init(&net,0,0,3);
    for(i=0;i<8;++i)CHECK(np_receive(&net,0,i,PAD_B));
    CHECK(np_step(&net,&machine));CHECK(np_step(&net,&machine));CHECK(np_step(&net,&machine));
    CHECK(!np_step(&net,&machine));CHECK(net.frame==3 && net.emitted==0);
    CHECK(np_receive(&net,1,2,PAD_A)); /* later arrival must not reopen prediction */
    CHECK(!np_step(&net,&machine));
    CHECK(np_receive(&net,1,0,PAD_A));CHECK(np_receive(&net,1,1,PAD_A));
    np_reconcile(&net,&machine,NULL);CHECK(!net.fault && net.max_depth==3 && net.emitted==3);
    CHECK(!np_receive(&net,1,1,PAD_B));CHECK(net.fault==1);
    /* No speculative checksum is published at frame 60. */
    memset(&machine,0,sizeof(machine));memset(&baseline,0,sizeof(baseline));
    CHECK(!nes_init(&machine,rom,length,pixels,48000));CHECK(!nes_init(&baseline,rom,length,expected_pixels,48000));
    np_init(&net,0,0,3);
    for(i=0;i<60;++i) {
        int16_t audio[NP_AUDIO_MAX];
        CHECK(np_receive(&net,0,i,0));if(i<57)CHECK(np_receive(&net,1,i,0));
        CHECK(np_step(&net,&machine));np_reconcile(&net,&machine,NULL);
        baseline.pads[0]=0;baseline.pads[1]=i>=57?PAD_A:0;nes_frame(&baseline,audio,NP_AUDIO_MAX);
    }
    CHECK(net.hash_frame==NP_NONE && net.emitted==57);
    np_receive_hash(&net,60,np_state_hash(&baseline));CHECK(!net.fault);
    for(i=57;i<60;++i)CHECK(np_receive(&net,1,i,PAD_A));
    np_reconcile(&net,&machine,NULL);
    CHECK(!net.fault && net.hash_frame==60 && net.hash==np_state_hash(&baseline));
    return 0;
}
int netplay_selftest(const uint8_t *rom,size_t length)
{
    unsigned window;
    CHECK(!timeline_tests(rom,length));
    for(window=0;window<=3;++window)CHECK(!run_session(rom,length,window,1,0));
    CHECK(!run_session(rom,length,3,0,0));
    CHECK(!run_session(rom,length,3,2,1));
    puts("netplay: all deterministic rollback, confirmed audio/hash and delay checks passed");return 0;
}
