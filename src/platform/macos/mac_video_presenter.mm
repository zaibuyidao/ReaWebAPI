#include "platform/macos/mac_video_presenter.hpp"
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

@interface ReaWebVideoSurface : NSView
@end
@implementation ReaWebVideoSurface
- (BOOL)isFlipped { return YES; }
- (NSView*)hitTest:(NSPoint)point { (void)point; return nil; }
@end

namespace reaweb {
namespace {
const char* metal_source = R"METAL(
#include <metal_stdlib>
using namespace metal;
kernel void convertI420(texture2d<uint,access::read> source [[texture(0)]],
 texture2d<float,access::write> target [[texture(1)]],
 constant uint2& size [[buffer(0)]], uint2 at [[thread_position_in_grid]]) {
 if(any(at>=size))return;
 int y=int(source.read(at).r)-16;
 uint ur=at.y/2,vr=ur+size.y/2;
 int u=int(source.read(uint2(at.x/2+(ur&1)*(size.x/2),size.y+ur/2)).r)-128;
 int v=int(source.read(uint2(at.x/2+(vr&1)*(size.x/2),size.y+vr/2)).r)-128;
 int yy=(y*9539)>>12;
 int r=(yy+((v*14686)>>12)+1)>>1;
 int g=(yy+((u*-1747)>>12)+((v*-4366)>>12)+1)>>1;
 int b=(yy+((u*17305)>>12)+1)>>1;
 target.write(float4(float3(clamp(int3(r,g,b),int3(0),int3(255)))/255.0,1),at);
}
vertex float4 videoVertex(uint index [[vertex_id]]) {
 const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
 return float4(p[index],0,1);
}
fragment float4 presentRGB(float4 at [[position]],texture2d<float,access::read> source [[texture(0)]],
 constant float* output [[buffer(0)]]) {
 int2 size=int2(source.get_width(),source.get_height());
 float2 point=float2(output[2],output[3])+at.xy*float2(output[4],output[5])/float2(output[0],output[1]);
 if(any(point<float2(0))||any(point>=float2(size)))return float4(0,0,0,1);
 float2 p=point-0.5;int2 base=int2(floor(p));float2 f=fract(p);int2 limit=size-1;
 float3 a=source.read(uint2(clamp(base,int2(0),limit))).rgb;
 if(all(f==float2(0)))return float4(a,1);
 float3 b=source.read(uint2(clamp(base+int2(1,0),int2(0),limit))).rgb;
 float3 c=source.read(uint2(clamp(base+int2(0,1),int2(0),limit))).rgb;
 float3 d=source.read(uint2(clamp(base+int2(1,1),int2(0),limit))).rgb;
 return float4(mix(mix(a,b,f.x),mix(c,d,f.x),f.y),1);
}
)METAL";

struct SurfaceState {
  CAMetalLayer* layer;
  __weak NSView* view;
  std::atomic<uint64_t> layer_revision{1};
  std::atomic<bool> invalidated{false}, failed{false}, visible{false}, has_frame{false}, redraw{true};
  std::atomic<unsigned> outstanding{0};
  std::atomic<uint64_t> generation{1};
  std::atomic<uint64_t> submitted{0}, presented{0}, dropped{0}, sequence{0};
  std::mutex mutex;
  std::array<float,4> uv{0,0,1,1};
  double content_width=0,content_height=0;
  bool aspect=true;
  std::string error;
  double first_presented=0,last_presented=0,max_acquire_ms=0;
};

CAMetalLayer* video_layer(id<MTLDevice> device) {
  CAMetalLayer* layer=[CAMetalLayer layer];layer.device=device;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly=YES;layer.maximumDrawableCount=3;layer.displaySyncEnabled=YES;layer.allowsNextDrawableTimeout=YES;
  CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);layer.colorspace=color;CGColorSpaceRelease(color);
  layer.backgroundColor=NSColor.blackColor.CGColor;
  return layer;
}

