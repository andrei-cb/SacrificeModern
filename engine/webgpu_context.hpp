#pragma once
#include "sacrifice/assets.hpp"
#include <functional>
#include <memory>
#include <webgpu/webgpu.h>

struct ImDrawData;
namespace sacrifice {
// Browser device/surface ownership, also used by the forthcoming scene renderer.
class WebGpuContext {
  public:
    struct Texture {
        WGPUTexture texture{};
        WGPUTextureView view{};
        ~Texture();
    };
    WebGpuContext();
    ~WebGpuContext();
    WebGpuContext(const WebGpuContext&) = delete;
    WebGpuContext& operator=(const WebGpuContext&) = delete;
    void start();
    bool ready() const {
        return device_ && error_.empty();
    }
    const std::string& error() const {
        return error_;
    }
    WGPUDevice device() const {
        return device_;
    }
    WGPUQueue queue() const {
        return queue_;
    }
    WGPUTextureFormat format() const {
        return format_;
    }
    bool draw(ImDrawData* data, unsigned width, unsigned height,
              const std::function<void(WGPUCommandEncoder, WGPUTextureView)>& scene = {});
    std::shared_ptr<Texture> upload(const Image& image);
    void update(Texture& texture, const Image& image);

  private:
    void request_device();
    void configure(unsigned width, unsigned height);
    void fail(std::string message);
    WGPUInstance instance_{};
    WGPUAdapter adapter_{};
    WGPUDevice device_{};
    WGPUQueue queue_{};
    WGPUSurface surface_{};
    WGPUTextureFormat format_{WGPUTextureFormat_Undefined};
    unsigned width_{}, height_{};
    std::string error_;
    bool started_{};
};
} // namespace sacrifice
