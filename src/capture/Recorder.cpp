#include "Recorder.hpp"
#include "CaptureScheduler.hpp"
#include "AudioCapture.hpp"
#include "../ui/StudioState.hpp"
#include "../imgui_hook/ImGuiManager.hpp"
#include <Geode/loader/Mod.hpp>
#include <Geode/loader/Log.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>

static double nowS(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}static std::wstring w2(const std::string& s){if(s.empty())return{};int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),0,0);std::wstring w(n,0);MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],n);return w;}
static std::string u8s(const std::filesystem::path& p){auto u=p.u8string();return std::string(u.begin(),u.end());}
static std::string q(const std::string& s){std::string r="\"";for(char c:s){if(c=='\"')r+="\\\"";else r+=c;}r+='\"';return r;}
static std::string ffPath(){
    auto& st=studioState();
    if(!st.ffmpegPath.empty()&&std::filesystem::exists(std::filesystem::path(w2(st.ffmpegPath))))return st.ffmpegPath;
    auto b=geode::Mod::get()->getResourcesDir()/"ffmpeg.exe";
    if(std::filesystem::exists(b))return u8s(b);
    return "ffmpeg";
}
static std::string outDir(const std::string& c){
    if(!c.empty())return c;
    wchar_t u[260];if(GetEnvironmentVariableW(L"USERPROFILE",u,260)>0){
        std::wstring w(u);int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),0,0,0,0);
        std::string s(n,0);WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&s[0],n,0,0);return s+"\\Videos";
    }return ".";
}
static std::string tsN(){std::time_t t=std::time(nullptr);std::tm tm{};localtime_s(&tm,&t);char b[64];std::strftime(b,64,"GDSR_%Y%m%d_%H%M%S",&tm);return b;}

// Safety ceiling on how long the render thread will block waiting for the
// encoder to drain a queue slot in "borrow fps" mode. Long enough to pace GD
// down to the encode rate; short enough that a wedged ffmpeg can't hang GD.
static constexpr int kBackpressureCapMs = 500;

