#include "upstream_gx.h"
#include "../../../third_party/gcn-recomp/gx/render.h"
#include "../../../third_party/gcn-recomp/gx/texture.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
static std::vector<uint8_t> fifo, ram(0x1800000);
static int finishes, tokens;
static void finish(void*) { ++finishes; }
static void token(uint16_t t, int irq, void*) { if (t == 0x1234 && irq) ++tokens; }
static void u32(uint32_t v) { for (int i = 24; i >= 0; i -= 8) fifo.push_back(v >> i); }
static void f32(float v) { uint32_t u; memcpy(&u, &v, 4); u32(u); }
static void bp(uint8_t r, uint32_t v) { fifo.push_back(0x61); u32(uint32_t(r) << 24 | v); }
static void cp(uint8_t r, uint32_t v) { fifo.push_back(8); fifo.push_back(r); u32(v); }
static void xf(uint32_t r, std::initializer_list<float> values) {
    fifo.push_back(0x10); u32(uint32_t(values.size()-1) << 16 | r); for (float v : values) f32(v);
}
static void xw(uint32_t r, uint32_t v) { fifo.push_back(0x10); u32(r); u32(v); }
static void copy(bool clear) { bp(0x49,0); bp(0x4a,639 | (479 << 10)); bp(0x4b,0x100000 >> 5); bp(0x52,0x4000 | (clear ? 0x800 : 0)); }
static bool send() {
    // Every boundary may split a header, float, vertex index or BP word.
    for (size_t i = 0; i < fifo.size(); i += 7)
        if (upstream_gx_consume(fifo.data()+i, std::min<size_t>(7,fifo.size()-i))) return false;
    fifo.clear(); return upstream_gx_present() >= 0;
}
static void call_dl(uint32_t addr, uint32_t size) { fifo.push_back(0x40); u32(addr); u32(size); }
static bool bounds_failure(const char* label, std::initializer_list<const char*> expected) {
    if (upstream_gx_consume(fifo.data(), fifo.size()) != -1) {
        fprintf(stderr,"CALL_DL %s was accepted\n",label); return false;
    }
    const std::string error = upstream_gx_error();
    for (const char* field : expected) if (error.find(field) == std::string::npos) {
        fprintf(stderr,"CALL_DL %s missing diagnostic %s: %s\n",label,field,error.c_str());return false;
    }
    // Failure is sticky: neither a retry nor later commands can reapply earlier PE effects.
    fifo.clear(); bp(0x45,2);
    const int before = finishes;
    if(upstream_gx_consume(fifo.data(),fifo.size())!=-1 || finishes!=before || error!=upstream_gx_error())return false;
    fifo.clear();upstream_gx_reset();
    if(*upstream_gx_error())return false;
    fprintf(stderr,"CALL_DL bounds %s: %s\n",label,error.c_str());
    return true;
}
static bool display_list_bounds() {
    fifo.clear();upstream_gx_reset();
    // A partial 9-byte header must wait rather than read its unavailable length.
    call_dl(0x1000,4165206873u);
    if(upstream_gx_consume(fifo.data(),8))return false;
    std::vector<uint8_t> final_byte(1,fifo[8]);fifo.swap(final_byte);
    if(!bounds_failure("huge length",{"outside MEM1","addr=00001000","len=4165206873","parent=FIFO","parent_pos=0","command=[40 00 00 10 00"}))return false;
    call_dl(0x017fffff,2);
    if(!bounds_failure("end overrun",{"phys=017FFFFF","len=2","outside MEM1"}))return false;
    call_dl(0x01800000,1);
    if(!bounds_failure("base at end",{"phys=01800000","outside MEM1"}))return false;
    call_dl(0x82001000,9);
    if(!bounds_failure("masked address would wrap",{"addr=82001000","phys=02001000","outside MEM1"}))return false;
    // Last byte is legal, including the cached/uncached MEM1 address aliases.
    ram.back()=0;call_dl(0x817fffff,1);call_dl(0xc17fffff,1);call_dl(0x01800000,0);
    if(!send())return false;
    // The immediate parent's coordinate must survive a nested failure.
    bp(0x45,2);call_dl(0x01800000,1);
    memcpy(ram.data()+0x6000,fifo.data(),fifo.size());uint32_t list_size=fifo.size();fifo.clear();
    fifo.push_back(0);call_dl(0x80006000,list_size);bp(0x45,2);
    const int before=finishes;
    if(!bounds_failure("nested span",{"parent=DL","parent_addr=00006000","parent_raw=80006000","parent_pos=5","root_pos=1"})||finishes!=before+1)return false;
    // Exception unwinding restores the recursion stack; reset permits a valid list again.
    call_dl(0x7000,9);memcpy(ram.data()+0x7000,fifo.data(),9);
    if(!bounds_failure("recursive list",{"depth limit","depth=65","max_depth=64","parent_addr=00007000","command=[40 00 00 70 00 00 00 00 09]"}))return false;
    ram[0x7000]=0;call_dl(0x7000,1);return send();
}
// The regressions below use real FIFO state changes and GL pixels. In particular,
// changing RAM between consume calls must preserve the earlier draw in the batch.
static void pixel_setup() {
    cp(0x50,1<<9); cp(0x60,1); cp(0x70,1|(4<<1)|(1<<21)|(4<<22)); cp(0x30,60<<6);
    xf(0,{1,0,0,0,0,1,0,0,0,0,1,0});
    xf(0x101a,{320,-240,16777215,662,582,16777215});
    xf(0x1020,{1,0,1,0,1,0}); xw(0x1026,1); xw(0x103f,1); xw(0x1040,(1<<2)|(5<<7));
    xw(0x1009,1); xw(0x100c,0xffffffff);
    bp(0,0x11); bp(0x20,342|(342<<12)); bp(0x21,(342+479)|((342+639)<<12)); bp(0x59,171|(171<<10));
    bp(0x40,0); bp(0x41,0x18); bp(0x43,0); bp(0x28,1<<6);
    bp(0x80,0); bp(0x88,7|(7<<10)|(4<<20)); bp(0x94,0x2000>>5);
    bp(0xf6,4); bp(0xf7,14);
    bp(0xc0,(15<<12)|(15<<8)|(15<<4)|8|(1<<19));
    bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(4<<4)|(1<<19));
    bp(0xf3,(7<<16)|(7<<19));
}
static void quad(float left=-1, float right=1, float z=-0.5f) {
    const float xyuv[16]={left,1,0,0, right,1,1,0, right,-1,1,1, left,-1,0,1};
    fifo.push_back(0x80); fifo.push_back(0); fifo.push_back(4);
    for(int i=0;i<4;++i) { f32(xyuv[i*4]); f32(xyuv[i*4+1]); f32(z); f32(xyuv[i*4+2]); f32(xyuv[i*4+3]); }
}
static void fill_texture(uint16_t color) {
    for(int i=0;i<64;++i) { ram[0x2000+i*2]=color>>8; ram[0x2001+i*2]=color; }
}
static bool pixel_is(const std::vector<uint8_t>& px, int x, int y, int r, int g, int b, const char* label) {
    const uint8_t* p=&px[(y*640+x)*4];
    const auto near=[](int a,int b) { return a>=b-2 && a<=b+2; };
    if(near(p[0],r)&&near(p[1],g)&&near(p[2],b))return true;
    fprintf(stderr,"%s at %d,%d: got %u,%u,%u expected %d,%d,%d\n",label,x,y,p[0],p[1],p[2],r,g,b);return false;
}
static bool texture_cache_regressions() {
    upstream_gx_reset(); pixel_setup();
    fill_texture(0xf800); quad(-1,-0.34f); if(!send())return false;
    fill_texture(0x07e0); bp(0x66,0x1000); quad(-0.34f,0.34f); if(!send())return false;
    // Repeat the same invalidate payload: it is a command, not unchanged state.
    fill_texture(0x001f); bp(0x66,0x1000); quad(0.34f,1); copy(false); if(!send())return false;
    std::vector<uint8_t> pixels(640*480*4);
    if(upstream_gx_read_rgba(pixels.data(),pixels.size()))return false;
    bool ok=pixel_is(pixels,100,240,255,0,0,"pre-invalidate texture");
    ok=pixel_is(pixels,320,240,0,255,0,"same-frame invalidate")&&ok;
    ok=pixel_is(pixels,540,240,0,0,255,"repeated invalidate")&&ok;

    // A successful copy writes RAM. Its own writeback must not make the GPU copy
    // look CPU-overwritten on the next frame; a later CPU overwrite must still do so.
    memset(ram.data()+0x4000,0,128);
    bp(0x49,0); bp(0x4a,7|(7<<10)); bp(0x4b,0x4000>>5); bp(0x52,8<<3);
    if(!send())return false;
    gx::TexParams tp{0x4000,gx::TF_RGB565,8,8,1,0,0};
    std::vector<std::shared_ptr<gx::TexData>> textures;
    const gx::TexLookup before=gx::texture_lookup(tp,textures);
    copy(false); if(!send())return false;
    const gx::TexLookup after=gx::texture_lookup(tp,textures);
    if(!before.efb || !after.efb || before.id!=after.id) {
        fprintf(stderr,"EFB own-writeback invalidated copy: before=%u/%d after=%u/%d\n",before.id,before.efb,after.id,after.efb);ok=false;
    }
    ram[0x4000]^=0xff; bp(0x66,0x1000); if(!send())return false;
    if(gx::texture_lookup(tp,textures).efb) { fprintf(stderr,"CPU overwrite did not invalidate EFB copy\n");ok=false; }
    if(ok)fprintf(stderr,"texture cache: same-frame/repeated invalidate, ordered draws, EFB writeback/CPU overwrite passed\n");
    return ok;
}
static bool sampler_lod_regression() {
    upstream_gx_reset(); pixel_setup();
    // Two states which collided in the old 32-bit sampler key: maxLOD 1/5,
    // yielding 2/6 texture levels. Each mip is a distinct solid RGB565 color.
    const uint16_t colors[6]={0xf800,0x07e0,0x001f,0xffe0,0x07ff,0xf81f};
    uint32_t addr=0x8000;
    for(int level=0,w=32;level<6;++level,w=std::max(1,w/2)) {
        const uint32_t bytes=((w+3)/4)*((w+3)/4)*32;
        for(uint32_t i=0;i<bytes;i+=2) { ram[addr+i]=colors[level]>>8; ram[addr+i+1]=colors[level]; }
        addr+=bytes;
    }
    bp(0x88,31|(31<<10)|(4<<20)); bp(0x94,0x8000>>5); bp(0x80,5|(1<<5));
    const auto minified_quad=[](float left,float right) {
        const float v[16]={left,1,0,0, right,1,1024,0, right,-1,1024,1024, left,-1,0,1024};
        fifo.push_back(0x80); fifo.push_back(0); fifo.push_back(4);
        for(int i=0;i<4;++i) { f32(v[i*4]);f32(v[i*4+1]);f32(-0.5f);f32(v[i*4+2]);f32(v[i*4+3]); }
    };
    bp(0x84,0x1000); minified_quad(-1,0);
    bp(0x84,0x5000); minified_quad(0,1); copy(false); if(!send())return false;
    std::vector<uint8_t> pixels(640*480*4); if(upstream_gx_read_rgba(pixels.data(),pixels.size()))return false;
    bool ok=pixel_is(pixels,160,240,0,255,0,"sampler maxLOD 1");
    ok=pixel_is(pixels,480,240,255,0,255,"sampler maxLOD 5")&&ok;
    if(ok)fprintf(stderr,"sampler cache: distinct maxLOD and mip level count passed\n");
    return ok;
}
static bool matrix_depth_regressions() {
    upstream_gx_reset(); pixel_setup();
    bp(0x40,0x10); bp(0x4f,0xff00); bp(0x50,0); bp(0x51,0xffffff); copy(true);
    bp(0x40,0x13); // enabled, LESS, update
    // Default PNMTXIDX selects XF rows, not a matrix ordinal.
    xf(12,{0.3f,0,0,-0.65f, 0,0.5f,0,0, 0,0,1,0}); xw(0x1018,3);
    fill_texture(0xf800); quad(-1,1,-0.8f); if(!send())return false;
    // An indexed matrix load must invalidate the XF snapshot and select rows 6..8.
    const float m[12]={0.3f,0,0,0, 0,0.5f,0,0, 0,0,1,0};
    for(int i=0;i<12;++i) { uint32_t u; memcpy(&u,m+i,4); for(int b=0;b<4;++b)ram[0x6000+48+i*4+b]=u>>(24-b*8); }
    cp(0xac,0x6000); cp(0xbc,48); fifo.push_back(0x20); u32((1<<16)|(11<<12)|24); xw(0x1018,6);
    fill_texture(0x07e0); bp(0x66,0x1000); quad(-1,1,-0.8f); if(!send())return false;
    // Per-vertex index overrides the default, including masking the high bits.
    xf(36,{0.3f,0,0,0.65f, 0,0.5f,0,0, 0,0,1,0}); xw(0x1018,3); cp(0x50,(1<<9)|1);
    fill_texture(0x001f); bp(0x66,0x1000);
    fifo.push_back(0x80); fifo.push_back(0); fifo.push_back(4);
    const float v[8]={-1,1, 1,1, 1,-1, -1,-1};
    for(int i=0;i<4;++i) { fifo.push_back(0xc0|9); f32(v[i*2]); f32(v[i*2+1]); f32(-0.8f); f32(0); f32(0); }
    if(!send())return false;
    // A farther quad must fail LESS; disabling Z test also disables Z writes.
    cp(0x50,1<<9); xw(0x1018,0); fill_texture(0xffff); bp(0x66,0x1000); quad(-1,1,-0.4f);
    copy(false); if(!send())return false;
    std::vector<uint8_t> pixels(640*480*4); if(upstream_gx_read_rgba(pixels.data(),pixels.size()))return false;
    bool ok=pixel_is(pixels,110,240,255,0,0,"default matrix + depth");
    ok=pixel_is(pixels,320,240,0,255,0,"indexed matrix + depth")&&ok;
    ok=pixel_is(pixels,530,240,0,0,255,"vertex matrix + depth")&&ok;
    if(ok)fprintf(stderr,"matrices/depth: default rows, LOAD_INDX_A, per-vertex override, LESS/update passed\n");
    return ok;
}
static bool normal_lighting_regressions() {
    upstream_gx_reset(); pixel_setup(); fill_texture(0xffff);
    cp(0x50,(1<<9)|(1<<11)); cp(0x70,1|(4<<1)|(4<<10)|(1<<21)|(4<<22));
    xw(0x100a,0x202020ff); xw(0x100e,0x506); xw(0x1010,0);
    xw(0x603,0xdfdfdf00); xf(0x60a,{0,0,1000000});
    bp(0xc0,(15<<12)|(15<<8)|(15<<4)|10|(1<<19));
    const auto lit_quad=[](float left,float right) {
        fifo.push_back(0x80); fifo.push_back(0); fifo.push_back(4);
        const float xy[8]={left,1,right,1,right,-1,left,-1};
        for(int i=0;i<4;++i) { f32(xy[i*2]);f32(xy[i*2+1]);f32(-0.5f);f32(0);f32(0);f32(1);f32(0);f32(0); }
    };
    xf(0x400,{1,0,0,0,1,0,0,0,1}); lit_quad(-1,-0.34f);
    xf(0x400,{1,0,0,0,1,0,0,0,-1}); lit_quad(-0.34f,0.34f);
    // Position index row 3 selects normal rows 3..5 (nine words), while the
    // position matrix itself starts at word 12. This also invalidates snapshots.
    xf(12,{1,0,0,0,0,1,0,0,0,0,1,0});
    xf(0x409,{1,0,0,0,1,0,0,0,1}); xw(0x1018,3); lit_quad(0.34f,1);
    copy(false); if(!send())return false;
    std::vector<uint8_t> pixels(640*480*4); if(upstream_gx_read_rgba(pixels.data(),pixels.size()))return false;
    bool ok=pixel_is(pixels,100,240,255,255,255,"normal identity lighting");
    ok=pixel_is(pixels,320,240,32,32,32,"normal update lighting")&&ok;
    ok=pixel_is(pixels,540,240,255,255,255,"normal row index lighting")&&ok;
    if(ok)fprintf(stderr,"normals/lighting: snapshot update, normal row indexing, diffuse/ambient passed\n");
    return ok;
}
static bool destination_alpha_regressions() {
    bool ok=true;
    for(uint32_t format : {1u,0u}) {
        upstream_gx_reset(); pixel_setup();
        // Seed the backing alpha with zero in RGBA6, then switch formats. RGB8
        // has no alpha plane: DST_ALPHA must become one, not the stale GL alpha.
        bp(0x43,1); bp(0x41,0x18); bp(0x4f,0); bp(0x50,0); copy(true);
        bp(0x43,format); bp(0x41,8); fill_texture(0x001f); quad(); if(!send())return false;
        fill_texture(0xf800); bp(0x66,0x1000);
        bp(0x41,9|(6<<8)); quad(-1,-0.5f);
        bp(0x41,9|(7<<8)); quad(-0.5f,0);
        bp(0x41,9|(6<<5)); quad(0,0.5f);
        bp(0x41,9|(7<<5)); quad(0.5f,1); copy(false); if(!send())return false;
        std::vector<uint8_t> pixels(640*480*4); if(upstream_gx_read_rgba(pixels.data(),pixels.size()))return false;
        const int alpha=format==0 ? 255 : 0;
        ok=pixel_is(pixels,80,240,alpha,0,0,"source DST_ALPHA")&&ok;
        ok=pixel_is(pixels,240,240,255-alpha,0,0,"source INV_DST_ALPHA")&&ok;
        ok=pixel_is(pixels,400,240,0,0,alpha,"destination DST_ALPHA")&&ok;
        ok=pixel_is(pixels,560,240,0,0,255-alpha,"destination INV_DST_ALPHA")&&ok;
    }
    if(ok)fprintf(stderr,"blend: RGBA6 destination alpha and RGB8 implicit alpha passed\n");
    return ok;
}
int main(int argc, char**argv) {
    // Single-threaded fixture: render from consume() so EFB write-back waits
    // complete without a separate render thread.
#ifdef _WIN32
    _putenv_s("GC_GX_SYNC", "1");
#else
    setenv("GC_GX_SYNC", "1", 1);
#endif
    UpstreamGxConfig c{}; c.ram=ram.data(); c.ram_size=ram.size(); c.width=640; c.height=480; c.hidden=1; c.internal_scale=1; c.pe_finish=finish; c.pe_token=token;
    if(upstream_gx_init(&c)) { fprintf(stderr,"%s\n",upstream_gx_error()); return 1; }
    for (int pass=0; pass<3; ++pass) {
        if (pass == 2) {
            upstream_gx_shutdown();
            if (upstream_gx_init(&c)) return 8;
        }
        upstream_gx_reset();
        // Clear EFB through the real copy command. Clear is after presentation.
        bp(0x41,0x18); bp(0x40,0x10); bp(0x4f,0xff20); bp(0x50,0x3040); bp(0x51,0xffffff); copy(true);
        if(!send()) return 2;
        // RAM-backed RGB565 texture: four 4x4 GC tiles, four distinct colors.
        const uint16_t colors[4]={0xf800,0x07e0,0x001f,0xffe0};
        for(int tile=0;tile<4;++tile) for(int p=0;p<16;++p) { ram[0x2000+tile*32+p*2]=colors[tile]>>8; ram[0x2001+tile*32+p*2]=colors[tile]; }
        const float positions[12]={-0.75f,0.75f,-0.5f, 0.75f,0.75f,-0.5f, 0.75f,-0.75f,-0.5f, -0.75f,-0.75f,-0.5f};
        for(int i=0;i<12;++i){ uint32_t u;memcpy(&u,positions+i,4);for(int b=0;b<4;++b)ram[0x3000+i*4+b]=u>>(24-b*8); }
        cp(0x50,2<<9); cp(0x60,1); cp(0x70,1 | (4<<1) | (1<<21) | (4<<22)); cp(0xa0,0x3000); cp(0xb0,12); cp(0x30,60<<6);
        xf(0,{1,0,0,0,0,1,0,0,0,0,1,0});
        xf(0x101a,{320,-240,16777215,662,582,16777215});
        xf(0x1020,{1,0,1,0,1,0}); xw(0x1026,1); xw(0x103f,1); xw(0x1040,(1<<2)|(5<<7));
        xw(0x1009,1); xw(0x100c,0xffffffff);
        bp(0,0x11); bp(0x20,342|(342<<12)); bp(0x21,(342+639)|((342+479)<<12)); bp(0x59,171|(171<<10));
        bp(0x40,0); bp(0x41,0x18); bp(0x43,0); bp(0x28,1<<6);
        bp(0x80,0); bp(0x88,7|(7<<10)|(4<<20)); bp(0x94,0x2000>>5);
        bp(0xf6,4); bp(0xf7,14); // identity texture swap r,g,b,a
        bp(0xc0,(15<<12)|(15<<8)|(15<<4)|8|(1<<19));
        bp(0xc1,(7<<13)|(7<<10)|(7<<7)|(4<<4)|(1<<19));
        bp(0xf3,(7<<16)|(7<<19));
        fifo.push_back(0x80); fifo.push_back(0); fifo.push_back(4);
        const float uv[8]={0,0,1,0,1,1,0,1};
        for(int i=0;i<4;++i){fifo.push_back(i);f32(uv[i*2]);f32(uv[i*2+1]);}
        // Texture-copy readback must be in MEM1 before the following PE finish.
        bp(0x49,80|(60<<10)); bp(0x4a,7|(7<<10)); bp(0x4b,0x4000>>5); bp(0x52,8<<3);
        // CALL_DL remains in the upstream parser; HAL must not expand it twice.
        const uint8_t list[]={0x61,0x48,0x00,0x12,0x34,0x61,0x45,0x00,0x00,0x02};
        memcpy(ram.data()+0x5000,list,sizeof(list));
        fifo.push_back(0x40);u32(0x80005000);u32(sizeof(list));copy(false);
        if(!send()) {fprintf(stderr,"%s\n",upstream_gx_error());return 3;}
        std::vector<uint8_t> pixels(640*480*4);
        if(upstream_gx_read_rgba(pixels.data(),pixels.size())) return 4;
        if(upstream_gx_capture_png(argc>1?argv[1]:"/tmp/gx-fixture.png"))return 5;
        unsigned red=0,green=0,blue=0,yellow=0,bg=0;
        for(size_t i=0;i<pixels.size();i+=4){auto*p=&pixels[i];red+=p[0]>220&&p[1]<30&&p[2]<30;green+=p[1]>220&&p[0]<30&&p[2]<30;blue+=p[2]>220&&p[0]<30&&p[1]<30;yellow+=p[0]>220&&p[1]>220&&p[2]<30;bg+=p[0]==32&&p[1]==48&&p[2]==64;}
        fprintf(stderr,"fixture pass=%d frame=%u red=%u green=%u blue=%u yellow=%u clear=%u finish=%d token=%d\n",pass,upstream_gx_frame_count(),red,green,blue,yellow,bg,finishes,tokens);
        if(red<20000||green<20000||blue<20000||yellow<20000||bg<100000||finishes!=pass+1||tokens!=pass+1)return 6;
        if(ram[0x4000]!=0xf8 || ram[0x4001]!=0x00) { fprintf(stderr,"EFB RAM writeback wrong %02x%02x\n",ram[0x4000],ram[0x4001]); return 7; }
    }
    if(!display_list_bounds())return 9;
    if(!texture_cache_regressions())return 10;
    if(!matrix_depth_regressions())return 11;
    if(!destination_alpha_regressions())return 12;
    if(!normal_lighting_regressions())return 13;
    if(!sampler_lod_regression())return 14;
    upstream_gx_shutdown(); return 0;
}