struct PresentationLease {
  std::shared_ptr<SurfaceState> state;
  std::atomic<bool> released{false};
  uint64_t revision;
  PresentationLease(std::shared_ptr<SurfaceState> value,uint64_t generation):state(std::move(value)),revision(generation) {}
  void release(){
    if(released.exchange(true))return;
    std::lock_guard<std::mutex> guard(state->mutex);
    if(state->layer_revision==revision)--state->outstanding;
  }
};

class MetalSource final : public VideoPresentationSource, public std::enable_shared_from_this<MetalSource> {
  struct Slot {
    std::vector<unsigned char> bytes;
    uint32_t width=0,height=0;
    id<MTLTexture> input=nil,rgb=nil;
    std::atomic<unsigned> leases{0};
    uint64_t sequence=0,generation=0;
    bool clear=false;
  };
  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  id<MTLComputePipelineState> convert_;
  id<MTLRenderPipelineState> present_;
  dispatch_queue_t work_=dispatch_queue_create("reaweb.video.present",DISPATCH_QUEUE_SERIAL);
  std::array<Slot,3> slots_;
  Slot* current_=nullptr;
  std::atomic<uint64_t> generation_{1},dropped_{0},uploaded_{0},converted_{0};
  std::atomic<bool> redraw_pending_{false};
  std::mutex surfaces_mutex_;
  std::vector<std::weak_ptr<SurfaceState>> surfaces_;
  int width_,height_;
  std::atomic<uint64_t> dimensions_;
  bool i420_;