Recorder& Recorder::get(){static Recorder inst;return inst;}
Recorder::~Recorder(){
    m_running.store(false);
    if(m_worker.joinable()){
        m_queueCv.notify_all();
        m_spaceCv.notify_all();
        m_worker.join();
    }
    if(m_finalizeThread.joinable()) m_finalizeThread.join();
    stopFfmpeg();
    AudioCapture::get().stop();
}
float Recorder::seconds()const{double s=m_startTs.load();return(m_running.load()&&s>0.0)?(float)(nowS()-s):0.0f;}
std::string Recorder::lastError() const {
    std::lock_guard<std::mutex> lk(m_stateMutex);
    return m_lastError;
}
void Recorder::setLastError(std::string error){
    std::lock_guard<std::mutex> lk(m_stateMutex);
    m_lastError = std::move(error);
}
bool Recorder::startFfmpeg(){
    stopFfmpeg();
    auto& st=studioState();
    int qv=st.quality<0?0:(st.quality>51?51:st.quality);
    int br=st.bitrateKbps>0?st.bitrateKbps:4000;

    // Capture rows are bottom-up (OpenGL convention; the D3D11 backend flips its
    // top-down readback to match). Feed those raw pixels through stdin and flip
    // them back in ffmpeg.
    std::string vf="vflip";
    if(st.outWidth>0&&st.outHeight>0&&(st.outWidth!=m_capW||st.outHeight!=m_capH))
        vf+=",scale="+std::to_string(st.outWidth)+":"+std::to_string(st.outHeight)+":flags=fast_bilinear";
    int ei=(int)st.videoEncoder;if(ei<0||ei>4)ei=0;
    bool isMjpeg=(ei==4);
    vf+=isMjpeg?",format=yuvj420p":",format=yuv420p";
    std::string ec;
    switch(ei){
        case 1: ec="-c:v h264_nvenc -preset p1 -tune ll -rc constqp -qp "+std::to_string(qv)+" -b:v "+std::to_string(br)+"k -bf 0"; break;
        case 2: ec="-c:v h264_amf -usage ultralowlatency -quality speed -rc cqp -qp_i "+std::to_string(qv)+" -qp_p "+std::to_string(qv)+" -b:v "+std::to_string(br)+"k"; break;
        case 3: ec="-c:v h264_qsv -preset veryfast -global_quality "+std::to_string(qv)+" -look_ahead 0 -b:v "+std::to_string(br)+"k"; break;
        case 4: ec="-c:v mjpeg -q:v 4 -huffman optimal"; break;
        default:
            ec="-c:v libx264 -preset ultrafast -tune zerolatency -crf "+std::to_string(qv)+" -maxrate "+std::to_string(br)+"k -bufsize "+std::to_string(br*2)+"k -pix_fmt yuv420p -x264-params keyint="+std::to_string(m_fps*2)+":min-keyint="+std::to_string(m_fps)+":scenecut=0";
            break;
    }

    std::ostringstream cmd;
    cmd << q(ffPath()) << " -hide_banner -loglevel error -y "
        << "-f rawvideo -pix_fmt " << (m_inputBGRA ? "bgra" : "rgba") << ' '
        << "-video_size " << m_capW << 'x' << m_capH << ' '
        << "-framerate " << m_fps << " -i - "
        << "-vf " << q(vf) << ' ' << ec << ' '
        // NOTE: deliberately NO "-movflags +faststart" here. faststart relocates
        // the moov atom to the front in a SECOND full-file pass when stdin closes.
        // On a long recording that pass reads+rewrites the whole file and easily
        // outruns stopFfmpeg's wait, so ffmpeg is terminated mid-relocation and the
        // temp video is left corrupt -> the later mux fails ("mux: ffmpeg code 1").
        // moov-at-end plays fine locally and the mux reads it without issue.
        << "-r " << m_fps; if(!isMjpeg) cmd << " -fps_mode cfr"; cmd << " -an " << q(m_videoTmpFile);

    SECURITY_ATTRIBUTES sa{sizeof(sa),0,TRUE};HANDLE rd=nullptr,wr=nullptr;
    DWORD pipeBuffer=(DWORD)std::min<size_t>(std::max<size_t>(m_bgraBytes*2,1u<<20),64ull*1024ull*1024ull);
    if(!CreatePipe(&rd,&wr,&sa,pipeBuffer)){setLastError("pipe failed");return false;}
    SetHandleInformation(wr,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=rd;
    HANDLE nul=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,0);
    si.hStdOutput=nul;si.hStdError=nul;
    PROCESS_INFORMATION pi{};auto wc=w2(cmd.str());
    DWORD flags=CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS;
    if(!CreateProcessW(nullptr,wc.data(),nullptr,nullptr,TRUE,flags,nullptr,nullptr,&si,&pi)){
        CloseHandle(wr);CloseHandle(rd);if(nul&&nul!=INVALID_HANDLE_VALUE)CloseHandle(nul);setLastError("ffmpeg failed");return false;}
    CloseHandle(rd);if(nul&&nul!=INVALID_HANDLE_VALUE)CloseHandle(nul);CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lk(m_ffmpegMutex);
        m_ffmpegProc=pi.hProcess;
        m_ffmpegStdinWr=wr;
    }
    return true;
}
void Recorder::stopFfmpeg(){
    std::lock_guard<std::mutex> lk(m_ffmpegMutex);
    // rawvideo input finishes when stdin is closed. Do not write "q" here,
    // because ffmpeg would interpret it as a couple of corrupted pixel bytes.
    if(m_ffmpegStdinWr){CloseHandle((HANDLE)m_ffmpegStdinWr);m_ffmpegStdinWr=nullptr;}
    // Give ffmpeg generous time to flush its remaining pipe backlog and write the
    // moov atom for a long recording (slow laptop HDD). Terminating early here is
    // what corrupts the file; the queue is bounded so a healthy ffmpeg still exits
    // in seconds — the long ceiling only matters for a genuinely wedged process.
    if(m_ffmpegProc){DWORD wait=WaitForSingleObject((HANDLE)m_ffmpegProc,120000);DWORD e=0;GetExitCodeProcess((HANDLE)m_ffmpegProc,&e);if(wait==WAIT_TIMEOUT||e==STILL_ACTIVE)TerminateProcess((HANDLE)m_ffmpegProc,0);CloseHandle((HANDLE)m_ffmpegProc);m_ffmpegProc=nullptr;}
}
void Recorder::tick(){
    HANDLE proc=nullptr;
    HANDLE stdinWr=nullptr;
    {
        std::lock_guard<std::mutex> lk(m_ffmpegMutex);
        proc = (HANDLE)m_ffmpegProc;
        stdinWr = (HANDLE)m_ffmpegStdinWr;
    }
    if(!m_running.load()||!proc)return;
    DWORD wait=WaitForSingleObject(proc,0);
    if(wait!=WAIT_OBJECT_0)return;
    DWORD e=0;GetExitCodeProcess(proc,&e);
    {
        std::lock_guard<std::mutex> lk(m_ffmpegMutex);
        if(m_ffmpegStdinWr){CloseHandle((HANDLE)m_ffmpegStdinWr);m_ffmpegStdinWr=nullptr;}
        if(m_ffmpegProc){CloseHandle((HANDLE)m_ffmpegProc);m_ffmpegProc=nullptr;}
    }
    m_running.store(false);m_finalizing.store(false);m_saveProgress.store(100);m_queueCv.notify_all();m_spaceCv.notify_all();
    timeEndPeriod(1);
    setLastError("ffmpeg stopped unexpectedly, code "+std::to_string(e));
    geode::log::error("GDSR: ffmpeg stopped unexpectedly, code {}", e);
}

