#include "../src/platform/windows/win_video_presenter.cpp"
#include <iostream>
#include <stdexcept>

using namespace reaweb;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(std::string("Check failed at ") + std::to_string(__LINE__) + ": " #value); } while (false)

void wait_gpu(Worker& worker, Slot& slot) {
  worker.context->End(slot.completion.Get());
  worker.context->Flush();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (worker.context->GetData(slot.completion.Get(), nullptr, 0, 0) != S_OK) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    Sleep(1);
  }
}

void verify_upload(Worker& worker, Slot& slot, unsigned char value) {
  for (int plane = 0; plane < (worker.i420 ? 3 : 1); ++plane) {
    D3D11_TEXTURE2D_DESC desc{};
    slot.textures[plane]->GetDesc(&desc);
    CHECK(desc.Width == (plane ? slot.width / 2 : slot.width));
    CHECK(desc.Height == (plane ? slot.height / 2 : slot.height));
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    require(worker.device->CreateTexture2D(&desc, nullptr, &readback), "Cannot allocate test readback");
    worker.context->CopyResource(readback.Get(), slot.textures[plane].Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(worker.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Cannot read test video texture");
    const size_t row_bytes = size_t(desc.Width) * (worker.i420 ? 1 : 4);
    const auto* pixels = static_cast<const unsigned char*>(mapped.pData);
    bool equal = true;
    for (unsigned y = 0; y < desc.Height; ++y)
      for (size_t x = 0; x < row_bytes; ++x) equal &= pixels[size_t(y) * mapped.RowPitch + x] == value;
    worker.context->Unmap(readback.Get(), 0);
    CHECK(equal);
  }
}

void check_format(HWND window, bool i420) {
  auto host = std::make_shared<CompositionHost>(window);
  Worker worker(1280, 720, i420); worker.initialize();
  Surface surface; surface.composition = host->composition;
  require(host->composition->CreateVisual(&surface.visual), "Cannot create test visual");
  host->attach(surface.visual.Get());
  surface.layout.width = 320; surface.layout.height = 240;
  surface.layout.content_width = 320; surface.layout.content_height = 240; surface.layout.visible = true;
  const auto* device = worker.device.Get();
  ComPtr<IDXGISwapChain1> chain;
  auto& slot = worker.slots[0];
  uint64_t sequence = 0;
  for (const auto size : {std::pair{1280u, 720u}, {1920u, 1080u}, {3840u, 2160u}, {640u, 480u}, {480u, 640u}, {1280u, 720u}}) {
    slot.width = size.first; slot.height = size.second;
    slot.sequence = ++sequence; slot.epoch = worker.epoch;
    const auto value = static_cast<unsigned char>(40 + sequence);
    slot.bytes.assign(size_t(slot.width) * slot.height * (i420 ? 3 : 8) / 2, value);
    worker.upload(slot); verify_upload(worker, slot, value);
    if(sequence>1){surface.retry_sequence=sequence-1;surface.retry_epoch=slot.epoch;}
    const auto dropped=surface.dropped;
    // This hidden test surface does not receive desktop frame pacing signals.
    surface.frame_permitted = true;
    CHECK(worker.draw(surface, slot, true));
    if(sequence>1){CHECK(surface.retry_sequence!=sequence-1);CHECK(surface.dropped==dropped+1);}
    wait_gpu(worker, slot);
    if (!chain) chain = surface.chain;
    CHECK(chain.Get() == surface.chain.Get() && device == worker.device.Get());
    CHECK(!surface.failed && worker.dimensions == ((uint64_t(slot.width) << 32) | slot.height));
  }
  CHECK(worker.uploaded == sequence);
  worker.release_surface(surface);
  worker.context->ClearState(); worker.context->Flush();
}

void check_submission(HWND window) {
  Json desc={{"name","dynamic.video"},{"format","rgba8"},{"width",1280},{"height",720},{"stride",5120}};
  auto host=make_win_video_host(window);
  auto presenter=make_win_video_presenter(host,desc);
  auto source=std::static_pointer_cast<Source>(presenter->source());
  uint64_t sequence=0;
  for(const auto size:{std::pair{1280u,720u},{3840u,2160u},{480u,640u},{1920u,1080u}}) {
    std::vector<unsigned char> pixels(size_t(size.first)*size.second*4,51);
    source->submit({pixels.data(),pixels.size(),++sequence,0,false,size.first,size.second,size.first*4});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(source->worker->uploaded<sequence&&!source->worker->failed) {
      CHECK(std::chrono::steady_clock::now()<deadline);Sleep(1);
    }
    CHECK(!presenter->closed());
    const auto diagnostics=presenter->diagnostics();
    CHECK(diagnostics["sourceWidth"]==size.first&&diagnostics["sourceHeight"]==size.second);
  }
  desc["width"]=1920;desc["height"]=1080;desc["stride"]=7680;
  auto late=make_win_video_presenter(host,desc);
  CHECK(late->source()==source&&!late->closed());
  late->invalidate();presenter->invalidate();
}

int main() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const auto window = CreateWindowExW(0, L"STATIC", L"Dynamic video test", WS_OVERLAPPEDWINDOW,
    0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  try {
    CHECK(window);
    check_format(window, false); check_format(window, true); check_submission(window);
    DestroyWindow(window); CoUninitialize();
    std::cout << "D3D11 dynamic uploads, retained device/swapchain and late consumer sharing passed\n";
    return 0;
  } catch (const std::exception& error) {
    if (window) DestroyWindow(window);
    CoUninitialize(); std::cerr << error.what() << '\n'; return 1;
  }
}