  std::vector<std::shared_ptr<SurfaceState>> surfaces() {
    std::vector<std::shared_ptr<SurfaceState>> result;
    std::lock_guard<std::mutex> guard(surfaces_mutex_);
    for(auto it=surfaces_.begin();it!=surfaces_.end();) {
      auto state=it->lock();
      if(!state||state->invalidated){it=surfaces_.erase(it);continue;}
      result.push_back(std::move(state));++it;
    }
    return result;
  }
  void fail(const std::string& message) {
    for(auto state:surfaces()) {
      {std::lock_guard<std::mutex> guard(state->mutex);state->error=message;}
      state->failed=true;
    }
  }
  void clear_surfaces(uint64_t generation) {
    const auto self=shared_from_this();
    for(auto state:surfaces()) {
      {std::lock_guard<std::mutex> guard(state->mutex);state->generation=generation;state->has_frame=false;state->redraw=true;}
      dispatch_async(dispatch_get_main_queue(),^{
        if(state->invalidated||state->generation!=generation)return;
        NSView* view=state->view;if(!view)return;
        CAMetalLayer* layer=video_layer(self->device_);
        {std::lock_guard<std::mutex> guard(state->mutex);
          layer.contentsScale=state->layer.contentsScale;layer.drawableSize=state->layer.drawableSize;
          state->layer=layer;++state->layer_revision;state->outstanding=0;
        }
        [CATransaction begin];[CATransaction setDisableActions:YES];view.layer=layer;[CATransaction commit];
        state->redraw=true;self->redraw();
      });
    }
  }
  void encode(Slot* slot,bool upload) {
    if(slot->generation!=generation_) {--slot->leases;return;}
    const auto width=slot->width,height=slot->height;
    if(upload&&(slot->rgb.width!=width||slot->rgb.height!=height)) {
      // Zero leases before submission guarantees that old textures are no longer in use.
      slot->input=nil;slot->rgb=nil;
      if(i420_) {
        MTLTextureDescriptor* input=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint width:width height:height*3/2 mipmapped:NO];
        input.storageMode=MTLStorageModeShared;input.usage=MTLTextureUsageShaderRead;slot->input=[device_ newTextureWithDescriptor:input];
      }
      MTLTextureDescriptor* rgb=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
      rgb.storageMode=i420_?MTLStorageModePrivate:MTLStorageModeShared;rgb.usage=MTLTextureUsageShaderRead|(i420_?MTLTextureUsageShaderWrite:MTLTextureUsageUnknown);slot->rgb=[device_ newTextureWithDescriptor:rgb];
      if((i420_&&!slot->input)||!slot->rgb){--slot->leases;fail("Metal video texture allocation failed");return;}
    }
    id<MTLCommandBuffer> command=[queue_ commandBuffer];
    if(!command){--slot->leases;fail("Metal command buffer allocation failed");return;}
    if(upload) {
      if(i420_) {
      [slot->input replaceRegion:MTLRegionMake2D(0,0,width,height*3/2) mipmapLevel:0 withBytes:slot->bytes.data() bytesPerRow:width];
      id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];
      if(!compute){--slot->leases;fail("Metal video conversion encoder allocation failed");return;}
      [compute setComputePipelineState:convert_];[compute setTexture:slot->input atIndex:0];[compute setTexture:slot->rgb atIndex:1];
      const uint32_t size[2]={uint32_t(width),uint32_t(height)};
      [compute setBytes:size length:sizeof(size) atIndex:0];
      [compute dispatchThreads:MTLSizeMake(width,height,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];[compute endEncoding];
      ++converted_;
      } else {
        [slot->rgb replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:slot->bytes.data() bytesPerRow:width*4];
      }
      dimensions_.store((uint64_t(width)<<32)|height);
      ++uploaded_;
    }
    std::vector<std::shared_ptr<PresentationLease>> acquired;
    for(auto state:surfaces()) {
      if(state->failed||!state->visible)continue;
      const bool redraw=state->redraw.exchange(false);
      if(!upload&&!redraw)continue;
      CAMetalLayer* layer;uint64_t layer_revision;
      {
        std::lock_guard<std::mutex> guard(state->mutex);
        if(state->invalidated||state->failed||slot->generation!=generation_||state->generation!=slot->generation)continue;
        if(state->outstanding>=2){if(upload)++state->dropped;state->redraw=true;continue;}
        ++state->outstanding;layer=state->layer;layer_revision=state->layer_revision.load();
      }
      auto lease=std::make_shared<PresentationLease>(state,layer_revision);
      const double started=CACurrentMediaTime();id<CAMetalDrawable> drawable=[layer nextDrawable];
      std::array<float,4> uv;bool obsolete;
      {std::lock_guard<std::mutex> guard(state->mutex);
        state->max_acquire_ms=std::max(state->max_acquire_ms,(CACurrentMediaTime()-started)*1000);uv=state->uv;
        if(state->aspect&&state->content_width>0&&state->content_height>0) {
          const double fit=std::min(state->content_width/width,state->content_height/height);
          const double fw=fit*width/state->content_width,fh=fit*height/state->content_height;
          uv={float((uv[0]-(1-fw)/2)/fw),float((uv[1]-(1-fh)/2)/fh),float(uv[2]/fw),float(uv[3]/fh)};
        }
        obsolete=state->invalidated||state->failed||slot->generation!=generation_||state->generation!=slot->generation||state->layer_revision!=layer_revision;
      }
      if(obsolete){lease->release();continue;}
      if(!drawable){lease->release();if(upload)++state->dropped;state->redraw=true;continue;}
      MTLRenderPassDescriptor* pass=[MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;
      pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);pass.colorAttachments[0].storeAction=MTLStoreActionStore;
      id<MTLRenderCommandEncoder> render=[command renderCommandEncoderWithDescriptor:pass];
      if(!render){lease->release();for(const auto& previous:acquired)previous->release();--slot->leases;fail("Metal video render encoder allocation failed");return;}
      [render setRenderPipelineState:present_];[render setFragmentTexture:slot->rgb atIndex:0];
      const float output[6]={float(drawable.texture.width),float(drawable.texture.height),uv[0]*width,uv[1]*height,uv[2]*width,uv[3]*height};
      [render setFragmentBytes:output length:sizeof(output) atIndex:0];
      [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[render endEncoding];
      const uint64_t sequence=slot->sequence,generation=slot->generation;const bool clear=slot->clear;
      if(upload)++state->submitted;
      [drawable addPresentedHandler:^(id<MTLDrawable> presented){
        lease->release();
        std::lock_guard<std::mutex> guard(state->mutex);
        if(state->invalidated||state->failed||state->generation!=generation||state->layer_revision!=layer_revision)return;
        state->has_frame=!clear;state->sequence=sequence;
        if(upload) {
          ++state->presented;
          if(!state->first_presented)state->first_presented=presented.presentedTime;
          state->last_presented=presented.presentedTime;
        }
      }];
      [command presentDrawable:drawable];acquired.push_back(std::move(lease));
    }
    if(upload) {
      ++slot->leases;
      if(current_)--current_->leases;
      current_=slot;
    }
    const auto self=shared_from_this();
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed){
      if(completed.status==MTLCommandBufferStatusError) {
        for(const auto& lease:acquired)lease->release();
        self->fail(completed.error.localizedDescription.UTF8String?:"Metal presentation failed");
      }
      --slot->leases;
    }];
    [command commit];
  }