bool Recorder::writePipe(const unsigned char* d,size_t bytes){
    HANDLE stdinWr = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_ffmpegMutex);
        stdinWr = (HANDLE)m_ffmpegStdinWr;
    }
    if(!stdinWr||!d||bytes==0)return false;
    const unsigned char* p=d;
    size_t left=bytes;
    while(left>0){
        DWORD chunk=(DWORD)(left>(1u<<20)?(1u<<20):left);
        DWORD written=0;
        if(!WriteFile(stdinWr,p,chunk,&written,nullptr)||written==0)return false;
        p+=written;left-=written;
    }
    return true;
}

bool Recorder::submitFrame(const unsigned char* data,size_t bytes,int w,int h,int repeat){
    if(!m_running.load()||!data||bytes!=m_bgraBytes||w!=m_capW||h!=m_capH)return false;
    if(repeat<1)repeat=1;
    std::vector<unsigned char> copy(data,data+bytes);
    return submitFrame(std::move(copy),w,h,repeat);
}

bool Recorder::submitFrame(std::vector<unsigned char>&& data,int w,int h,int repeat){
    if(!m_running.load()||data.size()!=m_bgraBytes||w!=m_capW||h!=m_capH)return false;
    if(repeat<1)repeat=1;
    {
        std::unique_lock<std::mutex> lk(m_queueMutex);
        if(m_frameQueue.size()>=kMaxQueued){
            if(m_borrowFps){
                // Encoder behind: block the render thread until a slot drains so
                // GD is paced to the encode rate ("borrow fps from the game")
                // rather than dropping/freezing frames. Bounded — on timeout we
                // fall back to the repeat-extend below so a wedged ffmpeg can't
                // hang GD forever.
                m_spaceCv.wait_for(lk, std::chrono::milliseconds(kBackpressureCapMs),
                    [this]{ return !m_running.load() || m_frameQueue.size()<kMaxQueued; });
                if(!m_running.load())return false;
            }
            if(m_frameQueue.size()>=kMaxQueued){
                // Still full (borrow disabled, or backpressure timed out): extend
                // the newest queued frame instead of growing the queue. Keeps
                // memory flat and preserves wall-clock duration.
                if(!m_frameQueue.empty())m_frameQueue.back().repeat+=repeat;
                m_droppedFrames.fetch_add(1);
                return true;
            }
        }
        m_frameQueue.push_back(QueuedFrame{std::move(data),repeat});
    }
    m_queueCv.notify_one();
    return true;
}

