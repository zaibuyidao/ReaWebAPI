#include "../src/platform/windows/win_video_presenter.cpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <cmath>

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
  for (const auto size : {std::pair{1280u, 720u}, {1920u, 1080u}, {3840u, 2160u}, {640u, 480u}, {480u, 640u}, {1576u, 1272u}, {808u, 1272u}, {744u, 1288u}, {18u, 6u}, {2u, 2u}, {1280u, 720u}}) {
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

void check_submission(HWND window, bool i420) {
  Json desc={{"name","dynamic.video"},{"format","rgba8"},{"width",1280},{"height",720},{"stride",5120}};
  if(i420)desc.update({{"format","i420"},{"width",808},{"height",1272},{"stride",808},{"colorMatrix","bt709"},{"colorRange","limited"},{"colorConversion","reaper-sse"}});
  auto host=make_win_video_host(window);
  auto presenter=make_win_video_presenter(host,desc);
  auto source=std::static_pointer_cast<Source>(presenter->source());
  uint64_t sequence=0;
  for(const auto size:{std::pair{1576u,1272u},{808u,1272u},{744u,1288u},{3840u,2160u},{480u,640u},{1920u,1080u}}) {
    std::vector<unsigned char> pixels(size_t(size.first)*size.second*(i420?3:8)/2,51);
    source->submit({pixels.data(),pixels.size(),++sequence,0,false,size.first,size.second,size.first*(i420?1u:4u)});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(source->worker->uploaded<sequence&&!source->worker->failed) {
      CHECK(std::chrono::steady_clock::now()<deadline);Sleep(1);
    }
    CHECK(!presenter->closed());
    const auto diagnostics=presenter->diagnostics();
    CHECK(diagnostics["sourceWidth"]==size.first&&diagnostics["sourceHeight"]==size.second);
  }
  desc["width"]=1920;desc["height"]=1080;desc["stride"]=i420?1920:7680;
  auto late=make_win_video_presenter(host,desc);
  CHECK(late->source()==source&&!late->closed());
  late->invalidate();presenter->invalidate();
}


void check_i420_pixels(const fs::path& directory) {
  Worker worker(2, 2, true); worker.initialize();
  size_t files = 0, checked = 0; int maximum_error = 0;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (entry.path().extension() != ".i420") continue;
    std::ifstream input(entry.path(), std::ios::binary);
    uint32_t size[2]{}; input.read(reinterpret_cast<char*>(size), sizeof(size));
    const auto w = size[0], h = size[1]; CHECK(w && h && !(w & 1) && !(h & 1) && w <= 8192 && h <= 8192);
    auto& slot = worker.slots[0]; slot.width = w; slot.height = h;
    slot.bytes.resize(size_t(w) * h * 3 / 2); input.read(reinterpret_cast<char*>(slot.bytes.data()), slot.bytes.size()); CHECK(input.good());
    worker.upload(slot);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output, readback; ComPtr<ID3D11RenderTargetView> target;
    require(worker.device->CreateTexture2D(&desc, nullptr, &output), "Cannot create color test output");
    require(worker.device->CreateRenderTargetView(output.Get(), nullptr, &target), "Cannot create color test target");
    D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1}; auto* context = worker.context.Get();
    context->RSSetViewports(1, &viewport); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(worker.vertex.Get(), nullptr, 0); context->PSSetShader(worker.pixel.Get(), nullptr, 0);
    float values[8]{float(w), float(h), 0, 0, 0, 0, float(w), float(h)};
    context->UpdateSubresource(worker.constants.Get(), 0, nullptr, values, 0, 0);
    auto* constants = worker.constants.Get(); context->PSSetConstantBuffers(0, 1, &constants);
    ID3D11ShaderResourceView* views[]{slot.views[0].Get(), slot.views[1].Get(), slot.views[2].Get()}; context->PSSetShaderResources(0, 3, views);
    auto* view = target.Get(); context->OMSetRenderTargets(1, &view, nullptr); context->Draw(3, 0);
    ID3D11ShaderResourceView* empty[3]{}; context->PSSetShaderResources(0, 3, empty); context->OMSetRenderTargets(0, nullptr, nullptr);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(worker.device->CreateTexture2D(&desc, nullptr, &readback), "Cannot create color test readback");
    context->CopyResource(readback.Get(), output.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
    require(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Cannot map color test output");
    const auto chroma = [&](size_t plane, unsigned x, unsigned y) {
      const double sx = x * .5 - .25, sy = y * .5 - .25;
      const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy)); const double fx = sx - x0, fy = sy - y0;
      const auto at = [&](int px, int py) { return slot.bytes[plane + size_t(std::clamp(py, 0, int(h / 2) - 1)) * (w / 2) + std::clamp(px, 0, int(w / 2) - 1)]; };
      return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
             (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    };
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
      if (x && y && x + 1 != w && y + 1 != h && (x % 31 || y % 29)) continue;
      const double yy = (slot.bytes[size_t(y) * w + x] - 16.) / 219.;
      const double u = (chroma(size_t(w) * h, x, y) - 128) / 224.;
      const double v = (chroma(size_t(w) * h * 5 / 4, x, y) - 128) / 224.;
      const double expected[]{yy + u * 1.8556, yy - u * .187324273 - v * .468124273, yy + v * 1.5748};
      const auto* pixel = static_cast<const unsigned char*>(mapped.pData) + size_t(y) * mapped.RowPitch + x * 4;
      for (int c = 0; c < 3; ++c) maximum_error = (std::max)(maximum_error, std::abs(int(pixel[c]) - int(std::lround(std::clamp(expected[c], 0., 1.) * 255))));
      CHECK(pixel[3] == 255); ++checked;
    }
    context->Unmap(readback.Get(), 0); CHECK(maximum_error <= 1); ++files;
  }
  CHECK(files); std::cout << "I420 source files=" << files << " border/interior pixels=" << checked << " max RGB error=" << maximum_error << "\n";
}

int main(int argc, char** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const auto window = CreateWindowExW(0, L"STATIC", L"Dynamic video test", WS_OVERLAPPEDWINDOW,
    0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  try {
    CHECK(window);
    check_format(window, false); check_format(window, true); check_submission(window, false); check_submission(window, true);
    if (argc == 2) check_i420_pixels(fs::u8path(argv[1]));
    DestroyWindow(window); CoUninitialize();
    std::cout << "D3D11 dynamic uploads, retained device/swapchain and late consumer sharing passed\n";
    return 0;
  } catch (const std::exception& error) {
    if (window) DestroyWindow(window);
    CoUninitialize(); std::cerr << error.what() << '\n'; return 1;
  }
}
