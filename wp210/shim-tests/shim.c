/* WP210 Pro (MT6895) first-boot test shim.
 * 1) disable the MediaTek watchdog (TOPRGU @ 0x1C007000)
 * 2) find the boot-logo framebuffer from /chosen "atag,videolfb-*" (added by LK)
 * 3) paint it with colour bands and stay alive forever
 * If no framebuffer is found, it forces an immediate watchdog reset instead,
 * so the two outcomes are easy to tell apart. */
#include <stdint.h>
#include "font.h"
typedef uint32_t u32; typedef uint64_t u64; typedef uint8_t u8;

#define WDT_BASE   0x1C007000UL
#define WDT_MODE   (*(volatile u32 *)(WDT_BASE + 0x00))
#define WDT_SWRST  (*(volatile u32 *)(WDT_BASE + 0x14))
#define WDT_KEY    0x22000000u
#define SWRST_KEY  0x1209u

static u32 be32(const void *p){const u8*b=p;return (u32)b[0]<<24|(u32)b[1]<<16|(u32)b[2]<<8|b[3];}
static int streq(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static int startswith(const char*s,const char*p){while(*p){if(*s++!=*p++)return 0;}return 1;}

static u64 fb_lo, fb_hi, vram, fb_found, chosen_seen;

static void scan_fdt(const u8 *fdt){
    if(!fdt || be32(fdt)!=0xd00dfeed) return;
    const u8 *st = fdt + be32(fdt+8);
    const char *strs = (const char*)fdt + be32(fdt+12);
    int depth=0, in_chosen=0;
    for(;;){
        u32 tok=be32(st); st+=4;
        if(tok==1){ const char*name=(const char*)st; int l=0; while(name[l])l++;
            depth++; if(depth==2 && startswith(name,"chosen")){in_chosen=1;chosen_seen=1;}
            st+=(l+4)&~3; }
        else if(tok==2){ if(depth==2) in_chosen=0; depth--; }
        else if(tok==3){ u32 len=be32(st), no=be32(st+4); const u8*v=st+8;
            if(in_chosen){ const char*n=strs+no;
                if(streq(n,"atag,videolfb-fb_base_l")){fb_lo=be32(v);fb_found|=1;}
                else if(streq(n,"atag,videolfb-fb_base_h")){fb_hi=be32(v);}
                else if(streq(n,"atag,videolfb-vramSize")){vram=be32(v);fb_found|=2;} }
            st+=8+((len+3)&~3); }
        else if(tok==4) continue;
        else break;
    }
}

static u64 cntfrq(void){u64 v;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(v));return v?v:13000000;}
static u64 cntpct(void){u64 v;__asm__ volatile("isb; mrs %0, cntpct_el0":"=r"(v));return v;}
/* wait N seconds, then reboot: the delay tells us which stage we reached */
static void reboot_after(u64 secs){
    u64 end=cntpct()+secs*cntfrq();
    while(cntpct()<end);
    WDT_MODE = WDT_KEY | 0x1;
    WDT_SWRST = SWRST_KEY;
    for(;;);
}


/* ---- command-mode panel: push frames, escalating every 10 s ----
 * Time after the orange screen when the bands appear tells us which step worked. */
#define DSI     0x14017000UL
#define MUTEX   0x14001000UL
#define R(a)    (*(volatile u32 *)(a))
static void delay_us(u64 us){u64 e=cntpct()+us*cntfrq()/1000000;while(cntpct()<e);}
static void kick_panel(void){
    u64 f=cntfrq(), t0=cntpct();
    for(;;){
        u64 s=(cntpct()-t0)/f;
#ifdef UNGATE_KICK
        if(1){ R(MUTEX+0x20)=0; R(MUTEX+0x20)=1; }               /* mutex0, left half pipe */
#ifdef DUALPIPE
        R(0x14401000UL+0x20)=0; R(0x14401000UL+0x20)=1;          /* mutex of right half pipe */
#endif
#else
        if(s>=15){ R(MUTEX+0x20)=0; R(MUTEX+0x20)=1; }          /* B: mutex0 enable  */
#endif
#ifndef UNGATE_KICK
        if(s>=25 && s<35){ R(DSI+0x60)=1; R(DSI+0x200)=0x002C3909; } /* C: CMDQ @0x200 */
        if(s>=35){ R(DSI+0x60)=1; R(DSI+0xD00)=0x002C3909; }      /* D: CMDQ @0xD00 */
#endif
#ifdef UNGATE_KICK
        (void)s; R(DSI+0x00)=0; R(DSI+0x00)=1;
#else
        if(s>=5){ R(DSI+0x00)=0; R(DSI+0x00)=1; }
#endif                 /* A: DSI_START     */
        delay_us(16667);                                           /* ~60 Hz           */
    }
}


