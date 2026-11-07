// Title-specific SAIL video bridge. Guest callbacks and frame memory remain
// owned by the game. FFmpeg decodes the user's PAMF through a private pipe.
#define NOMINMAX
#include <windows.h>
#include "ppu_recomp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <thread>
#include <mmsystem.h>

extern "C" void ps3_hle_register_ctx(unsigned, const char*, void (*)(ppu_context*));
extern "C" int cellSailPlayerInitialize2(unsigned,unsigned,unsigned,unsigned,unsigned,unsigned);
extern "C" int cellSailGraphicsAdapterInitialize(unsigned,unsigned,unsigned);
extern "C" int cellSailPlayerDestroy(unsigned);
extern "C" uint64_t ppu_guest_call(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
static struct {
    unsigned player, callback, argument, adapter, adapter_arg, callbacks[5];
    unsigned buffers[4], session, frame, coding, width=1280, height=720;
    bool allocated, running, paused;
    std::string path;
    HANDLE process=nullptr, pipe=nullptr, job=nullptr;
    std::chrono::steady_clock::time_point epoch;
    std::vector<unsigned char> pixels;
} movie;

[[noreturn]] static void fail(const char* why) {
    fprintf(stderr,"[bleach] STOP: SAIL %s (winerr=%lu)\n",why,GetLastError());
    fflush(stderr); std::exit(81);
}
static uint64_t invoke(ppu_context* ctx,unsigned opd,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
    if (!opd) return 0;
    return ppu_guest_call(opd,a,b,c,d,0,0,0,0);
}
static void notify(ppu_context* ctx,unsigned major,unsigned minor,uint64_t a=0,uint64_t b=0) {
    invoke(ctx,movie.callback,movie.argument,((uint64_t)major<<32)|minor,a,b);
}
static void transition(ppu_context*ctx,unsigned call,unsigned via,unsigned end,uint64_t token=0) {
    notify(ctx,3,0,via); notify(ctx,2,call,0,token); notify(ctx,3,0,end);
}
static void format(unsigned out) {
    for (unsigned i=0;i<32;++i) vm_write8(out+i,0);
    vm_write8(out,movie.coding); vm_write8(out+1,0); vm_write8(out+2,8); vm_write8(out+3,0);
    vm_write16(out+4,movie.width); vm_write16(out+6,movie.height);
    vm_write32(out+8,movie.width*4); vm_write32(out+12,255);
    vm_write8(out+16,1); vm_write8(out+17,1); vm_write8(out+18,0);
}
// ---------------------------------------------------------------------------
// Movie audio. PAMF carries ATRAC3plus in MPEG-PS private_stream_1 (0xBD,
// sub-stream 0x00): 4 private bytes per PES, then access units of an 8-byte
// PS3 header (0x0FD0, params, 4 bytes) and ((params&0x3FF)*8+8) bytes of
// frame. FFmpeg's MPEG demuxer does not expose that track, so it is unwrapped
// here into the PSP .at3 layout (RIFF, WAVE_FORMAT_EXTENSIBLE + ATRAC3plus
// GUID), decoded to 16-bit PCM by the same FFmpeg, and played on the host with
// waveOut against the video's own wall clock. Preparation runs off-thread; a
// late start skips ahead so sound never lags the picture. The guest sound
// adapter keeps receiving an empty queue (sound_frame below).
struct SailSound {
    std::thread prepare;
    std::atomic<bool> ready{false}, cancel{false};
    std::vector<int16_t> pcm;          // interleaved stereo
    unsigned rate=48000, channels=2;
    double lead=0;                     // seconds of audio before video frame 0
    HWAVEOUT out=nullptr; WAVEHDR header{}; bool playing=false;
};
// Never destroyed: a joinable std::thread in a static destructor would
// terminate the process when the game exits mid-movie.
static SailSound& sound=*new SailSound();
static uint64_t pes_pts(const std::vector<unsigned char>& d,size_t i) {
    if(i+14>d.size() || !(d[i+7]&0x80)) return 0;
    return (uint64_t)((d[i+9]>>1)&7)<<30 | (uint64_t)d[i+10]<<22 | (uint64_t)(d[i+11]>>1)<<15 |
           (uint64_t)d[i+12]<<7 | (uint64_t)(d[i+13]>>1);
}
static bool build_at3(const std::vector<unsigned char>& d,std::string& riff,double& lead) {
    std::string payload; uint64_t audio_pts=0,video_pts=0;
    for(size_t i=0;i+9<d.size();) {
        if(d[i]||d[i+1]||d[i+2]!=1) {++i;continue;}
        const unsigned id=d[i+3];
        if(id==0xBA){i+=14+(d[i+13]&7);continue;}               // pack header
        if(id<0xBB){++i;continue;}
        const size_t end=i+6+((size_t)d[i+4]<<8|d[i+5]);
        if(end>d.size()) break;
        if(id==0xE0 && !video_pts) video_pts=pes_pts(d,i);
        if(id==0xBD) {
            const size_t start=i+9+d[i+8];
            if(start+4<=end && d[start]==0x00) {
                if(!audio_pts) audio_pts=pes_pts(d,i);
                payload.append((const char*)&d[start+4],end-start-4);
            }
        }
        i=end;
    }
    std::string frames; unsigned params=0,count=0;
    for(size_t p=0;p+8<=payload.size();) {
        const unsigned sync=(unsigned char)payload[p]<<8|(unsigned char)payload[p+1];
        const unsigned prm=(unsigned char)payload[p+2]<<8|(unsigned char)payload[p+3];
        if(sync!=0x0FD0 || (params && prm!=params)) return false;
        params=prm; const size_t size=((prm&0x3FF)<<3)+8;
        if(p+8+size>payload.size()) break;
        frames.append(payload,p+8,size); p+=8+size; ++count;
    }
    if(!count || !audio_pts || !video_pts) return false;
    static const unsigned rates[8]={32000,44100,48000,88200,96000,0,0,0};
    const unsigned rate=rates[(params>>13)&7], channels=((params>>10)&7)==2?2:0, block=((params&0x3FF)<<3)+8;
    if(!rate || !channels) return false;
    sound.rate=rate; sound.channels=channels;
    lead=((double)video_pts-(double)audio_pts)/90000.0;
    auto u16=[&](std::string& s,unsigned v){s+=(char)(v&255);s+=(char)(v>>8);};
    auto u32=[&](std::string& s,unsigned v){u16(s,v&0xFFFF);u16(s,v>>16);};
    std::string fmt;
    u16(fmt,0xFFFE);u16(fmt,channels);u32(fmt,rate);u32(fmt,rate*block/2048);u16(fmt,block);u16(fmt,0);
    u16(fmt,34);u16(fmt,0);u32(fmt,3);
    static const unsigned char guid[16]={0xBF,0xAA,0x23,0xE9,0x58,0xCB,0x71,0x44,0xA1,0x19,0xFF,0xFA,0x01,0xE4,0xCE,0x62};
    fmt.append((const char*)guid,16);
    fmt+=(char)0;fmt+=(char)1;fmt+=(char)(params>>8);fmt+=(char)(params&255);fmt.append(8,'\0');
    std::string body="WAVE";
    body+="fmt ";u32(body,(unsigned)fmt.size());body+=fmt;
    body+="fact";u32(body,8);u32(body,count*2048);u32(body,0);
    body+="data";u32(body,(unsigned)frames.size());body+=frames;
    riff="RIFF";u32(riff,(unsigned)body.size());riff+=body;
    return true;
}
static void prepare_sound(std::string path,std::filesystem::path ffmpeg) {
    std::vector<unsigned char> data;
    { FILE* f=_wfopen(std::filesystem::path(path).c_str(),L"rb"); if(!f) return;
      fseek(f,0,SEEK_END); data.resize((size_t)ftell(f)); fseek(f,0,SEEK_SET);
      const size_t got=fread(data.data(),1,data.size(),f); fclose(f); if(got!=data.size()) return; }
    std::string riff; double lead=0;
    if(!build_at3(data,riff,lead)) { fprintf(stderr,"[sail-audio] no ATRAC3plus track in %s\n",path.c_str()); return; }
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH,tmp);
    const std::filesystem::path at3=std::filesystem::path(tmp)/(L"bleach-sail-"+std::to_wstring(GetCurrentProcessId())+L".at3");
    { FILE* f=_wfopen(at3.c_str(),L"wb"); if(!f) return; fwrite(riff.data(),1,riff.size(),f); fclose(f); }
    std::wstring command=L"\""+ffmpeg.wstring()+L"\" -hide_banner -loglevel error -nostdin -i \""+at3.wstring()+
        L"\" -f s16le -acodec pcm_s16le pipe:1";
    SECURITY_ATTRIBUTES sa={sizeof(sa),nullptr,TRUE}; HANDLE read=nullptr,write=nullptr;
    if(!CreatePipe(&read,&write,&sa,1<<20)) return;
    SetHandleInformation(read,HANDLE_FLAG_INHERIT,0);
    HANDLE null=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    STARTUPINFOW si={sizeof(si)}; si.dwFlags=STARTF_USESTDHANDLES; si.hStdInput=null; si.hStdOutput=write; si.hStdError=null;
    PROCESS_INFORMATION pi={};
    const BOOL ok=CreateProcessW(ffmpeg.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
    CloseHandle(write); CloseHandle(null);
    if(!ok) { CloseHandle(read); return; }
    std::vector<int16_t> pcm; std::vector<char> chunk(1<<16); size_t carry=0; char odd=0;
    for(DWORD got=0; !sound.cancel && ReadFile(read,chunk.data(),(DWORD)chunk.size(),&got,nullptr) && got;) {
        size_t i=0;
        if(carry) { pcm.push_back((int16_t)((unsigned char)odd|(unsigned char)chunk[0]<<8)); i=1; carry=0; }
        for(; i+1<got; i+=2) pcm.push_back((int16_t)((unsigned char)chunk[i]|(unsigned char)chunk[i+1]<<8));
        if(i<got) { odd=chunk[i]; carry=1; }
    }
    CloseHandle(read);
    if(sound.cancel) TerminateProcess(pi.hProcess,0);
    WaitForSingleObject(pi.hProcess,5000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    std::error_code ec; std::filesystem::remove(at3,ec);
    if(sound.cancel || pcm.empty()) return;
    // PS3_VOLUME: movie audio bypasses cellAudio, so the master gain is applied here too.
    if(const char* e=getenv("PS3_VOLUME")) {
        const double gain=std::max(0.0,atof(e));
        if(gain!=1.0) for(auto& s:pcm) s=(int16_t)std::clamp((int)std::lround(s*gain),-32768,32767);
    }
    sound.pcm.swap(pcm); sound.lead=lead; sound.ready=true;
    fprintf(stderr,"[sail-audio] ATRAC3plus decoded: %.2f s, %u Hz, lead %.1f ms\n",
        sound.pcm.size()/(double)sound.channels/sound.rate,sound.rate,lead*1000.0);
}
static void stop_sound() {
    sound.cancel=true;
    if(sound.prepare.joinable()) sound.prepare.join();
    if(sound.out) {
        waveOutReset(sound.out);
        if(sound.header.dwFlags&WHDR_PREPARED) waveOutUnprepareHeader(sound.out,&sound.header,sizeof(sound.header));
        waveOutClose(sound.out); sound.out=nullptr;
    }
    sound.header={}; sound.playing=false; sound.ready=false; sound.pcm.clear(); sound.cancel=false;
}
static void begin_sound(const std::filesystem::path& ffmpeg) {
    stop_sound();
    sound.prepare=std::thread(prepare_sound,movie.path,ffmpeg);
}
// Called from the video clock: starts playback, skipping ahead if late.
static void pump_sound(double elapsed) {
    if(sound.playing || !sound.ready) return;
    sound.playing=true;
    const size_t frames=sound.pcm.size()/sound.channels;
    const double position=elapsed+sound.lead;
    const size_t skip=position>0?std::min(frames,(size_t)(position*sound.rate)):0;
    if(skip>=frames) return;
    WAVEFORMATEX wf={}; wf.wFormatTag=WAVE_FORMAT_PCM; wf.nChannels=(WORD)sound.channels; wf.nSamplesPerSec=sound.rate;
    wf.wBitsPerSample=16; wf.nBlockAlign=(WORD)(sound.channels*2); wf.nAvgBytesPerSec=sound.rate*wf.nBlockAlign;
    if(waveOutOpen(&sound.out,WAVE_MAPPER,&wf,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR) { sound.out=nullptr; return; }
    sound.header.lpData=(LPSTR)(sound.pcm.data()+skip*sound.channels);
    sound.header.dwBufferLength=(DWORD)((frames-skip)*sound.channels*sizeof(int16_t));
    if(waveOutPrepareHeader(sound.out,&sound.header,sizeof(sound.header))==MMSYSERR_NOERROR)
        waveOutWrite(sound.out,&sound.header,sizeof(sound.header));
    fprintf(stderr,"[sail-audio] playback at %.3f s (skipped %zu samples)\n",elapsed,skip);
}

static void close_decoder() {
    stop_sound();
    movie.running=false;
    if(movie.process) { TerminateProcess(movie.process,0); WaitForSingleObject(movie.process,1000); CloseHandle(movie.process); movie.process=nullptr; }
    if(movie.pipe) {CloseHandle(movie.pipe);movie.pipe=nullptr;}
    if(movie.job) {CloseHandle(movie.job);movie.job=nullptr;}
}
static std::filesystem::path ffmpeg_path() {
    // BLEACH_FFMPEG, else the ffmpeg.exe installed next to this executable.
    const char* ffmpeg=getenv("BLEACH_FFMPEG");
    std::filesystem::path exe;
    if(ffmpeg) exe=ffmpeg;
    else { wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr,self,MAX_PATH); exe=std::filesystem::path(self).parent_path()/L"ffmpeg.exe"; }
    if(!std::filesystem::exists(exe)) fail("decoder executable missing; set BLEACH_FFMPEG");
    return exe;
}
static void open_decoder() {
    close_decoder();
    const std::filesystem::path exe=ffmpeg_path();
    std::wstring command=L"\""+exe.wstring()+L"\" -hide_banner -loglevel error -nostdin -threads 2 -f mpeg -skip_initial_bytes 2048 -i \""+
        std::filesystem::path(movie.path).wstring()+L"\" -map 0:v:0 -an -sn -pix_fmt "+(movie.coding==0?L"argb":L"rgba")+L" -fps_mode passthrough -f rawvideo pipe:1";
    SECURITY_ATTRIBUTES sa={sizeof(sa),nullptr,TRUE}; HANDLE output=nullptr;
    if(!CreatePipe(&movie.pipe,&output,&sa,1<<20)) fail("CreatePipe failed");
    SetHandleInformation(movie.pipe,HANDLE_FLAG_INHERIT,0);
    HANDLE null=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
    HANDLE errors=CreateFileW(L"sail-decoder.log",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,CREATE_ALWAYS,0,nullptr);
    STARTUPINFOW si={sizeof(si)}; si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=null; si.hStdOutput=output; si.hStdError=errors;
    PROCESS_INFORMATION pi={};
    BOOL ok=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&si,&pi);
    CloseHandle(output);CloseHandle(null);CloseHandle(errors);
    if(!ok) fail("CreateProcess decoder failed");
    movie.process=pi.hProcess;
    movie.job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!movie.job || !SetInformationJobObject(movie.job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) || !AssignProcessToJobObject(movie.job,pi.hProcess)) {
        CloseHandle(pi.hThread);close_decoder();fail("decoder lifetime setup failed");
    }
    ResumeThread(pi.hThread);CloseHandle(pi.hThread);
    movie.pixels.resize(movie.width*movie.height*4);
}
static void initialize(ppu_context* c) {
    movie.player=c->gpr[3];movie.callback=c->gpr[5];movie.argument=c->gpr[6];
    c->gpr[3]=cellSailPlayerInitialize2(c->gpr[3],c->gpr[4],c->gpr[5],c->gpr[6],c->gpr[7],c->gpr[8]);
}
static void graphics_initialize(ppu_context*c) {
    movie.adapter=c->gpr[3];movie.adapter_arg=c->gpr[5];
    for(unsigned i=0;i<5;++i) movie.callbacks[i]=vm_read32(c->gpr[4]+i*4);
    fprintf(stderr,"[sail-video] callbacks %08X %08X %08X %08X %08X\n",movie.callbacks[0],movie.callbacks[1],movie.callbacks[2],movie.callbacks[3],movie.callbacks[4]);
    c->gpr[3]=cellSailGraphicsAdapterInitialize(c->gpr[3],c->gpr[4],c->gpr[5]);
}
static void preferred(ppu_context*c) {
    movie.coding=vm_read8(c->gpr[4]);
    fprintf(stderr,"[sail-video] preferred coding=%u\n",movie.coding);
    if(movie.coding>1) fail("unsupported requested pixel format");
    c->gpr[3]=0;
}
static void descriptor(ppu_context*c) {
    std::string uri; for(unsigned i=0;i<1024;++i) {char v=vm_read8(c->gpr[6]+i);if(!v)break;uri+=v;}
    const std::string prefix="x-cell-fs:///dev_bdvd/";
    if(uri.rfind(prefix,0)!=0 || uri.find("..")!=std::string::npos) fail("unsupported media URI");
    const char* root=getenv("PS3_VFS_ROOT");if(!root) fail("missing PS3_VFS_ROOT");
    movie.path=(std::filesystem::path(root)/uri.substr(prefix.size())).string();
    if(!std::filesystem::exists(movie.path)) fail("media file not found");
    fprintf(stderr,"[sail-video] descriptor %s\n",uri.c_str());
    vm_write32(c->gpr[7],0x5341494c);c->gpr[3]=0;
}
static void add_descriptor(ppu_context*c) { if(c->gpr[4]!=0x5341494c) fail("invalid descriptor");c->gpr[3]=0; }
static void open_stream(ppu_context*c) {
    unsigned scratch=(unsigned)c->gpr[1]-0x100;
    if(movie.path.empty()) fail("open without descriptor");
    ++movie.session;
    invoke(c,movie.callbacks[0],movie.adapter_arg);
    format(scratch);invoke(c,movie.callbacks[2],movie.adapter_arg,scratch,movie.session);
    for(unsigned i=0;i<4;++i) vm_write32(scratch+64+i*4,0);
    auto rc=invoke(c,movie.callbacks[3],movie.adapter_arg,movie.width*movie.height*4,4,scratch+64);
    if((int32_t)rc<0) fail("guest frame allocation failed");
    for(unsigned i=0;i<4;++i) {movie.buffers[i]=vm_read32(scratch+64+i*4);if(!movie.buffers[i]) fail("guest frame allocation returned null");}
    movie.allocated=true;
    fprintf(stderr,"[sail-video] frame buffers %08X %08X %08X %08X\n",movie.buffers[0],movie.buffers[1],movie.buffers[2],movie.buffers[3]);
    open_decoder();begin_sound(ffmpeg_path());transition(c,2,3,4,c->gpr[4]);c->gpr[3]=0;
}
static void start(ppu_context*c) {
    if(!movie.process) fail("start without decoder");
    movie.frame=0;movie.running=true;movie.paused=false;movie.epoch=std::chrono::steady_clock::now();
    transition(c,10,5,6,c->gpr[5]);c->gpr[3]=0;
    fprintf(stderr,"[sail-video] playback started, 1280x720 24000/1001 fps\n");
}
static void publish(ppu_context*c,unsigned index,bool repeated) {
    unsigned out=c->gpr[4];
    vm_write32(out,movie.buffers[index%4]);vm_write32(out+4,movie.session);
    vm_write32(out+8,index%4);vm_write32(out+12,0);
    vm_write64(out+16,90000+(uint64_t)index*90000*1001/24000);
    // SAIL returns sync flags, not CELL_OK: NEXT=2<<8, REPEATED=1<<8.
    c->gpr[3]=repeated?0x100:0x200;
}
static void frame(ppu_context*c) {
    unsigned out=c->gpr[4];for(unsigned i=0;i<24;i+=4)vm_write32(out+i,0);
    if(!movie.running||movie.paused) {c->gpr[3]=0;return;}
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-movie.epoch).count();
    pump_sound(elapsed);
    if(elapsed+0.001<movie.frame*1001.0/24000.0) {if(movie.frame)publish(c,movie.frame-1,true);else c->gpr[3]=0;return;}
    size_t bytes=0;
    while(bytes<movie.pixels.size()) {
        DWORD got=0;
        if(!ReadFile(movie.pipe,movie.pixels.data()+bytes,(DWORD)(movie.pixels.size()-bytes),&got,nullptr)||!got) {
            DWORD code=STILL_ACTIVE;WaitForSingleObject(movie.process,1000);GetExitCodeProcess(movie.process,&code);
            if(bytes||code!=0) fail("decoder ended with error or partial frame");
            fprintf(stderr,"[sail-video] decoded end of stream, frames=%u\n",movie.frame);
            close_decoder();notify(c,8,0);notify(c,3,0,7);notify(c,3,0,4);c->gpr[3]=0;return;
        }
        bytes+=got;
    }
    unsigned buffer=movie.buffers[movie.frame%4];
    std::memcpy(vm_base+buffer,movie.pixels.data(),bytes);
    publish(c,movie.frame,false);
    if(movie.frame<3 || movie.frame%120==0) fprintf(stderr,"[sail-video] frame=%u buffer=%08X pts=%llu\n",movie.frame,buffer,(unsigned long long)vm_read64(out+16));
    ++movie.frame;
}
static void get_format(ppu_context*c){format(c->gpr[4]);c->gpr[3]=0;}
static void pts_to_time(ppu_context*c){vm_write64(c->gpr[5],(c->gpr[4]-90000)*1000000/90000);c->gpr[3]=0;}
static void avsync(ppu_context*c){c->gpr[3]=0;}
// No audio stream is exposed by the MPEG demuxer for this PAMF yet. Return an
// empty queue (never fabricated samples); video progress remains independent.
static void sound_frame(ppu_context*c){
    unsigned out=c->gpr[5];for(unsigned i=0;i<24;i+=4)vm_write32(out+i,0);
    c->gpr[3]=0;
}
static void stop(ppu_context*c){close_decoder();transition(c,11,7,4,c->gpr[4]);c->gpr[3]=0;}
static void close_stream(ppu_context*c){
    close_decoder();unsigned scratch=(unsigned)c->gpr[1]-0x100;
    if(movie.allocated){for(unsigned i=0;i<4;++i)vm_write32(scratch+i*4,movie.buffers[i]);invoke(c,movie.callbacks[4],movie.adapter_arg,4,scratch);movie.allocated=false;}
    invoke(c,movie.callbacks[1],movie.adapter_arg);transition(c,3,8,2,c->gpr[4]);c->gpr[3]=0;
}
static void destroy_descriptor(ppu_context*c){movie.path.clear();c->gpr[3]=0;}
static void finalize(ppu_context*c){
    close_decoder();
    if(movie.allocated) fail("player finalized before frame buffers were released");
    c->gpr[3]=cellSailPlayerDestroy(movie.player);movie.player=0;
}
static void graphics_finalize(ppu_context*c){
    if(movie.allocated)fail("graphics adapter finalized with live buffers");
    movie.adapter=0;c->gpr[3]=0;
}
static void sound_finalize(ppu_context*c){c->gpr[3]=0;}
// cellSailPlayerSetPaused(player, paused): frames stop (frame() returns none while
// paused) and the host audio pauses; on resume the video clock skips the paused time.
static void set_paused(ppu_context*c){
    static std::chrono::steady_clock::time_point since;
    const bool pause=(c->gpr[4]&0xFF)!=0;
    if(pause && movie.running && !movie.paused) {
        since=std::chrono::steady_clock::now(); movie.paused=true;
        if(sound.out) waveOutPause(sound.out);
    } else if(!pause && movie.paused) {
        movie.epoch+=std::chrono::steady_clock::now()-since; movie.paused=false;
        if(sound.out) waveOutRestart(sound.out);
    }
    fprintf(stderr,"[sail] SetPaused(%d)\n",(int)pause);
    c->gpr[3]=0;
}
extern "C" void bleach_register_sail() {
    struct Entry {unsigned nid;const char*name;void(*fn)(ppu_context*);};
    Entry entries[]={
        {0x23654375,"cellSailPlayerInitialize2",initialize},{0x1c983864,"cellSailGraphicsAdapterInitialize",graphics_initialize},
        {0x2e3ccb5e,"cellSailGraphicsAdapterSetPreferredFormat",preferred},{0xd7938b8d,"cellSailPlayerCreateDescriptor",descriptor},
        {0x7c8dff3b,"cellSailPlayerAddDescriptor",add_descriptor},{0x34ecc1b9,"cellSailPlayerOpenStream",open_stream},
        {0xe535b0d3,"cellSailPlayerStart",start},{0x0247c69e,"cellSailGraphicsAdapterGetFrame",frame},
        {0xffd58aa4,"cellSailGraphicsAdapterGetFormat",get_format},{0x1872331b,"cellSailGraphicsAdapterPtsToTimePosition",pts_to_time},
        {0x44a20e79,"cellSailGraphicsAdapterUpdateAvSync",avsync},{0xeba8d4ec,"cellSailPlayerStop",stop},
        {0x7eb8d6b5,"cellSailSoundAdapterGetFrame",sound_frame},
        {0xeec22809,"cellSailSoundAdapterUpdateAvSync",avsync},
        {0x18b4629d,"cellSailPlayerFinalize",finalize},{0x76488bb1,"cellSailGraphicsAdapterFinalize",graphics_finalize},
        {0xd1462438,"cellSailSoundAdapterFinalize",sound_finalize},
        {0xd1d55a90,"cellSailPlayerSetPaused",set_paused},
        {0x85beffcc,"cellSailPlayerCloseStream",close_stream},{0xfc839bd4,"cellSailPlayerDestroyDescriptor",destroy_descriptor}};
    for(auto&e:entries) ps3_hle_register_ctx(e.nid,e.name,e.fn);
}