public:
  MetalSource(int width,int height,bool i420=true):width_(width),height_(height),dimensions_((uint64_t(width)<<32)|uint32_t(height)),i420_(i420) {
    device_=MTLCreateSystemDefaultDevice();
    if(!device_)throw Error("VIDEO_RENDERER_UNAVAILABLE","Metal is unavailable");
    queue_=[device_ newCommandQueue];
    NSError* error=nil;MTLCompileOptions* options=[MTLCompileOptions new];options.fastMathEnabled=NO;
    id<MTLLibrary> library=[device_ newLibraryWithSource:[NSString stringWithUTF8String:metal_source] options:options error:&error];
    if(!library)throw Error("VIDEO_RENDERER_UNAVAILABLE",error.localizedDescription.UTF8String?:"Metal shader compilation failed");
    convert_=[device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"convertI420"] error:&error];
    MTLRenderPipelineDescriptor* pipeline=[MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction=[library newFunctionWithName:@"videoVertex"];pipeline.fragmentFunction=[library newFunctionWithName:@"presentRGB"];
    pipeline.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
    present_=[device_ newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if(!queue_||!convert_||!present_)throw Error("VIDEO_RENDERER_UNAVAILABLE",error.localizedDescription.UTF8String?:"Metal pipeline allocation failed");
  }
  bool i420()const{return i420_;}
  uint64_t dimensions()const{return dimensions_.load();}
  uint64_t dropped() const{return dropped_;}
  uint64_t uploaded()const{return uploaded_;}
  uint64_t converted()const{return converted_;}
  id<MTLDevice> device()const{return device_;}
  void attach(const std::shared_ptr<SurfaceState>& state) {
    {std::lock_guard<std::mutex> guard(surfaces_mutex_);state->generation=generation_.load();surfaces_.push_back(state);}
    redraw();
  }
  void redraw() {
    if(redraw_pending_.exchange(true))return;
    const auto self=shared_from_this();
    dispatch_async(work_,^{@autoreleasepool {
      self->redraw_pending_=false;
      if(self->current_){++self->current_->leases;self->encode(self->current_,false);}
    }});
  }
  void submit(const VideoPresentationFrame& frame) override {
    if(!frame.data||!frame.bytes) {
      const auto generation=++generation_;
      clear_surfaces(generation);
      const auto self=shared_from_this();
      dispatch_async(work_,^{if(self->current_){--self->current_->leases;self->current_=nullptr;}});
      return;
    }
    const uint32_t width=frame.width?frame.width:uint32_t(width_),height=frame.height?frame.height:uint32_t(height_);
    if(!width||!height||width>8192||height>8192||uint64_t(width)*height>10485760||
       (i420_&&((width&1)||(height&1)))||(frame.stride&&frame.stride!=width*(i420_?1:4))||
       frame.bytes!=size_t(width)*height*(i420_?3:8)/2){fail("Invalid native video frame dimensions");return;}
    const auto generation=frame.discontinuity?++generation_:generation_.load();
    if(frame.discontinuity)clear_surfaces(generation);
    Slot* slot=nullptr;
    for(auto& candidate:slots_){unsigned expected=0;if(candidate.leases.compare_exchange_strong(expected,1)){slot=&candidate;break;}}
    if(!slot){++dropped_;return;}
    try {
      if(frame.bytes>slot->bytes.capacity())std::vector<unsigned char>(frame.bytes).swap(slot->bytes);
      else slot->bytes.resize(frame.bytes);
    }
    catch(const std::exception& error){--slot->leases;fail(error.what());return;}
    slot->width=width;slot->height=height;
    std::memcpy(slot->bytes.data(),frame.data,frame.bytes);slot->sequence=frame.sequence;slot->generation=generation;slot->clear=frame.discontinuity;
    const auto self=shared_from_this();
    dispatch_async(work_,^{@autoreleasepool{self->encode(slot,true);}});
  }
};

