#include "platform/windows/win_video_presenter.hpp"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wrl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace reaweb {
namespace {
using Microsoft::WRL::ComPtr;
void require(HRESULT hr, const char* message) {
  if (FAILED(hr)) throw Error("VIDEO_RENDERER_UNAVAILABLE", std::string(message) + " (" + std::to_string(static_cast<unsigned long>(hr)) + ")");
}
const char* shaders = R"HLSL(
cbuffer Settings : register(b0) { float4 outputSize; float4 sourceRect; };
Texture2D<float> luma : register(t0);
Texture2D<float> chromaU : register(t1);
Texture2D<float> chromaV : register(t2);
SamplerState linearClamp : register(s0);
float4 presentI420(float4 at:SV_Position):SV_Target {
 uint w,h;luma.GetDimensions(w,h);
 float2 scale=sourceRect.zw/outputSize.xy;
 float2 pointInSource=sourceRect.xy+at.xy*scale;
 if(any(pointInSource<0)||any(pointInSource>=float2(w,h)))return float4(0,0,0,1);
 // Match REAPER's OpenGL viewport: linear YUV sampling before RGB conversion.
 float2 uv=pointInSource/float2(w,h);
 float y=(luma.SampleLevel(linearClamp,uv,0)*255.0-16.0)/219.0;
 float u=(chromaU.SampleLevel(linearClamp,uv,0)*255.0-128.0)/224.0;
 float v=(chromaV.SampleLevel(linearClamp,uv,0)*255.0-128.0)/224.0;
 return float4(saturate(y+u*float3(0,-0.187324273,1.8556)+v*float3(1.5748,-0.468124273,0)),1);
}
float4 videoVertex(uint index:SV_VertexID):SV_Position {
 return float4(index==2?3:-1,index==1?3:-1,0,1);
}
Texture2D<float4> rgb : register(t0);
float4 presentRGB(float4 at:SV_Position):SV_Target {
 uint w,h;rgb.GetDimensions(w,h);
 float2 scale=sourceRect.zw/outputSize.xy;
 float2 samplePoint=sourceRect.xy+at.xy*scale;
 if(any(samplePoint<0)||any(samplePoint>=float2(w,h)))return float4(0,0,0,1);
 float2 r=max(float2(1,1),min(scale,float2(16,16)));
 float2 lo=samplePoint-r*0.5,hi=samplePoint+r*0.5;
 int2 i0=int2(floor(lo)),i1=int2(ceil(hi))-1,limit=int2(w,h)-1;
 int ny=min(i1.y-i0.y+1,17),nx=min(i1.x-i0.x+1,17);
 float3 sum=0;
 [loop] for(int dy=0;dy<ny;++dy){
  int y=i0.y+dy;
  float wy=min(float(y+1),hi.y)-max(float(y),lo.y);
  int cy=clamp(y,0,limit.y);
  [loop] for(int dx=0;dx<nx;++dx){
   int x=i0.x+dx;
   float wx=min(float(x+1),hi.x)-max(float(x),lo.x);
   sum+=wx*wy*rgb.Load(int3(clamp(x,0,limit.x),cy,0)).rgb;
  }
 }
 return float4(sum/(r.x*r.y),1);
}
)HLSL";

