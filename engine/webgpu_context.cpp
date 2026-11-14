#include "webgpu_context.hpp"
#include <cstring>
#include <imgui_impl_wgpu.h>

namespace sacrifice {
namespace {
std::string message_text(WGPUStringView text) {
    return text.data
               ? std::string(text.data, text.length == WGPU_STRLEN ? std::strlen(text.data) : text.length)
               : "";
}
} // namespace
void WebGpuContext::fail(std::string message) {
    if (error_.empty())
        error_ = std::move(message);
}
WebGpuContext::WebGpuContext() {
    instance_ = wgpuCreateInstance(nullptr);
    if (!instance_)
        throw Error("Cannot create WebGPU instance");
    auto canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = {"#canvas", 7};
    auto surface = WGPU_SURFACE_DESCRIPTOR_INIT;
    surface.nextInChain = &canvas.chain;
    surface_ = wgpuInstanceCreateSurface(instance_, &surface);
    if (!surface_) {
        wgpuInstanceRelease(instance_);
        instance_ = nullptr;
        throw Error("Cannot create WebGPU canvas surface");
    }
}
void WebGpuContext::start() {
    if (started_)
        return;
    started_ = true;
    auto options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.compatibleSurface = surface_;
    auto callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.userdata1 = this;
    callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                           void* user, void*) {
        auto& self = *static_cast<WebGpuContext*>(user);
        if (status != WGPURequestAdapterStatus_Success) {
            self.fail("No WebGPU adapter: " + message_text(message));
            return;
        }
        self.adapter_ = adapter;
        self.request_device();
    };
    wgpuInstanceRequestAdapter(instance_, &options, callback);
}
void WebGpuContext::request_device() {
    auto descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    auto limits = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(adapter_, &limits);
    auto required = WGPU_LIMITS_INIT;
    required.maxTextureArrayLayers = std::min(2048u, limits.maxTextureArrayLayers);
    descriptor.requiredLimits = &required;
    descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    descriptor.deviceLostCallbackInfo.userdata1 = this;
    descriptor.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason,
                                                    WGPUStringView message, void* user, void*) {
        if (reason != WGPUDeviceLostReason_Destroyed && reason != WGPUDeviceLostReason_CallbackCancelled)
            static_cast<WebGpuContext*>(user)->fail("WebGPU device lost: " + message_text(message));
    };
    descriptor.uncapturedErrorCallbackInfo.userdata1 = this;
    descriptor.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType,
                                                         WGPUStringView message, void* user, void*) {
        static_cast<WebGpuContext*>(user)->fail("WebGPU error: " + message_text(message));
    };
    auto callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.userdata1 = this;
    callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
                           void* user, void*) {
        auto& self = *static_cast<WebGpuContext*>(user);
        if (status != WGPURequestDeviceStatus_Success) {
            self.fail("Cannot create WebGPU device: " + message_text(message));
            return;
        }
        auto capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
        if (wgpuSurfaceGetCapabilities(self.surface_, self.adapter_, &capabilities) != WGPUStatus_Success ||
            !capabilities.formatCount) {
            wgpuSurfaceCapabilitiesFreeMembers(capabilities);
            wgpuDeviceRelease(device);
            self.fail("The WebGPU surface has no supported format");
            return;
        }
        self.format_ = capabilities.formats[0];
        for (std::size_t i = 0; i < capabilities.formatCount; ++i)
            if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm)
                self.format_ = capabilities.formats[i];
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
        self.queue_ = wgpuDeviceGetQueue(device);
        self.device_ = device;
    };
    wgpuAdapterRequestDevice(adapter_, &descriptor, callback);
}
WebGpuContext::~WebGpuContext() {
    if (surface_)
        wgpuSurfaceRelease(surface_);
    if (queue_)
        wgpuQueueRelease(queue_);
    if (device_)
        wgpuDeviceRelease(device_);
    if (adapter_)
        wgpuAdapterRelease(adapter_);
    if (instance_)
        wgpuInstanceRelease(instance_);
}
void WebGpuContext::configure(unsigned width, unsigned height) {
    auto config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device_;
    config.format = format_;
    config.width = width;
    config.height = height;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface_, &config);
    width_ = width;
    height_ = height;
}
bool WebGpuContext::draw(ImDrawData* data, unsigned width, unsigned height,
                         const std::function<void(WGPUCommandEncoder, WGPUTextureView)>& scene) {
    if (!ready() || !width || !height)
        return false;
    if (width != width_ || height != height_)
        configure(width, height);
    auto frame = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(surface_, &frame);
    if (frame.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        frame.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (frame.texture)
            wgpuTextureRelease(frame.texture);
        width_ = height_ = 0;
        if (frame.status != WGPUSurfaceGetCurrentTextureStatus_Timeout &&
            frame.status != WGPUSurfaceGetCurrentTextureStatus_Outdated &&
            frame.status != WGPUSurfaceGetCurrentTextureStatus_Lost)
            fail("Cannot acquire WebGPU canvas texture");
        return false;
    }
    auto view = wgpuTextureCreateView(frame.texture, nullptr);
    auto encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
    if (scene)
        scene(encoder, view);
    auto attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    attachment.view = view;
    attachment.loadOp = scene ? WGPULoadOp_Load : WGPULoadOp_Clear;
    attachment.storeOp = WGPUStoreOp_Store;
    attachment.clearValue = {.015, .02, .03, 1};
    auto descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &attachment;
    auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
    ImGui_ImplWGPU_RenderDrawData(data, pass);
    wgpuRenderPassEncoderEnd(pass);
    auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue_, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuRenderPassEncoderRelease(pass);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(frame.texture);
    // The browser presents the canvas automatically after the animation frame.
    return true;
}
WebGpuContext::Texture::~Texture() {
    if (view)
        wgpuTextureViewRelease(view);
    if (texture)
        wgpuTextureRelease(texture);
}
std::shared_ptr<WebGpuContext::Texture> WebGpuContext::upload(const Image& image) {
    if (!ready() || !image.width || !image.height ||
        image.rgba.size() != std::size_t(image.width) * image.height)
        throw Error("Invalid WebGPU texture upload");
    auto result = std::make_shared<Texture>();
    auto descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    descriptor.size = {image.width, image.height, 1};
    descriptor.mipLevelCount = descriptor.sampleCount = 1;
    result->texture = wgpuDeviceCreateTexture(device_, &descriptor);
    update(*result, image);
    result->view = wgpuTextureCreateView(result->texture, nullptr);
    return result;
}
void WebGpuContext::update(Texture& texture, const Image& image) {
    if (!texture.texture || image.width != wgpuTextureGetWidth(texture.texture) ||
        image.height != wgpuTextureGetHeight(texture.texture) ||
        image.rgba.size() != std::size_t(image.width) * image.height)
        throw Error("Invalid WebGPU texture update");
    auto destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = texture.texture;
    auto layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = image.width * 4;
    layout.rowsPerImage = image.height;
    const WGPUExtent3D size{image.width, image.height, 1};
    wgpuQueueWriteTexture(queue_, &destination, image.rgba.data(), image.rgba.size() * 4, &layout, &size);
}
} // namespace sacrifice