class Presenter final : public MacVideoPresenter {
  __weak WKWebView* webview_;
  ReaWebVideoSurface* view_;
  std::shared_ptr<MetalSource> source_;
  std::shared_ptr<SurfaceState> state_=std::make_shared<SurfaceState>();
  NSRect rect_{},clip_{};
  bool aspect_=true,has_layout_=false,updating_=false;
public:
  Presenter(WKWebView* webview,std::shared_ptr<MetalSource> source):webview_(webview),source_(std::move(source)) {
    view_=[[ReaWebVideoSurface alloc] initWithFrame:NSZeroRect];view_.wantsLayer=YES;
    CAMetalLayer* layer=video_layer(source_->device());
    view_.layer=layer;view_.hidden=YES;state_->layer=layer;state_->view=view_;source_->attach(state_);
  }
  ~Presenter() override{invalidate();}
  std::shared_ptr<VideoPresentationSource> source()const override{return source_;}
  bool closed()const override{return state_->invalidated||state_->failed;}
  void invalidate()noexcept override {
    if(state_->invalidated.exchange(true))return;
    state_->visible=false;
    ReaWebVideoSurface* view=view_;const auto state=state_;
    const auto cleanup=^{
      [view removeFromSuperview];view.layer=nil;
      std::lock_guard<std::mutex> guard(state->mutex);state->layer=nil;state->view=nil;
    };
    if([NSThread isMainThread])cleanup();
    else dispatch_async(dispatch_get_main_queue(),cleanup);
  }
  void layout(const Json& value)override {
    if(closed())throw Error("VIDEO_STREAM_CLOSED","Native video presenter is closed");
    const auto read=[](const Json& item) {
      NSRect rect=NSMakeRect(item.at("x").get<double>(),item.at("y").get<double>(),item.at("width").get<double>(),item.at("height").get<double>());
      for(double n:{rect.origin.x,rect.origin.y,rect.size.width,rect.size.height})if(!std::isfinite(n)||std::abs(n)>1e7)throw Error("INVALID_ARGUMENT","Invalid video surface rectangle");
      if(rect.size.width<0||rect.size.height<0)throw Error("INVALID_ARGUMENT","Invalid video surface size");
      return rect;
    };
    rect_=read(value.at("rect"));clip_=read(value.at("clip"));aspect_=value.value("preserveAspect",true);has_layout_=true;
    update_view();state_->redraw=true;source_->redraw();
  }
  void update_view()override {
    if(closed()||updating_||!view_)return;
    WKWebView* web=webview_;
    if(!web||!web.superview){state_->visible=false;return;}
    updating_=true;
    if(view_.superview!=web.superview){[view_ removeFromSuperview];[web.superview addSubview:view_ positioned:NSWindowBelow relativeTo:web];}
    NSRect visible=NSIntersectionRect(NSIntersectionRect(rect_,clip_),NSMakeRect(0,0,web.bounds.size.width,web.bounds.size.height));
    const bool valid=has_layout_&&visible.size.width>0&&visible.size.height>0;
    if(valid) {
      NSRect content=rect_;
      {std::lock_guard<std::mutex> guard(state_->mutex);
        state_->content_width=content.size.width;state_->content_height=content.size.height;state_->aspect=aspect_;
        state_->uv={float((visible.origin.x-content.origin.x)/content.size.width),float((visible.origin.y-content.origin.y)/content.size.height),float(visible.size.width/content.size.width),float(visible.size.height/content.size.height)};
      }
      NSRect target=visible;if(!web.isFlipped)target.origin.y=web.bounds.size.height-NSMaxY(target);
      target=[web convertRect:target toView:web.superview];
      const double scale=web.window.backingScaleFactor?:1;
      const CGSize pixels=CGSizeMake(std::max(1.0,std::round(visible.size.width*scale)),std::max(1.0,std::round(visible.size.height*scale)));
      if(!NSEqualRects(view_.frame,target)||!CGSizeEqualToSize(state_->layer.drawableSize,pixels)) {
        [CATransaction begin];[CATransaction setDisableActions:YES];view_.frame=target;state_->layer.contentsScale=scale;state_->layer.drawableSize=pixels;[CATransaction commit];state_->redraw=true;
      }
    }
    const bool active=valid&&web.window.visible&&!web.window.miniaturized&&!web.hidden;
    if(state_->visible.exchange(active)!=active)state_->redraw=true;
    view_.hidden=!valid;
    updating_=false;
    if(active&&state_->redraw)source_->redraw();
  }
  Json diagnostics()const override {
    std::lock_guard<std::mutex> guard(state_->mutex);
    const auto presented=state_->presented.load(),dimensions=source_->dimensions();
    return {{"renderer","native-metal"},{"closed",closed()},{"hasFrame",state_->has_frame.load()},
      {"submittedFrames",state_->submitted.load()},{"presentedFrames",presented},{"droppedFrames",state_->dropped.load()+source_->dropped()},
      {"sourceWidth",uint32_t(dimensions>>32)},{"sourceHeight",uint32_t(dimensions)},{"sequence",state_->sequence.load()},
      {"sourceUploadedFrames",source_->uploaded()},{"sourceConvertedFrames",source_->converted()},{"sourceDroppedFrames",source_->dropped()},
      {"presentationFPS",presented>1&&state_->last_presented>state_->first_presented?(presented-1)/(state_->last_presented-state_->first_presented):0},
      {"maxDrawableAcquireMs",state_->max_acquire_ms},{"error",state_->error}};
  }
};
}