bool Recorder::start(HWND hwnd){
    if(m_finalizeThread.joinable()) m_finalizeThread.join();
    if(m_running.load())return true;if(m_finalizing.load()){setLastError("Saving previous...");return false;}
    setLastError("");if(!hwnd){setLastError("No window");return false;}if(m_worker.joinable())m_worker.join();
    auto& st=studioState();m_fps=(st.recFps>0)?st.recFps:60; if(m_fps<1)m_fps=1; if(m_fps>300)m_fps=300;m_hwnd=hwnd;

    ImGuiManager::get().ensureCaptureReady();
    ImGuiManager::get().plannedRecordSize(m_capW,m_capH);
    m_srcX=0;m_srcY=0;
    if(m_capW<=0||m_capH<=0){setLastError("Bad dims");return false;}

    // Bring up the capture backend now (we are on the render thread, GL/EGL is
    // current) and take the *actual* pixel byte order from it, so the ffmpeg
    // -pix_fmt always matches the bytes the backend produces.
    gdr::CaptureScheduler::get().prepare();
    m_inputBGRA = gdr::CaptureScheduler::get().isBGRA();
    m_borrowFps = st.borrowFpsFromGame;

    std::string dir=outDir(st.outputDir);std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(w2(dir)),ec);
    std::string base=dir+"\\"+tsN();
    m_outFile=base+".mp4";

    // Decide whether to capture audio. If so, the video records to a temp file
    // (-an) and is muxed with the WAV at finalize; otherwise ffmpeg writes the
    // final mp4 directly (no second pass).
    m_audioOn = st.audioDesktopEnabled || st.audioMicEnabled;
    m_audioBitrate = st.audioBitrateKbps>0 ? st.audioBitrateKbps : 192;
    if(m_audioOn){
        m_videoTmpFile = base+".video.mp4";
        m_audioTmpFile = base+".audio.wav";
    } else {
        m_videoTmpFile = m_outFile;     // ffmpeg writes straight to the final file
        m_audioTmpFile.clear();
    }

    m_bgraBytes=(size_t)m_capW*m_capH*4;
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        m_frameQueue.clear();
    }
    m_droppedFrames.store(0);m_startTs.store(0.0);m_writtenFrames.store(0);m_stopTargetFrames.store(0);m_saveProgress.store(0);
    if(!startFfmpeg())return false;

    // Arm audio capture. Mirror the mixer settings/device selection from the UI
    // state into the engine, then start writing the WAV. Audio failing never
    // aborts the recording — we just fall back to a video-only result at mux.
    if(m_audioOn){
        auto& ac=AudioCapture::get();
        ac.setDesktopDeviceId(w2(st.desktopDeviceId));
        ac.setMicDeviceId(w2(st.micDeviceId));
        ac.setDesktopEnabled(st.audioDesktopEnabled);
        ac.setMicEnabled(st.audioMicEnabled);
        ac.setDesktopVolume(st.audioDesktopVol);
        ac.setMicVolume(st.audioMicVol);
        ac.setDesktopMuted(st.audioDesktopMuted);
        ac.setMicMuted(st.audioMicMuted);
        if(!ac.start(w2(m_audioTmpFile))){
            geode::log::warn("GDSR: audio capture failed to start, recording video-only");
            m_audioOn=false; m_audioTmpFile.clear();
            // ffmpeg already targets m_videoTmpFile; finalize still needs to turn
            // that temp into m_outFile (it renames when no WAV exists).
        }
    }

    timeBeginPeriod(1);m_startTs.store(nowS());m_running.store(true);
    m_worker=std::thread(&Recorder::workerLoop,this);
    geode::log::info("GDSR: recording {}x{} {}fps input={} audio={}",m_capW,m_capH,m_fps,m_inputBGRA?"bgra":"rgba",m_audioOn);return true;
}

void Recorder::workerLoop(){
    for(;;){
        QueuedFrame frame;
        {
            std::unique_lock<std::mutex> lk(m_queueMutex);
            m_queueCv.wait(lk,[this](){return !m_running.load()||!m_frameQueue.empty();});
            if(m_frameQueue.empty()){
                if(!m_running.load())break;
                continue;
            }
            frame=std::move(m_frameQueue.front());
            m_frameQueue.pop_front();
        }
        // A queue slot just freed — wake a render thread blocked in submitFrame
        // (the "borrow fps" backpressure).
        m_spaceCv.notify_one();
        for(int i=0;i<frame.repeat;i++){
            if(!m_running.load())return;
            if(!writePipe(frame.data.data(),frame.data.size())){
                if(m_running.load())setLastError("ffmpeg pipe write failed");
                m_running.store(false);
                m_queueCv.notify_all();
                m_spaceCv.notify_all();
                return;
            }
            m_writtenFrames.fetch_add(1);
        }
    }
}

