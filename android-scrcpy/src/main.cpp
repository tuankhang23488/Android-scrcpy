#include <windows.h>
#include <windowsx.h>
#include <cstring>
#include <cstdio>
#include <commctrl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#pragma comment(lib,"comctl32.lib")
using Microsoft::WRL::ComPtr;

static HINSTANCE g_instance;
static HWND g_main{}, g_list{};
static std::wstring g_adb;
static constexpr UINT WM_FRAME = WM_APP + 1;
static constexpr UINT WM_ENDED = WM_APP + 2;
static constexpr UINT WM_STATUS = WM_APP + 3;
static constexpr int ID_REFRESH=100, ID_START=101, ID_STOP=102, ID_LIST=103;

struct Child {
    HANDLE process{}, read{}, thread{};
    void close() {
        if (read) { CloseHandle(read); read=nullptr; }
        if (process) { TerminateProcess(process, 0); WaitForSingleObject(process, 3000); CloseHandle(process); process=nullptr; }
        if (thread) { CloseHandle(thread); thread=nullptr; }
    }
};
static std::wstring q(const std::wstring& s) { return L"\""+s+L"\""; }
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);
    if (!n) return L"";
    std::wstring w(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),w.data(),n); return w;
}
static std::string toUtf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string v(n,0); if(n) WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),v.data(),n,nullptr,nullptr); return v;
}
static bool spawn(const std::wstring& args, Child& child) {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE r{},w{};
    if(!CreatePipe(&r,&w,&sa,0)) return false;
    SetHandleInformation(r,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    si.wShowWindow=SW_HIDE; si.hStdInput=GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput=w; si.hStdError=w;
    PROCESS_INFORMATION pi{};
    std::wstring cmd=q(g_adb)+L" "+args;
    BOOL ok=CreateProcessW(g_adb.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
    CloseHandle(w);
    if(!ok) { CloseHandle(r); return false; }
    child.process=pi.hProcess; child.thread=pi.hThread; child.read=r; return true;
}
static std::string run(const std::wstring& args) {
    Child c; if(!spawn(args,c)) return {};
    std::string output; char b[4096]; DWORD n=0;
    while(ReadFile(c.read,b,sizeof b,&n,nullptr)&&n && output.size()<1024*1024) output.append(b,n);
    CloseHandle(c.read); c.read=nullptr;
    WaitForSingleObject(c.process,5000);
    CloseHandle(c.process); c.process=nullptr;
    CloseHandle(c.thread); c.thread=nullptr;
    return output;
}
static bool validSerial(const std::string& s) {
    return !s.empty() && s.size()<256 && std::all_of(s.begin(),s.end(),[](unsigned char c){return std::isalnum(c)||c=='.'||c=='_'||c=='-'||c==':';});
}
static void adbInput(const std::string& serial, const std::wstring& cmd) {
    std::thread([serial,cmd]{ run(L"-s "+q(toWide(serial))+L" shell input "+cmd); }).detach();
}
struct Frame { std::vector<uint8_t> bgra; int width{},height{}; };
struct Session {
    std::string serial;
    HWND window{};
    std::atomic<bool> stopped{false};
    std::thread worker;
    std::mutex lock;
    Frame frame;
    int deviceW=1080, deviceH=1920; // actual Android input coordinates (not encoded frame size)
    Child child;
    std::mutex childLock;
    std::atomic<bool> framePending{false};
    bool dragging=false;
    POINT down{};
};
static std::mutex g_lock;
static std::vector<std::shared_ptr<Session>> g_sessions;

// H264 Annex-B NAL streaming parser; 4 or 3 byte start codes.
class Nals {
    std::vector<uint8_t> buf;
    static size_t start(const std::vector<uint8_t>& b,size_t from) {
        for(size_t i=from;i+3<b.size();++i)
            if(b[i]==0 && b[i+1]==0 && (b[i+2]==1 || (b[i+2]==0 && b[i+3]==1))) return i;
        return b.size();
    }
public:
    template<class F> void feed(const uint8_t* data,size_t n,F fn) {
        buf.insert(buf.end(),data,data+n);
        for(;;) {
            size_t a=start(buf,0);
            if(a==buf.size()) { if(buf.size()>4*1024*1024) buf.clear(); return; }
            if(a) buf.erase(buf.begin(),buf.begin()+a);
            size_t code=buf[2]==1?3:4;
            size_t b=start(buf,code);
            if(b==buf.size()) return;
            if(b>code) fn(std::vector<uint8_t>(buf.begin(),buf.begin()+b),code);
            buf.erase(buf.begin(),buf.begin()+b);
        }
    }
};
static bool firstSlice(const std::vector<uint8_t>& nal,size_t prefix) {
    // first_mb_in_slice is Exp-Golomb at beginning of RBSP after NAL header.
    size_t pos=prefix+1; int zeros=0;
    while(pos<nal.size() && zeros<16) {
        uint8_t b=nal[pos++];
        for(int k=7;k>=0;--k) {
            if((b>>k)&1) return zeros==0; // UE(v)=0 iff the first bit is 1.
            ++zeros;
        }
    }
    return true;
}
class Decoder {
    ComPtr<IMFTransform> mft;
    int w=0,h=0;
    LONGLONG pts=0;
    bool ready=false;
    bool configureOutput() {
        for(DWORD i=0;i<32;++i) {
            ComPtr<IMFMediaType> t;
            if(FAILED(mft->GetOutputAvailableType(0,i,&t))) break;
            GUID subtype{}; t->GetGUID(MF_MT_SUBTYPE,&subtype);
            if(subtype!=MFVideoFormat_NV12) continue;
            if(SUCCEEDED(mft->SetOutputType(0,t.Get(),0))) {
                UINT32 x=0,y=0;
                MFGetAttributeSize(t.Get(),MF_MT_FRAME_SIZE,&x,&y);
                if(x&&y) {w=(int)x;h=(int)y;}
                return true;
            }
        }
        return false;
    }
    static uint8_t clamp(int x) {return (uint8_t)std::clamp(x,0,255);}
    void frameFrom(IMFSample* s, Frame& out) {
        ComPtr<IMFMediaBuffer> b;
        if(FAILED(s->ConvertToContiguousBuffer(&b))) return;
        BYTE* ptr=nullptr; DWORD len=0,maxlen=0;
        if(FAILED(b->Lock(&ptr,&maxlen,&len))) return;
        int W=w,H=h;
        if(W>0 && H>0 && W<=4096 && H<=4096 && len>=static_cast<DWORD>(W*H*3/2)) {
            out.width=W;out.height=H;out.bgra.resize((size_t)W*H*4);
            const uint8_t* y=ptr; const uint8_t* uv=ptr+W*H;
            for(int j=0;j<H;++j) for(int i=0;i<W;++i) {
                int Y=std::max(0,(int)y[(size_t)j*W+i]-16)*298;
                size_t uvpos=(size_t)(j/2)*W+(i&~1);
                int U=(int)uv[uvpos]-128,V=(int)uv[uvpos+1]-128;
                size_t p=((size_t)j*W+i)*4;
                out.bgra[p]=clamp((Y+516*U+128)>>8);
                out.bgra[p+1]=clamp((Y-100*U-208*V+128)>>8);
                out.bgra[p+2]=clamp((Y+409*V+128)>>8);
                out.bgra[p+3]=255;
            }
        }
        b->Unlock();
    }
public:
    bool init() {
        // Discover a Windows H.264 decoder at runtime instead of linking to
        // CLSID_CMSH264DecoderMFT (which needs an extra Windows SDK UUID library).
        MFT_REGISTER_TYPE_INFO inputType{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType, nullptr, &activations, &count);
        if (FAILED(hr) || count == 0) {
            if (activations) CoTaskMemFree(activations);
            return false;
        }
        for (UINT32 i = 0; i < count; ++i) {
            if (!mft) activations[i]->ActivateObject(IID_PPV_ARGS(&mft));
            activations[i]->Release();
        }
        CoTaskMemFree(activations);
        if (!mft) return false;
        ComPtr<IMFMediaType> t;
        if(FAILED(MFCreateMediaType(&t))) return false;
        t->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264);
        MFSetAttributeSize(t.Get(),MF_MT_FRAME_SIZE,540,960);
        MFSetAttributeRatio(t.Get(),MF_MT_FRAME_RATE,30,1);
        t->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
        if(FAILED(mft->SetInputType(0,t.Get(),0))) return false;
        configureOutput();
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0);
        ready=true; return true;
    }
    void decode(const std::vector<uint8_t>& au, Frame& last) {
        if(!ready || au.empty()) return;
        ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer;
        if(FAILED(MFCreateSample(&sample))||FAILED(MFCreateMemoryBuffer((DWORD)au.size(),&buffer))) return;
        BYTE* dst=nullptr; DWORD cap=0,len=0;
        if(FAILED(buffer->Lock(&dst,&cap,&len))) return;
        memcpy(dst,au.data(),au.size());buffer->Unlock();buffer->SetCurrentLength((DWORD)au.size());
        sample->AddBuffer(buffer.Get());sample->SetSampleTime(pts);sample->SetSampleDuration(333333);pts+=333333;
        HRESULT hr=mft->ProcessInput(0,sample.Get(),0);
        if(FAILED(hr)) return;
        for(int attempts=0;attempts<8;++attempts) {
            MFT_OUTPUT_STREAM_INFO info{};
            if(FAILED(mft->GetOutputStreamInfo(0,&info))) break;
            ComPtr<IMFSample> outSample;
            if(!(info.dwFlags&MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                if(FAILED(MFCreateSample(&outSample))) break;
                ComPtr<IMFMediaBuffer> outBuffer;
                DWORD capacity=std::max<DWORD>(info.cbSize,(DWORD)std::max(1,w*h*3/2));
                if(FAILED(MFCreateMemoryBuffer(capacity,&outBuffer))) break;
                outSample->AddBuffer(outBuffer.Get());
            }
            MFT_OUTPUT_DATA_BUFFER output{}; output.dwStreamID=0;output.pSample=outSample.Get();
            DWORD status=0;
            hr=mft->ProcessOutput(0,1,&output,&status);
            if(output.pEvents) output.pEvents->Release();
            if(hr==MF_E_TRANSFORM_STREAM_CHANGE) {configureOutput();continue;}
            if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if(FAILED(hr)) break;
            IMFSample* actual=output.pSample?output.pSample:outSample.Get();
            if(actual) frameFrom(actual,last);
            if(output.pSample && output.pSample!=outSample.Get()) output.pSample->Release();
        }
    }
};
static void postFrame(std::shared_ptr<Session> s, Frame& output) {
    if(output.bgra.empty()) return;
    {
        std::lock_guard<std::mutex> l(s->lock);
        s->frame=std::move(output);
        // Keep input coordinates separate from the resized video frame.
    }
    if(IsWindow(s->window) && !s->framePending.exchange(true)) PostMessage(s->window,WM_FRAME,0,0);
}
static void stream(std::shared_ptr<Session> s) {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    // screenrecord --size changes video pixels, not Android input coordinates.
    // Query the native input display resolution once per session.
    {
        auto dimensions = run(L"-s " + q(toWide(s->serial)) + L" shell wm size");
        auto x = dimensions.rfind("size: ");
        if (x != std::string::npos) {
            int dw=0, dh=0;
            if (sscanf_s(dimensions.c_str()+x, "size: %dx%d", &dw, &dh)==2 && dw>0 && dh>0) {
                s->deviceW=dw; s->deviceH=dh;
            }
        }
    }
    MFStartup(MF_VERSION);
    while(!s->stopped) {
        Decoder decoder;
        if(!decoder.init()) {PostMessage(s->window,WM_STATUS,0,0);break;}
        Child child;
        std::wstring args=L"-s "+q(toWide(s->serial))+L" exec-out screenrecord --output-format=h264 --size 540x960 --bit-rate 1500000 -";
        if(!spawn(args,child)) {PostMessage(s->window,WM_STATUS,1,0);break;}
        {
            std::lock_guard<std::mutex> l(s->childLock);
            s->child.process=child.process;s->child.read=child.read;s->child.thread=child.thread;
        }
        Nals parser; std::vector<uint8_t> au; bool hasVcl=false;
        auto receive=[&](std::vector<uint8_t> nal,size_t prefix) {
            int type=(nal[prefix]&31);bool vcl=(type==1||type==5);
            // A new first slice ends the previous access unit.
            if((vcl&&firstSlice(nal,prefix)&&hasVcl) || (type==9&&hasVcl)) {
                Frame f;decoder.decode(au,f);postFrame(s,f);au.clear();hasVcl=false;
            }
            if(type>=1&&type<=12) {
                if(au.size()+nal.size()>4*1024*1024) {au.clear();hasVcl=false;}
                au.insert(au.end(),nal.begin(),nal.end());
                if(vcl) hasVcl=true;
            }
        };
        uint8_t temp[65536];DWORD n=0;
        while(!s->stopped) {
            HANDLE pipe;
            {std::lock_guard<std::mutex> l(s->childLock);pipe=s->child.read;}
            if(!pipe || !ReadFile(pipe,temp,sizeof temp,&n,nullptr)||n==0) break;
            parser.feed(temp,n,receive);
        }
        {std::lock_guard<std::mutex> l(s->childLock);s->child.close();}
        if(!s->stopped) Sleep(500); // screenrecord ends after ~180s: auto-reconnect.
    }
    MFShutdown();CoUninitialize();
    if(IsWindow(s->window)) PostMessage(s->window,WM_ENDED,0,0);
}
static RECT fitRect(HWND hwnd,int W,int H) {
    RECT rc{};GetClientRect(hwnd,&rc);
    if(W<=0||H<=0) return rc;
    int cw=rc.right,ch=rc.bottom;
    int dw=cw,dh=MulDiv(cw,H,W);
    if(dh>ch) {dh=ch;dw=MulDiv(ch,W,H);}
    return RECT{(cw-dw)/2,(ch-dh)/2,(cw+dw)/2,(ch+dh)/2};
}
static void tapPoint(const std::shared_ptr<Session>& s,POINT pt,bool swipe) {
    int videoW=540, videoH=960;
    { std::lock_guard<std::mutex> guard(s->lock); if(s->frame.width>0 && s->frame.height>0) {videoW=s->frame.width;videoH=s->frame.height;} }
    RECT f=fitRect(s->window,videoW,videoH);
    if(f.right<=f.left||f.bottom<=f.top) return;
    auto translate=[&](POINT p){
        POINT t{};
        t.x=std::clamp(MulDiv(p.x-f.left,s->deviceW,f.right-f.left),0,s->deviceW-1);
        t.y=std::clamp(MulDiv(p.y-f.top,s->deviceH,f.bottom-f.top),0,s->deviceH-1);
        return t;
    };
    POINT a=translate(s->down),b=translate(pt);
    if(swipe) adbInput(s->serial,L"swipe "+std::to_wstring(a.x)+L" "+std::to_wstring(a.y)+L" "+std::to_wstring(b.x)+L" "+std::to_wstring(b.y)+L" 250");
    else adbInput(s->serial,L"tap "+std::to_wstring(b.x)+L" "+std::to_wstring(b.y));
}
static LRESULT CALLBACK viewerProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto *raw=(Session*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
    std::shared_ptr<Session> s;
    if(raw) {std::lock_guard<std::mutex> l(g_lock);for(auto &x:g_sessions) if(x.get()==raw){s=x;break;}}
    switch(msg) {
    case WM_CREATE: SetWindowLongPtr(hwnd,GWLP_USERDATA,(LONG_PTR)((CREATESTRUCT*)lp)->lpCreateParams);return 0;
    case WM_FRAME: if(s) s->framePending=false; InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_STATUS: SetWindowText(hwnd,wp?L"Khong chay duoc adb":L"Khong khoi tao duoc H264 decoder");return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);RECT rc{};GetClientRect(hwnd,&rc);
        FillRect(dc,&rc,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if(s) {
            std::lock_guard<std::mutex> l(s->lock);
            auto &f=s->frame;
            if(!f.bgra.empty()) {
                RECT r=fitRect(hwnd,f.width,f.height);
                BITMAPINFO bmi{};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bmi.bmiHeader.biWidth=f.width;
                bmi.bmiHeader.biHeight=-f.height;bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;
                bmi.bmiHeader.biCompression=BI_RGB;
                SetStretchBltMode(dc,COLORONCOLOR); // faster than CPU-intensive HALFTONE
                StretchDIBits(dc,r.left,r.top,r.right-r.left,r.bottom-r.top,0,0,f.width,f.height,f.bgra.data(),&bmi,DIB_RGB_COLORS,SRCCOPY);
            }
        }
        EndPaint(hwnd,&ps);return 0;
    }
    case WM_LBUTTONDOWN:if(s){s->dragging=true;s->down={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};SetCapture(hwnd);}return 0;
    case WM_LBUTTONUP:if(s&&s->dragging){POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ReleaseCapture();s->dragging=false;tapPoint(s,p,abs(p.x-s->down.x)+abs(p.y-s->down.y)>12);}return 0;
    case WM_KEYDOWN:if(s){int key=0;switch(wp){case VK_ESCAPE:key=4;break;case VK_HOME:key=3;break;case VK_BACK:key=67;break;case VK_RETURN:key=66;break;case VK_UP:key=19;break;case VK_DOWN:key=20;break;case VK_LEFT:key=21;break;case VK_RIGHT:key=22;break;}if(key)adbInput(s->serial,L"keyevent "+std::to_wstring(key));}return 0;
    case WM_CLOSE: if(s) {s->stopped=true;{std::lock_guard<std::mutex> l(s->childLock);if(s->child.process) TerminateProcess(s->child.process,0);}}DestroyWindow(hwnd);return 0;
    case WM_DESTROY:if(s){s->window=nullptr;}return 0;
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}
static std::string selectedSerial() {
    int i=ListView_GetNextItem(g_list,-1,LVNI_SELECTED);
    if(i<0) return {};
    wchar_t b[256]{};ListView_GetItemText(g_list,i,0,b,256);
    return toUtf8(b);
}
static void refresh() {
    ListView_DeleteAllItems(g_list);
    std::string output=run(L"devices -l"); std::istringstream lines(output);std::string line;
    while(std::getline(lines,line)) {
        if(line.find("List of devices")!=std::string::npos || line.empty()) continue;
        std::istringstream parts(line);std::string serial,state;parts>>serial>>state;
        if(!validSerial(serial)||state.empty()) continue;
        std::wstring ws=toWide(serial),status=toWide(state);
        LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=ListView_GetItemCount(g_list);item.pszText=ws.data();
        int idx=ListView_InsertItem(g_list,&item);
        ListView_SetItemText(g_list,idx,1,status.data());
        size_t pos=line.find("model:");
        if(pos!=std::string::npos) {
            auto model=line.substr(pos+6);auto space=model.find(' ');if(space!=std::string::npos)model.resize(space);
            std::wstring wmodel=toWide(model);ListView_SetItemText(g_list,idx,2,wmodel.data());
        }
    }
}
static void start() {
    std::string serial=selectedSerial();if(!validSerial(serial)) return;
    {
        std::lock_guard<std::mutex> l(g_lock);
        for(auto &s:g_sessions) if(s->serial==serial&&!s->stopped) {if(s->window){ShowWindow(s->window,SW_SHOW);SetForegroundWindow(s->window);}return;}
    }
    auto s=std::make_shared<Session>();s->serial=serial;
    std::wstring title=L"Android - "+toWide(serial)+L" | ESC=Back, Home=Home";
    HWND w=CreateWindowExW(0,L"ANDROID_VIEW",title.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,420,760,g_main,nullptr,g_instance,s.get());
    if(!w)return;s->window=w;
    {std::lock_guard<std::mutex> l(g_lock);g_sessions.push_back(s);}
    s->worker=std::thread(stream,s);
}
static void stop() {
    std::string serial=selectedSerial();
    std::shared_ptr<Session> found;
    {std::lock_guard<std::mutex> l(g_lock);for(auto &s:g_sessions) if(s->serial==serial&&!s->stopped){found=s;break;}}
    if(found&&found->window) SendMessage(found->window,WM_CLOSE,0,0);
}
static LRESULT CALLBACK mainProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE,10,10,100,32,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)),g_instance,nullptr);
        CreateWindowW(L"BUTTON",L"Start",WS_CHILD|WS_VISIBLE,120,10,100,32,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_START)),g_instance,nullptr);
        CreateWindowW(L"BUTTON",L"Stop",WS_CHILD|WS_VISIBLE,230,10,100,32,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STOP)),g_instance,nullptr);
        g_list=CreateWindowW(WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|WS_BORDER,
              10,52,770,490,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)),g_instance,nullptr);
        ListView_SetExtendedListViewStyle(g_list,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES);
        LVCOLUMNW col{};col.mask=LVCF_WIDTH|LVCF_TEXT;
        col.cx=240;col.pszText=(LPWSTR)L"Serial";ListView_InsertColumn(g_list,0,&col);
        col.cx=140;col.pszText=(LPWSTR)L"Status";ListView_InsertColumn(g_list,1,&col);
        col.cx=320;col.pszText=(LPWSTR)L"Model";ListView_InsertColumn(g_list,2,&col);
        return 0;
    }
    case WM_SIZE:if(g_list)MoveWindow(g_list,10,52,std::max(100,(int)LOWORD(lp)-20),std::max(50,(int)HIWORD(lp)-62),TRUE);return 0;
    case WM_COMMAND:switch(LOWORD(wp)){case ID_REFRESH:refresh();break;case ID_START:start();break;case ID_STOP:stop();break;}return 0;
    case WM_NOTIFY:if(((NMHDR*)lp)->idFrom==ID_LIST&&((NMHDR*)lp)->code==NM_DBLCLK)start();return 0;
    case WM_DESTROY: {
        std::vector<std::shared_ptr<Session>> copy;
        {std::lock_guard<std::mutex> l(g_lock);copy=g_sessions;}
        for(auto&s:copy){s->stopped=true;std::lock_guard<std::mutex> l(s->childLock);if(s->child.process)TerminateProcess(s->child.process,0);}
        for(auto&s:copy)if(s->worker.joinable())s->worker.join();
        PostQuitMessage(0);return 0;
    }
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int show) {
    g_instance=hi;wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
    std::wstring folder=path;folder=folder.substr(0,folder.find_last_of(L"\\/"));
    g_adb=folder+L"\\platform-tools\\adb.exe";
    if(GetFileAttributesW(g_adb.c_str())==INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr,L"Missing platform-tools\\adb.exe. Extract official Android SDK Platform-Tools to platform-tools next to the EXE.",L"android-scrcpy",MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof controls,ICC_LISTVIEW_CLASSES};InitCommonControlsEx(&controls);
    WNDCLASSW a{};a.hInstance=hi;a.lpfnWndProc=mainProc;a.lpszClassName=L"ANDROID_MAIN";a.hCursor=LoadCursor(nullptr,IDC_ARROW);a.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);RegisterClassW(&a);
    WNDCLASSW b{};b.hInstance=hi;b.lpfnWndProc=viewerProc;b.lpszClassName=L"ANDROID_VIEW";b.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&b);
    g_main=CreateWindowW(L"ANDROID_MAIN",L"android-scrcpy | C++ | H264 / Media Foundation",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,820,610,nullptr,nullptr,hi,nullptr);
    ShowWindow(g_main,show);UpdateWindow(g_main);refresh();
    MSG m{};while(GetMessageW(&m,nullptr,0,0)>0){TranslateMessage(&m);DispatchMessageW(&m);}return (int)m.wParam;
}