bool mac_video_presenter_available() {
#if defined(__aarch64__)
  return MTLCreateSystemDefaultDevice()!=nil;
#else
  return false;
#endif
}
std::shared_ptr<MacVideoPresenter> make_mac_video_presenter(WKWebView* web,const Json& descriptor) {
  if(!mac_video_presenter_available())throw Error("VIDEO_RENDERER_UNAVAILABLE","Native project video presentation is unavailable");
  const int width=descriptor.at("width").get<int>(),height=descriptor.at("height").get<int>();
  const auto format=descriptor.value("format","");const bool i420=format=="i420";
  if((!i420&&format!="rgba8")||width<=0||height<=0||uint64_t(width)*height>10485760||descriptor.value("stride",0)!=width*(i420?1:4))
    throw Error("VIDEO_FORMAT_UNSUPPORTED","Native Metal presentation requires tightly packed project video frames");
  if(i420&&(descriptor.value("colorMatrix","")!="bt709"||descriptor.value("colorRange","")!="limited"||descriptor.value("colorConversion","")!="reaper-neon"||width%2||height%2))
    throw Error("VIDEO_FORMAT_UNSUPPORTED","Native Metal I420 presentation requires the verified REAPER conversion");
  const auto name=descriptor.at("name").get<std::string>();
  if(name.empty())throw Error("INVALID_ARGUMENT","Video source name is required");
  static std::mutex registry_mutex;
  static std::map<std::string,std::weak_ptr<MetalSource>> registry;
  std::shared_ptr<MetalSource> source;
  {std::lock_guard<std::mutex> guard(registry_mutex);
    for(auto it=registry.begin();it!=registry.end();)if(it->second.expired())it=registry.erase(it);else ++it;
    source=registry[name].lock();
    if(source&&source->i420()!=i420)throw Error("VIDEO_FORMAT_CHANGED","Native video source format changed");
    if(!source){source=std::make_shared<MetalSource>(width,height,i420);registry[name]=source;}
  }
  return std::make_shared<Presenter>(web,std::move(source));
}
}