class CompositionHost final : public WinVideoHost {
public:
  ComPtr<IDCompositionDevice> composition;
  ComPtr<IDCompositionTarget> target;
  uint64_t generation = 0;
  const DWORD thread = GetCurrentThreadId();
  explicit CompositionHost(HWND window) {
    require(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&composition)), "Cannot create video composition device");
    require(composition->CreateTargetForHwnd(window, FALSE, &target), "Cannot create video composition target");
  }
  ~CompositionHost() override { target->SetRoot(nullptr); composition->Commit(); }
  uint64_t attach(IDCompositionVisual* visual) {
    require(target->SetRoot(visual), "Cannot attach video visual");
    require(composition->Commit(), "Cannot commit video visual");
    return ++generation;
  }
  void detach(uint64_t value) noexcept {
    if (value != generation) return;
    ++generation; target->SetRoot(nullptr); composition->Commit();
  }
};
struct Layout {
  int width = 0, height = 0;
  float x = 0, y = 0;
  std::array<float, 4> uv{0, 0, 1, 1};
  double content_width = 0, content_height = 0;
  bool aspect = true, visible = false;
};
struct Surface {
  std::atomic<bool> invalidated{false}, failed{false}, redraw{true};
  std::mutex mutex;
  Layout layout;
  ComPtr<IDCompositionDevice> composition;
  ComPtr<IDCompositionVisual> visual;
  bool has_frame = false, statistics_available = false;
  uint64_t submitted = 0, presented = 0, dropped = 0, sequence = 0;
  double first_presented = 0, last_presented = 0, max_present_ms = 0;
  std::string error;
  // The following resources are owned exclusively by the source worker.
  ComPtr<IDXGISwapChain1> chain;
  ComPtr<IDXGISwapChainMedia> media;
  HANDLE frame_ready = nullptr;
  bool frame_permitted = false;
  bool content_attached = false;
  bool content_changed = false;
  float visual_x = 0, visual_y = 0;
  int width = 0, height = 0;
  UINT observed_present = 0;
  uint64_t submitted_sequence = 0, observed_sequence = 0, epoch = 0;
  uint64_t retry_sequence = 0, retry_epoch = 0;
  struct Pending { UINT id; uint64_t sequence, epoch; bool clear; };
  std::deque<Pending> pending;
};
struct Slot {
  // 0 free, 1 being copied, 2 queued, 3 retained current, 4 GPU retirement.
  std::atomic<unsigned> status{0};
  std::vector<unsigned char> bytes;
  uint32_t width = 0, height = 0, texture_width = 0, texture_height = 0;
  uint64_t sequence = 0, epoch = 0;
  bool clear = false;
  bool conversion_counted = false;
  std::array<ComPtr<ID3D11Texture2D>, 3> textures;
  std::array<ComPtr<ID3D11ShaderResourceView>, 3> views;
  ComPtr<ID3D11Query> completion;
};
struct Worker {
  const int width, height;
  const bool i420;
  std::atomic<uint64_t> dimensions;
  std::array<Slot, 3> slots;
  std::atomic<bool> stopping{false}, failed{false};
  std::atomic<uint64_t> epoch{1}, dropped{0}, uploaded{0}, converted{0};
  std::mutex surfaces_mutex, wake_mutex;
  std::condition_variable wake;
  std::vector<std::shared_ptr<Surface>> surfaces;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGIFactory2> factory;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11VertexShader> vertex;
  ComPtr<ID3D11PixelShader> pixel;
  ComPtr<ID3D11Buffer> constants;
  Slot* current = nullptr;
  uint64_t current_epoch = 1;
  double qpc_frequency = 1;
  Worker(int w, int h, bool yuv) : width(w), height(h), i420(yuv), dimensions((uint64_t(w) << 32) | uint32_t(h)) {}
  std::vector<std::shared_ptr<Surface>> attached() {
    std::lock_guard<std::mutex> guard(surfaces_mutex);
    return surfaces;
  }
  void fail(const std::string& message) {
    failed = true;
    for (auto& state : attached()) {
      { std::lock_guard<std::mutex> guard(state->mutex); state->error = message; state->has_frame = false; }
      state->failed = true;
    }
  }
  ComPtr<ID3DBlob> compile(const char* entry, const char* profile) {
    static std::mutex cache_mutex;
    static std::map<std::string, ComPtr<ID3DBlob>> cache;
    const std::string key = std::string(entry) + ":" + profile;
    {
      std::lock_guard<std::mutex> guard(cache_mutex);
      const auto found = cache.find(key);
      if (found != cache.end()) return found->second;
    }
    ComPtr<ID3DBlob> code, error;
    const HRESULT result = D3DCompile(shaders, std::strlen(shaders), nullptr, nullptr, nullptr, entry, profile,
      D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(result)) throw Error("VIDEO_RENDERER_UNAVAILABLE", error ?
      std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()) : "Cannot compile video shader");
    std::lock_guard<std::mutex> guard(cache_mutex);
    cache[key] = code;
    return code;
  }
  void initialize() {
    require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context), "Cannot create video D3D device");
    ComPtr<IDXGIDevice> dxgi; require(device.As(&dxgi), "Cannot query video DXGI device");
    ComPtr<IDXGIAdapter> adapter; require(dxgi->GetAdapter(&adapter), "Cannot query video adapter");
    require(adapter->GetParent(IID_PPV_ARGS(&factory)), "Cannot query video factory");
    auto vs = compile("videoVertex", "vs_5_0"), ps = compile(i420 ? "presentI420" : "presentRGB", "ps_5_0");
    require(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex), "Cannot create video vertex shader");
    require(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel), "Cannot create video pixel shader");
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    require(device->CreateSamplerState(&desc, &sampler), "Cannot create video sampler");
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 32; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    require(device->CreateBuffer(&cb, nullptr, &constants), "Cannot create video constants");
    for (auto& slot : slots) {
      D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0}; require(device->CreateQuery(&query, &slot.completion), "Cannot create video completion query");
    }
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency); qpc_frequency = double(frequency.QuadPart);
  }
  void statistics(Surface& state) {
    if (!state.chain) return;
    UINT id = 0; LONGLONG time = 0; HRESULT hr;
    if (state.media) { DXGI_FRAME_STATISTICS_MEDIA value{}; hr = state.media->GetFrameStatisticsMedia(&value); id = value.PresentCount; time = value.SyncQPCTime.QuadPart; }
    else { DXGI_FRAME_STATISTICS value{}; hr = state.chain->GetFrameStatistics(&value); id = value.PresentCount; time = value.SyncQPCTime.QuadPart; }
    { std::lock_guard<std::mutex> guard(state.mutex); state.statistics_available = SUCCEEDED(hr); }
    if (FAILED(hr) || id == state.observed_present) return;
    state.observed_present = id;
    const auto found = std::find_if(state.pending.begin(), state.pending.end(), [id](const Surface::Pending& item) { return item.id == id; });
    if (found == state.pending.end()) return;
    if (found->epoch == epoch && found->sequence != state.observed_sequence) {
      state.observed_sequence = found->sequence;
      std::lock_guard<std::mutex> guard(state.mutex);
      if (!found->clear) { ++state.presented; if (!state.first_presented) state.first_presented = time/qpc_frequency; state.last_presented = time/qpc_frequency; }
    }
    state.pending.erase(state.pending.begin(), std::next(found));
  }
  void release_surface(Surface& state, bool detach = true) {
    if (detach) {
      state.visual->SetContent(nullptr); state.composition->Commit();
      state.content_attached = false;
    }
    if (state.frame_ready) { CloseHandle(state.frame_ready); state.frame_ready = nullptr; }
    state.frame_permitted = false;
    state.pending.clear(); state.media.Reset(); state.chain.Reset();
  }
  bool draw(Surface& state, Slot& slot, bool fresh) {
    if (state.invalidated || state.failed || slot.epoch != epoch) return false;
    Layout layout; bool redraw;
    { std::lock_guard<std::mutex> guard(state.mutex); layout = state.layout; redraw = state.redraw.exchange(false); }
    if (!layout.visible || !layout.width || !layout.height) {
      state.retry_sequence = 0;
      if (state.content_attached) { state.visual->SetContent(nullptr); state.composition->Commit(); state.content_attached = false; }
      return false;
    }
    if (!fresh && !redraw) return false;
    if (state.retry_sequence && (state.retry_sequence != slot.sequence || state.retry_epoch != slot.epoch)) {
      if (state.retry_epoch == slot.epoch) { std::lock_guard<std::mutex> guard(state.mutex); ++state.dropped; }
      state.retry_sequence = 0;
    }
    const bool resized = state.width != layout.width || state.height != layout.height;
    if (!state.chain || resized) {
      // Keep the displayed swapchain attached until its replacement has a frame.
      release_surface(state, false);
      DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = layout.width; desc.Height = layout.height; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 3;
      desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      require(factory->CreateSwapChainForComposition(device.Get(), &desc, nullptr, &state.chain), "Cannot create video viewport swapchain");
      ComPtr<IDXGISwapChain2> paced;
      require(state.chain.As(&paced), "Cannot configure video viewport latency");
      require(paced->SetMaximumFrameLatency(2), "Cannot bound video viewport latency");
      state.frame_ready = paced->GetFrameLatencyWaitableObject();
      if (!state.frame_ready) throw Error("VIDEO_RENDERER_UNAVAILABLE", "Cannot monitor video viewport availability");
      state.chain.As(&state.media); state.observed_present = 0; state.width = layout.width; state.height = layout.height;
      state.content_changed = true;
    }
    if (!state.frame_permitted) {
      const auto ready = WaitForSingleObject(state.frame_ready, 0);
      if (ready == WAIT_TIMEOUT) {
        if (!slot.clear && state.submitted_sequence != slot.sequence) { state.retry_sequence = slot.sequence; state.retry_epoch = slot.epoch; }
        state.redraw = true;
        return false;
      }
      if (ready != WAIT_OBJECT_0) throw Error("VIDEO_RENDERER_UNAVAILABLE", "Cannot poll video viewport availability");
      state.frame_permitted = true;
    }
    ComPtr<ID3D11Texture2D> back; require(state.chain->GetBuffer(0, IID_PPV_ARGS(&back)), "Cannot get video viewport buffer");
    ComPtr<ID3D11RenderTargetView> target; require(device->CreateRenderTargetView(back.Get(), nullptr, &target), "Cannot create video viewport target");
    D3D11_VIEWPORT viewport{0, 0, float(layout.width), float(layout.height), 0, 1}; context->RSSetViewports(1, &viewport);
    auto uv = layout.uv;
    if (layout.aspect && layout.content_width > 0 && layout.content_height > 0) {
      const double fit = (std::min)(layout.content_width / slot.width, layout.content_height / slot.height);
      const double fw = fit * slot.width / layout.content_width, fh = fit * slot.height / layout.content_height;
      uv = {float((uv[0] - (1 - fw) / 2) / fw), float((uv[1] - (1 - fh) / 2) / fh), float(uv[2] / fw), float(uv[3] / fh)};
    }
    float values[8]={float(layout.width),float(layout.height),0,0,uv[0]*slot.width,uv[1]*slot.height,uv[2]*slot.width,uv[3]*slot.height};
    context->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0); auto* cb=constants.Get(); context->PSSetConstantBuffers(0,1,&cb);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); context->VSSetShader(vertex.Get(),nullptr,0); context->PSSetShader(pixel.Get(),nullptr,0);
    ID3D11ShaderResourceView* views[] = {slot.views[0].Get(), slot.views[1].Get(), slot.views[2].Get()};
    const UINT plane_count = i420 ? 3 : 1;
    context->PSSetShaderResources(0, plane_count, views);
    auto* filter = sampler.Get(); context->PSSetSamplers(0, 1, &filter);
    auto* render_target = target.Get(); context->OMSetRenderTargets(1, &render_target, nullptr);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* empty[3]{};
    context->PSSetShaderResources(0, plane_count, empty); context->OMSetRenderTargets(0, nullptr, nullptr);
    if (i420 && !slot.conversion_counted) { slot.conversion_counted = true; ++converted; }
    if (state.invalidated || slot.epoch != epoch) return true;
    const auto started=std::chrono::steady_clock::now();
    // A replacement must have a front buffer before the visual switches to it.
    const HRESULT result=state.chain->Present(state.content_changed ? 0 : 1,DXGI_PRESENT_DO_NOT_WAIT);
    {std::lock_guard<std::mutex> guard(state.mutex);state.max_present_ms=(std::max)(state.max_present_ms,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());}
    if(result==DXGI_ERROR_WAS_STILL_DRAWING){if(!slot.clear&&state.submitted_sequence!=slot.sequence){state.retry_sequence=slot.sequence;state.retry_epoch=slot.epoch;}state.redraw=true;return true;}
    require(result,"Cannot present video viewport");
    state.frame_permitted=false;
    state.retry_sequence=0;
    {std::lock_guard<std::mutex> guard(state.mutex);state.has_frame=!slot.clear;state.sequence=slot.sequence;}
    UINT id=0; require(state.chain->GetLastPresentCount(&id),"Cannot query video submission");
    if(state.submitted_sequence!=slot.sequence){state.submitted_sequence=slot.sequence;std::lock_guard<std::mutex> guard(state.mutex);if(!slot.clear)++state.submitted;}
    state.pending.push_back({id,slot.sequence,slot.epoch,slot.clear});if(state.pending.size()>64)state.pending.pop_front();
    if(!state.content_attached||state.content_changed||state.visual_x!=layout.x||state.visual_y!=layout.y){
      require(state.visual->SetOffsetX(layout.x),"Cannot position video viewport");require(state.visual->SetOffsetY(layout.y),"Cannot position video viewport");
      require(state.visual->SetContent(state.chain.Get()),"Cannot set video viewport content");require(state.composition->Commit(),"Cannot commit video viewport");
      state.content_attached=true;state.content_changed=false;state.visual_x=layout.x;state.visual_y=layout.y;
    }
    return true;
  }
  void upload(Slot& slot) {
    if (slot.texture_width != slot.width || slot.texture_height != slot.height) {
      // A queued slot has no outstanding GPU readers after its retirement query.
      slot.views = {}; slot.textures = {};
      for (int plane = 0; plane < (i420 ? 3 : 1); ++plane) {
        D3D11_TEXTURE2D_DESC tex{};
        tex.Width = plane ? slot.width / 2 : slot.width; tex.Height = plane ? slot.height / 2 : slot.height;
        tex.MipLevels = tex.ArraySize = 1; tex.SampleDesc.Count = 1;
        tex.Format = i420 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        tex.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        require(device->CreateTexture2D(&tex, nullptr, &slot.textures[plane]), "Cannot allocate shared video texture");
        require(device->CreateShaderResourceView(slot.textures[plane].Get(), nullptr, &slot.views[plane]), "Cannot create shared video view");
      }
      slot.texture_width = slot.width; slot.texture_height = slot.height;
    }
    if (i420) {
      const size_t luma_bytes = size_t(slot.width) * slot.height;
      const auto* data = slot.bytes.data();
      context->UpdateSubresource(slot.textures[0].Get(), 0, nullptr, data, slot.width, 0);
      context->UpdateSubresource(slot.textures[1].Get(), 0, nullptr, data + luma_bytes, slot.width / 2, 0);
      context->UpdateSubresource(slot.textures[2].Get(), 0, nullptr, data + luma_bytes + luma_bytes / 4, slot.width / 2, 0);
      slot.conversion_counted = false;
    } else context->UpdateSubresource(slot.textures[0].Get(), 0, nullptr, slot.bytes.data(), slot.width * 4, 0);
    dimensions.store((uint64_t(slot.width) << 32) | slot.height);
    ++uploaded;
  }
  void draw_surfaces(const std::vector<std::shared_ptr<Surface>>& states, Slot& slot, bool fresh, bool retry_only = false) {
    bool commands = fresh;
    for (auto& state : states) {
      if (retry_only && (state->retry_sequence != slot.sequence || state->retry_epoch != slot.epoch)) continue;
      try { commands = draw(*state, slot, fresh) || commands; }
      catch (const std::exception& error) {
        require(device->GetDeviceRemovedReason(), "Video D3D device was removed");
        { std::lock_guard<std::mutex> guard(state->mutex); state->error = error.what(); state->has_frame = false; }
        state->failed = true; release_surface(*state); commands = true;
      }
    }
    if (commands) { context->End(slot.completion.Get()); context->Flush(); }
  }
  void run() noexcept {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    try {
      initialize();
      while(!stopping&&!failed){
        require(device->GetDeviceRemovedReason(),"Video D3D device was removed");
        auto states=attached();
        for(auto& state:states)if(!state->invalidated)statistics(*state);
        for(auto& slot:slots)if(slot.status==4&&context->GetData(slot.completion.Get(),nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK)slot.status=0;
        if(current_epoch!=epoch){
          current_epoch=epoch;if(current){current->status=4;current=nullptr;}
          for(auto& state:states){state->visual->SetContent(nullptr);state->composition->Commit();state->content_attached=false;state->pending.clear();state->redraw=true;std::lock_guard<std::mutex> guard(state->mutex);state->has_frame=false;}
        }
        Slot* next=nullptr;for(auto& slot:slots)if(slot.status==2&&(!next||slot.sequence<next->sequence))next=&slot;
        bool fresh=false;
        if(next){if(next->epoch!=epoch)next->status=0;else{
          // Leave the next viewport permit for the new size instead of retrying the old frame.
          if(current&&current->width==next->width&&current->height==next->height)draw_surfaces(states,*current,false,true);
          upload(*next);if(current)current->status=4;current=next;current->status=3;fresh=true;
        }}
        if(current)draw_surfaces(states,*current,fresh);
        std::vector<std::shared_ptr<Surface>> removed;
        {std::lock_guard<std::mutex> guard(surfaces_mutex);for(auto it=surfaces.begin();it!=surfaces.end();)if((*it)->invalidated){removed.push_back(*it);it=surfaces.erase(it);}else ++it;}
        for(auto& state:removed)release_surface(*state);
        std::unique_lock<std::mutex> lock(wake_mutex);wake.wait_for(lock,std::chrono::milliseconds(4));
      }
    }catch(const std::exception& error){fail(error.what());}
    for(auto& state:attached())release_surface(*state);
    if(context){context->ClearState();context->Flush();}
    for (auto& slot : slots) { slot.views = {}; slot.textures = {}; slot.completion.Reset(); }
    constants.Reset();sampler.Reset();vertex.Reset();pixel.Reset();factory.Reset();context.Reset();device.Reset();
    CoUninitialize();
  }
};
class Source final : public VideoPresentationSource {
public:
  std::shared_ptr<Worker> worker;
  Source(int width,int height,bool i420):worker(std::make_shared<Worker>(width,height,i420)) {
    std::thread([state=worker]{state->run();}).detach();
  }
  ~Source() override{worker->stopping=true;worker->wake.notify_one();}
  void submit(const VideoPresentationFrame& frame) override {
    if(worker->failed||worker->stopping)return;
    if(!frame.data||!frame.bytes){++worker->epoch;worker->wake.notify_one();return;}
    const uint32_t width=frame.width?frame.width:uint32_t(worker->width),height=frame.height?frame.height:uint32_t(worker->height);
    if(!width||!height||width>8192||height>8192||uint64_t(width)*height>10485760||
       (worker->i420&&((width&1)||(height&1)))||(frame.stride&&frame.stride!=width*(worker->i420?1:4))||
       frame.bytes!=size_t(width)*height*(worker->i420?3:8)/2){worker->fail("Invalid native video frame dimensions");return;}
    const uint64_t epoch=frame.discontinuity?++worker->epoch:worker->epoch.load();
    Slot* selected=nullptr;for(auto& slot:worker->slots){unsigned free=0;if(slot.status.compare_exchange_strong(free,1)){selected=&slot;break;}}
    if(!selected){++worker->dropped;return;}
    try {
      if(frame.bytes>selected->bytes.capacity())std::vector<unsigned char>(frame.bytes).swap(selected->bytes);
      else selected->bytes.resize(frame.bytes);
    }
    catch(const std::exception& error){selected->status=0;worker->fail(error.what());return;}
    selected->width=width;selected->height=height;
    std::memcpy(selected->bytes.data(),frame.data,frame.bytes);selected->sequence=frame.sequence;selected->epoch=epoch;selected->clear=frame.discontinuity;selected->status=2;worker->wake.notify_one();
  }
};
class Presenter final : public WinVideoPresenter {
  std::shared_ptr<CompositionHost> host_;
  std::shared_ptr<Source> source_;
  std::shared_ptr<Surface> state_=std::make_shared<Surface>();
  uint64_t generation_=0;
  Json layout_;
  RECT bounds_{};
  double scale_=1;
  bool visible_=false;
public:
  Presenter(std::shared_ptr<CompositionHost> host,std::shared_ptr<Source> source):host_(std::move(host)),source_(std::move(source)) {
    state_->composition=host_->composition;require(host_->composition->CreateVisual(&state_->visual),"Cannot create native video visual");generation_=host_->attach(state_->visual.Get());
    {std::lock_guard<std::mutex> guard(source_->worker->surfaces_mutex);source_->worker->surfaces.push_back(state_);}source_->worker->wake.notify_one();
  }
  ~Presenter() override{invalidate();}
  std::shared_ptr<VideoPresentationSource> source()const override{return source_;}
  bool closed()const override{return state_->invalidated||state_->failed||source_->worker->failed;}
  void detach_view()noexcept override{host_->detach(generation_);}
  void invalidate()noexcept override{state_->invalidated=true;if(GetCurrentThreadId()==host_->thread)detach_view();source_->worker->wake.notify_one();}
  void layout(const Json& value)override{layout_=value;update_view(bounds_,scale_,visible_);}
  void update_view(const RECT& bounds,double scale,bool visible)override {
    bounds_=bounds;scale_=scale;visible_=visible;
    if(closed()){detach_view();return;}
    Layout next;
    if(layout_.is_object()&&scale>0&&std::isfinite(scale)){
      const auto& rect=layout_.at("rect");const auto& clip=layout_.at("clip");
      double x=rect.at("x"),y=rect.at("y"),w=rect.at("width"),h=rect.at("height");
      const double left=(std::max)({x,clip.at("x").get<double>(),0.0});const double top=(std::max)({y,clip.at("y").get<double>(),0.0});
      const double right=(std::min)({x+w,clip.at("x").get<double>()+clip.at("width").get<double>(),(bounds.right-bounds.left)/scale});
      const double bottom=(std::min)({y+h,clip.at("y").get<double>()+clip.at("height").get<double>(),(bounds.bottom-bounds.top)/scale});
      if(w>0&&h>0&&right>left&&bottom>top){
        next.content_width=w;next.content_height=h;next.aspect=layout_.value("preserveAspect",true);
        next.width=int(std::ceil((right-left)*scale));next.height=int(std::ceil((bottom-top)*scale));
        if(next.width>8192||next.height>8192||uint64_t(next.width)*next.height>10485760)throw Error("VIDEO_PIXEL_BUDGET","Native video viewport exceeds the pixel budget");
        next.x=float(bounds.left+left*scale);next.y=float(bounds.top+top*scale);next.uv={float((left-x)/w),float((top-y)/h),float((right-left)/w),float((bottom-top)/h)};next.visible=visible;
      }
    }
    {std::lock_guard<std::mutex> guard(state_->mutex);const auto& old=state_->layout;if(old.width==next.width&&old.height==next.height&&old.x==next.x&&old.y==next.y&&old.uv==next.uv&&old.content_width==next.content_width&&old.content_height==next.content_height&&old.aspect==next.aspect&&old.visible==next.visible)return;state_->layout=next;state_->redraw=true;}
    source_->worker->wake.notify_one();
  }
  Json diagnostics()const override {
    std::lock_guard<std::mutex> guard(state_->mutex);const bool available=state_->statistics_available;
    const auto dimensions=source_->worker->dimensions.load();
    return {{"renderer","native-d3d11"},{"closed",closed()},{"hasFrame",state_->has_frame},{"submittedFrames",state_->submitted},
      {"presentedFrames",available?Json(state_->presented):Json(nullptr)},{"droppedFrames",state_->dropped+source_->worker->dropped.load()},
      {"sourceWidth",uint32_t(dimensions>>32)},{"sourceHeight",uint32_t(dimensions)},{"sequence",state_->sequence},
      {"sourceUploadedFrames",source_->worker->uploaded.load()},{"sourceConvertedFrames",source_->worker->converted.load()},{"sourceDroppedFrames",source_->worker->dropped.load()},
      {"presentationAvailable",available},{"presentationCounter","dxgi-observed"},
      {"presentationFPS",available?Json(state_->presented>1&&state_->last_presented>state_->first_presented?(state_->presented-1)/(state_->last_presented-state_->first_presented):0):Json(nullptr)},
      {"maxDrawableAcquireMs",state_->max_present_ms},{"error",state_->error}};
  }
};
}
std::shared_ptr<WinVideoHost> make_win_video_host(HWND window){return std::make_shared<CompositionHost>(window);}
std::shared_ptr<WinVideoPresenter> make_win_video_presenter(const std::shared_ptr<WinVideoHost>& host,const Json& descriptor) {
  const int width=descriptor.at("width"),height=descriptor.at("height");const bool i420=descriptor.value("format","")=="i420";
  if((!i420&&descriptor.value("format","")!="rgba8")||width<=0||height<=0||uint64_t(width)*height>10485760||descriptor.value("stride",0)!=width*(i420?1:4))throw Error("VIDEO_FORMAT_UNSUPPORTED","Native D3D presentation requires tightly packed project video frames");
  const auto conversion=descriptor.value("colorConversion","");
  if(i420&&(descriptor.value("colorMatrix","")!="bt709"||descriptor.value("colorRange","")!="limited"||
      (conversion!="reaper-sse"&&conversion!="reaper-directdraw")||width%2||height%2))throw Error("VIDEO_FORMAT_UNSUPPORTED","Native D3D I420 presentation requires the verified REAPER conversion");
  const auto name=descriptor.at("name").get<std::string>();if(name.empty())throw Error("INVALID_ARGUMENT","Video source name is required");
  static std::mutex mutex;static std::map<std::string,std::weak_ptr<Source>> registry;std::shared_ptr<Source> source;
  {std::lock_guard<std::mutex> guard(mutex);for(auto it=registry.begin();it!=registry.end();)if(it->second.expired())it=registry.erase(it);else ++it;
    source=registry[name].lock();if(source&&source->worker->i420!=i420)throw Error("VIDEO_FORMAT_CHANGED","Native video source format changed");
    if(!source){source=std::make_shared<Source>(width,height,i420);registry[name]=source;}}
  return std::make_shared<Presenter>(std::static_pointer_cast<CompositionHost>(host),std::move(source));
}
}