void Recorder::finalizeStop(){
    {
        std::lock_guard<std::mutex> lk(m_ffmpegMutex);
        if(m_ffmpegStdinWr){CloseHandle((HANDLE)m_ffmpegStdinWr);m_ffmpegStdinWr=nullptr;}
    }
    if(m_worker.joinable())m_worker.join();
    m_saveProgress.store(40);
    stopFfmpeg();
    AudioCapture::get().stop();

    if(m_videoTmpFile!=m_outFile){
        std::error_code ec;
        bool muxed=false;
        if(m_audioOn){
            m_saveProgress.store(60);
            m_saveProgress.store(70);
            muxed=runMux();
        }
        if(muxed){
            std::filesystem::remove(std::filesystem::path(w2(m_videoTmpFile)),ec);
            std::filesystem::remove(std::filesystem::path(w2(m_audioTmpFile)),ec);
        } else {
            if(m_audioOn) geode::log::warn("GDSR: mux failed ({}), saving video-only",lastError());
            std::filesystem::remove(std::filesystem::path(w2(m_outFile)),ec);
            std::filesystem::rename(std::filesystem::path(w2(m_videoTmpFile)),
                                    std::filesystem::path(w2(m_outFile)),ec);
            if(!m_audioTmpFile.empty())
                std::filesystem::remove(std::filesystem::path(w2(m_audioTmpFile)),ec);
        }
    }

    m_saveProgress.store(95);
    timeEndPeriod(1);m_capW=m_capH=0;m_finalizing.store(false);m_saveProgress.store(100);
}

void Recorder::stop(){
    if(!m_running.load()){
        if(m_worker.joinable())m_worker.join();
        if(m_finalizeThread.joinable())m_finalizeThread.join();
        return;
    }
    if(m_finalizeThread.joinable()) m_finalizeThread.join();
    m_saveProgress.store(5);
    m_running.store(false);
    m_finalizing.store(true);
    gdr::CaptureScheduler::get().endRecording();
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        m_frameQueue.clear();
    }
    m_queueCv.notify_all();
    m_spaceCv.notify_all();
    m_finalizeThread = std::thread(&Recorder::finalizeStop, this);
}

// Second pass: copy the recorded video stream and AAC-encode the captured WAV
// into the final mp4. Bounded so it can never hang the finalize thread forever.
bool Recorder::runMux(){
    if(m_videoTmpFile.empty()||m_audioTmpFile.empty()){setLastError("mux: missing temp");return false;}
    std::error_code ec;
    if(!std::filesystem::exists(std::filesystem::path(w2(m_videoTmpFile)),ec)){setLastError("mux: no video");return false;}
    bool haveWav=std::filesystem::exists(std::filesystem::path(w2(m_audioTmpFile)),ec);

    std::ostringstream cmd;
    cmd << q(ffPath()) << " -hide_banner -loglevel error -y "
        << "-i " << q(m_videoTmpFile) << ' ';
    if(haveWav) cmd << "-i " << q(m_audioTmpFile) << ' ';
    cmd << "-map 0:v:0 ";
    if(haveWav){
        cmd << "-map 1:a:0 -c:a aac -b:a " << m_audioBitrate << "k ";
    }
    // No "+faststart": for a local file moov-at-end is fine, and faststart would
    // add a full extra read+write of the whole muxed file — on a long recording
    // (slow HDD) that doubles mux time and risks the timeout below.
    cmd << "-c:v copy -shortest " << q(m_outFile);

    STARTUPINFOW si{};si.cb=sizeof(si);
    PROCESS_INFORMATION pi{};auto wc=w2(cmd.str());
    if(!CreateProcessW(nullptr,wc.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){
        setLastError("mux: CreateProcess failed");return false;
    }
    CloseHandle(pi.hThread);
    // Scale the wait with the recording length: copying+remuxing an 11-minute file
    // and AAC-encoding its audio can take well over a flat 60 s on a slow disk
    // (that flat ceiling is exactly what produced "mux: ffmpeg code 1"). Floor at
    // 2 min, allow ~4x realtime, cap at 30 min so a wedged process still gives up.
    double vidSecs = (double)m_writtenFrames.load() / (double)(m_fps>0?m_fps:60);
    DWORD muxTimeoutMs = (DWORD)std::min(30.0*60.0*1000.0, std::max(120000.0, vidSecs*4000.0));
    DWORD wait=WaitForSingleObject(pi.hProcess,muxTimeoutMs);
    DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);
    if(wait==WAIT_TIMEOUT){TerminateProcess(pi.hProcess,1);code=1;}
    CloseHandle(pi.hProcess);
    if(code!=0){setLastError("mux: ffmpeg code "+std::to_string(code));return false;}
    return true;
}