/* ---- tiny framebuffer console (stride 1080, ARGB8888, 3 buffers) ---- */
#define FB_W 1080
#define FB_H 2412
#define FB_S 1080
#define FB_BUF (1088ull*2432)
#define SCALE 4
static volatile u32 *con_fb; static u64 con_vram; static int cx, cy;
static void putpx(u64 x,u64 y,u32 c){
    for(u64 b=0;b<3 && (b+1)*FB_BUF*4<=con_vram;b++) con_fb[b*FB_BUF+y*FB_S+x]=c;
}
static void con_clear(u32 c){ for(u64 y=0;y<FB_H;y++) for(u64 x=0;x<FB_W;x++) putpx(x,y,c); }
static void con_putc(char ch){
    if(ch=='\n'){cx=0;cy++;return;}
    if(ch<32||ch>126) ch='?';
    const unsigned char *g=font8x16[ch-32];
    u64 ox=24+cx*8*SCALE, oy=60+cy*16*SCALE;
    if(ox+8*SCALE>FB_W){cx=0;cy++;ox=24;oy=60+cy*16*SCALE;}
    if(oy+16*SCALE>FB_H) return;
    for(int y=0;y<16;y++) for(int x=0;x<8;x++){
        u32 c=(g[y]&(0x80>>x))?0xFFFFFFFFu:0xFF000000u;
        for(int sy=0;sy<SCALE;sy++) for(int sx=0;sx<SCALE;sx++) putpx(ox+x*SCALE+sx,oy+y*SCALE+sy,c);
    }
    cx++;
}
static void con_puts(const char*s){while(*s)con_putc(*s++);}
static void con_hex(u64 v){con_puts("0x");int st=0;for(int i=60;i>=0;i-=4){int d=(v>>i)&15;if(d||st||i==0){st=1;con_putc("0123456789ABCDEF"[d]);}}}
static void con_dec(u64 v){char b[24];int i=0;do{b[i++]='0'+v%10;v/=10;}while(v);while(i)con_putc(b[--i]);}

void shim_main(const u8 *fdt){
    WDT_MODE = WDT_KEY;                 /* watchdog off */
#ifdef SELFTEST
    reboot_after(10);                   /* self-test: timer + reset must work */
#endif
    if(!fdt || be32(fdt)!=0xd00dfeed)
        reboot_after(5);                /* code 1: no device tree in x0 */
    scan_fdt(fdt);
    if(fb_found!=3 || vram==0)
        reboot_after(30);               /* code 2: device tree OK, no framebuffer info */
#ifdef OVLCHECK
    /* Does any overlay layer read from the framebuffer the bootloader reported? */
    {
        static const u64 ovl[]={0x14002000,0x14003000,0x14004000,0x14402000,0x14403000,0x14404000};
        u64 base=(fb_hi<<32)|fb_lo; int hit=0;
#ifdef UNGATE
        /* turn on every display clock gate in both MMSYS blocks (CG_CLR registers) */
        static const u64 mm[]={0x14000000,0x14400000};
        for(int m=0;m<2;m++){ *(volatile u32*)(mm[m]+0x108)=~0u; *(volatile u32*)(mm[m]+0x118)=~0u; *(volatile u32*)(mm[m]+0x1A8)=~0u; }
        __asm__ volatile("dsb sy");
        delay_us(1000);
#endif
        for(int o=0;o<6;o++) for(int l=0;l<4;l++){
            u64 a=*(volatile u32*)(ovl[o]+0xF40+0x20*l);
            if(a>=base && a<base+vram) hit=1;
        }
        reboot_after(hit?10:30);
    }
#endif
#ifdef UNGATE_KICK
    { static const u64 mm[]={0x14000000,0x14400000};
      for(int m=0;m<2;m++){ *(volatile u32*)(mm[m]+0x108)=~0u; *(volatile u32*)(mm[m]+0x118)=~0u; *(volatile u32*)(mm[m]+0x1A8)=~0u; }
      __asm__ volatile("dsb sy"); delay_us(1000); }
#endif
    volatile u32 *fb=(volatile u32*)((fb_hi<<32)|fb_lo);
    u64 n = vram/4, band = n/3;
#ifdef CONSOLE
    (void)band; (void)n;
    con_fb=fb; con_vram=vram;
    con_clear(0xFF000000u);
    /* colour check bar along the bottom: red | green | blue */
    for(u64 y=FB_H-240;y<FB_H-40;y++) for(u64 x=0;x<FB_W;x++)
        putpx(x,y, x<360?0xFFFF0000u:(x<720?0xFF00FF00u:0xFF0000FFu));
    u64 el; __asm__ volatile("mrs %0, CurrentEL":"=r"(el));
    con_puts("WP210 Pro bare-metal shim\n\n");
    con_puts("Hello from your own code!\n\n");
    con_puts("EL      "); con_dec((el>>2)&3); con_putc('\n');
    con_puts("DTB     "); con_hex((u64)fdt); con_putc('\n');
    con_puts("FB      "); con_hex((u64)fb); con_putc('\n');
    con_puts("VRAM    "); con_hex(vram); con_putc('\n');
    con_puts("CNTFRQ  "); con_dec(cntfrq()); con_putc('\n');
    con_puts("PANEL   1080x2412 cmd\n");
#else
    for(u64 i=0;i<n;i++) fb[i] = i<band ? 0xFF00FF00u : (i<2*band ? 0xFFFFFFFFu : 0xFFFF0000u);
#endif
    __asm__ volatile("dsb sy");
    kick_panel();
}
