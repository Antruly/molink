#include "capture/nvenc_av1_encoder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <ffnvcodec/nvEncodeAPI.h>

#if defined(_WIN32)
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#else
#include <dlfcn.h>
#endif

namespace molink {
namespace {

#if defined(_WIN32)
using nvenc_module = HMODULE;
constexpr const char* kEncodeLibNames[] = {"nvEncodeAPI64.dll",
                                           "nvEncodeAPI.dll"};
#else
using nvenc_module = void*;
constexpr const char* kEncodeLibNames[] = {"libnvidia-encode.so.1",
                                           "libnvidia-encode.so"};
#endif

const char* nvenc_status_name(NVENCSTATUS status) {
  switch (status) {
    case NV_ENC_SUCCESS:
      return "success";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:
      return "no encode device";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:
      return "unsupported device";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:
      return "invalid encoder device";
    case NV_ENC_ERR_INVALID_DEVICE:
      return "invalid device";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:
      return "device does not exist";
    case NV_ENC_ERR_INVALID_PTR:
      return "invalid pointer";
    case NV_ENC_ERR_INVALID_CALL:
      return "invalid call";
    case NV_ENC_ERR_OUT_OF_MEMORY:
      return "out of memory";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED:
      return "encoder not initialized";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:
      return "unsupported parameter";
    default:
      return "NVENC error";
  }
}

struct api_holder {
  nvenc_module module = nullptr;
  NV_ENCODE_API_FUNCTION_LIST fn{};

  bool load(std::string& error) {
#if defined(_WIN32)
    for (const char* name : kEncodeLibNames) {
      module = LoadLibraryA(name);
      if (module != nullptr) break;
    }
#else
    for (const char* name : kEncodeLibNames) {
      module = dlopen(name, RTLD_NOW | RTLD_LOCAL);
      if (module != nullptr) break;
    }
#endif
    if (module == nullptr) {
      error = "nvenc: the NVIDIA encode library is not installed";
      return false;
    }
#if defined(_WIN32)
    auto create = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(
        NV_ENCODE_API_FUNCTION_LIST*)>(
        GetProcAddress(module, "NvEncodeAPICreateInstance"));
#else
    auto create = reinterpret_cast<NVENCSTATUS(*)(
        NV_ENCODE_API_FUNCTION_LIST*)>(
        dlsym(module, "NvEncodeAPICreateInstance"));
#endif
    if (create == nullptr) {
      error = "nvenc: NvEncodeAPICreateInstance is missing";
      return false;
    }
    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    const NVENCSTATUS status = create(&fn);
    if (status != NV_ENC_SUCCESS) {
      error = std::string("nvenc: NvEncodeAPICreateInstance failed (") +
              nvenc_status_name(status) + ")";
      return false;
    }
    if (fn.nvEncOpenEncodeSessionEx == nullptr ||
        fn.nvEncGetEncodeGUIDs == nullptr ||
        fn.nvEncInitializeEncoder == nullptr) {
      error = "nvenc: the driver is too old (missing session API)";
      return false;
    }
    return true;
  }

  ~api_holder() {
    if (module == nullptr) return;
#if defined(_WIN32)
    FreeLibrary(module);
#else
    dlclose(module);
#endif
  }
};

#if defined(_WIN32)
struct nvenc_device {
  ID3D11Device* d3d = nullptr;
  std::string name;
  bool create(std::string& error) {
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    const HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
        D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(result) || d3d == nullptr) {
      error = "nvenc: could not create a D3D11 device";
      return false;
    }
    IDXGIDevice* dxgi = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(IID_IDXGIDevice,
                                      reinterpret_cast<void**>(&dxgi))) &&
        dxgi != nullptr) {
      IDXGIAdapter* adapter = nullptr;
      if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && adapter != nullptr) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
          char utf8[256] = {};
          WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, utf8,
                              sizeof(utf8) - 1, nullptr, nullptr);
          name = utf8;
        }
        adapter->Release();
      }
      dxgi->Release();
    }
    if (name.empty()) name = "NVIDIA NVENC";
    return true;
  }
  void* handle() const { return d3d; }
  NV_ENC_DEVICE_TYPE type() const { return NV_ENC_DEVICE_TYPE_DIRECTX; }
  void destroy() {
    if (d3d != nullptr) {
      d3d->Release();
      d3d = nullptr;
    }
  }
  ~nvenc_device() { destroy(); }
};
#else
struct nvenc_device {
  void* cuda = nullptr;
  void* context = nullptr;
  std::string name = "NVIDIA NVENC";
  void* cu_ctx_destroy = nullptr;

  bool create(std::string& error) {
    cuda = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (cuda == nullptr) {
      error = "nvenc: libcuda.so.1 is not available";
      return false;
    }
    using cu_init_fn = int (*)(unsigned int);
    using cu_device_get_fn = int (*)(int*, int);
    using cu_ctx_create_fn = int (*)(void**, unsigned int, int);
    auto cu_init = reinterpret_cast<cu_init_fn>(dlsym(cuda, "cuInit"));
    auto cu_device_get =
        reinterpret_cast<cu_device_get_fn>(dlsym(cuda, "cuDeviceGet"));
    auto cu_ctx_create = reinterpret_cast<cu_ctx_create_fn>(
        dlsym(cuda, "cuCtxCreate_v2"));
    if (cu_init == nullptr || cu_device_get == nullptr ||
        cu_ctx_create == nullptr) {
      error = "nvenc: the CUDA driver API is incomplete";
      return false;
    }
    if (cu_init(0) != 0) {
      error = "nvenc: cuInit failed";
      return false;
    }
    int device = 0;
    if (cu_device_get(&device, 0) != 0 ||
        cu_ctx_create(&context, 0, device) != 0 ||
        context == nullptr) {
      error = "nvenc: could not create a CUDA context";
      return false;
    }
    cu_ctx_destroy = dlsym(cuda, "cuCtxDestroy_v2");
    return true;
  }
  void* handle() const { return context; }
  NV_ENC_DEVICE_TYPE type() const { return NV_ENC_DEVICE_TYPE_CUDA; }
  void destroy() {
    using cu_ctx_destroy_fn = int (*)(void*);
    if (context != nullptr && cu_ctx_destroy != nullptr) {
      reinterpret_cast<cu_ctx_destroy_fn>(cu_ctx_destroy)(context);
      context = nullptr;
    }
    if (cuda != nullptr) {
      dlclose(cuda);
      cuda = nullptr;
    }
  }
  ~nvenc_device() { destroy(); }
};
#endif

bool same_guid(const GUID& a, const GUID& b) {
  return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

struct probe_result {
  bool available = false;
  std::string detail;
};

probe_result probe_nvenc() {
  probe_result result;
  api_holder api;
  std::string error;
  if (!api.load(error)) return result;
  nvenc_device device;
  if (!device.create(error)) return result;

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
  params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  params.deviceType = device.type();
  params.device = device.handle();
  params.apiVersion = NVENCAPI_VERSION;
  void* session = nullptr;
  const NVENCSTATUS opened =
      api.fn.nvEncOpenEncodeSessionEx(&params, &session);
  if (opened != NV_ENC_SUCCESS || session == nullptr) return result;

  uint32_t count = 0;
  bool av1 = false;
  if (api.fn.nvEncGetEncodeGUIDCount(session, &count) == NV_ENC_SUCCESS &&
      count > 0 && count < 64) {
    std::vector<GUID> guids(count);
    uint32_t written = 0;
    if (api.fn.nvEncGetEncodeGUIDs(session, guids.data(), count, &written) ==
        NV_ENC_SUCCESS) {
      for (uint32_t i = 0; i < written; ++i) {
        if (same_guid(guids[i], NV_ENC_CODEC_AV1_GUID)) {
          av1 = true;
          break;
        }
      }
    }
  }
  api.fn.nvEncDestroyEncoder(session);
  if (!av1) return result;
  result.available = true;
  result.detail = device.name;
  return result;
}

const probe_result& cached_probe() {
  static const probe_result result = probe_nvenc();
  return result;
}

GUID preset_guid_for(int preset) {
  switch (preset) {
    case 1:
      return NV_ENC_PRESET_P1_GUID;
    case 2:
      return NV_ENC_PRESET_P2_GUID;
    case 3:
      return NV_ENC_PRESET_P3_GUID;
    case 4:
      return NV_ENC_PRESET_P4_GUID;
    case 5:
      return NV_ENC_PRESET_P5_GUID;
    case 6:
      return NV_ENC_PRESET_P6_GUID;
    default:
      return NV_ENC_PRESET_P7_GUID;
  }
}

inline uint8_t clamp_u8(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

class nvenc_av1_encoder final : public av1_encoder {
 public:
  explicit nvenc_av1_encoder(av1_encoder_config config)
      : config_(std::move(config)) {}
  ~nvenc_av1_encoder() override { shutdown(); }

  bool encode_intra(const uint8_t* bgra, std::size_t stride, int width,
                    int height, int qp, std::vector<uint8_t>& out,
                    std::string& error) override {
    out.clear();
    if (bgra == nullptr || width <= 0 || height <= 0) {
      error = "nvenc: invalid rectangle";
      return false;
    }
    if (stride < static_cast<std::size_t>(width) * 4) {
      error = "nvenc: stride is shorter than a row";
      return false;
    }
    if (!open(error)) return false;
    // NV12 4:2:0 needs even dimensions. The decoded picture is padded; the
    // receiver crops back to the tile's real w/h.
    const int padded_w = (std::max)(2, (width + 1) & ~1);
    const int padded_h = (std::max)(2, (height + 1) & ~1);
    const int nv_qp = std::clamp(qp, 1, 63) * 4;
    if (padded_w != width_ || padded_h != height_ || nv_qp != qp_) {
      if (!init(padded_w, padded_h, nv_qp, error)) return false;
    }

    NV_ENC_LOCK_INPUT_BUFFER lock{};
    lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lock.inputBuffer = input_;
    NVENCSTATUS status = api_.fn.nvEncLockInputBuffer(session_, &lock);
    if (status != NV_ENC_SUCCESS) {
      error = std::string("nvenc: lock input failed (") +
              nvenc_status_name(status) + ")";
      return false;
    }
    fill_nv12(static_cast<uint8_t*>(lock.bufferDataPtr), lock.pitch, bgra,
              stride, width, height, padded_w, padded_h);
    api_.fn.nvEncUnlockInputBuffer(session_, input_);

    NV_ENC_PIC_PARAMS picture{};
    picture.version = NV_ENC_PIC_PARAMS_VER;
    picture.inputWidth = static_cast<uint32_t>(padded_w);
    picture.inputHeight = static_cast<uint32_t>(padded_h);
    picture.inputPitch = lock.pitch;
    picture.inputBuffer = input_;
    picture.outputBitstream = bitstream_;
    picture.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picture.pictureType = NV_ENC_PIC_TYPE_IDR;
    picture.encodePicFlags =
        NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    picture.inputTimeStamp = ++frame_index_;
    status = api_.fn.nvEncEncodePicture(session_, &picture);
    if (status != NV_ENC_SUCCESS) {
      error = std::string("nvenc: encode failed (") +
              nvenc_status_name(status) + ")";
      return false;
    }

    NV_ENC_LOCK_BITSTREAM bits{};
    bits.version = NV_ENC_LOCK_BITSTREAM_VER;
    bits.outputBitstream = bitstream_;
    bits.doNotWait = 0;
    status = api_.fn.nvEncLockBitstream(session_, &bits);
    if (status != NV_ENC_SUCCESS) {
      error = std::string("nvenc: lock bitstream failed (") +
              nvenc_status_name(status) + ")";
      return false;
    }
    if (bits.bitstreamBufferPtr != nullptr && bits.bitstreamSizeInBytes > 0) {
      const auto* bytes =
          static_cast<const uint8_t*>(bits.bitstreamBufferPtr);
      out.assign(bytes, bytes + bits.bitstreamSizeInBytes);
    }
    api_.fn.nvEncUnlockBitstream(session_, bitstream_);
    if (out.empty()) {
      error = "nvenc: the encoder produced an empty bitstream";
      return false;
    }
    return true;
  }

 private:
  bool open(std::string& error) {
    if (session_ != nullptr) return true;
    if (!api_.load(error)) return false;
    if (!device_.create(error)) return false;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = device_.type();
    params.device = device_.handle();
    params.apiVersion = NVENCAPI_VERSION;
    const NVENCSTATUS status =
        api_.fn.nvEncOpenEncodeSessionEx(&params, &session_);
    if (status != NV_ENC_SUCCESS || session_ == nullptr) {
      error = std::string("nvenc: open session failed (") +
              nvenc_status_name(status) + ")";
      session_ = nullptr;
      return false;
    }
    return true;
  }

  bool init(int width, int height, int qp, std::string& error) {
    shutdown_buffers();
    if (session_ != nullptr) {
      api_.fn.nvEncDestroyEncoder(session_);
      session_ = nullptr;
    }
    if (!open(error)) return false;

    const int preset = nvenc_preset_for(config_.speed);
    const GUID preset_guid = preset_guid_for(preset);
    NV_ENC_PRESET_CONFIG preset_config{};
    preset_config.version = NV_ENC_PRESET_CONFIG_VER;
    preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
    NVENCSTATUS status = NV_ENC_ERR_INVALID_CALL;
    if (api_.fn.nvEncGetEncodePresetConfigEx != nullptr) {
      status = api_.fn.nvEncGetEncodePresetConfigEx(
          session_, NV_ENC_CODEC_AV1_GUID, preset_guid,
          NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset_config);
    }
    if (status != NV_ENC_SUCCESS &&
        api_.fn.nvEncGetEncodePresetConfig != nullptr) {
      status = api_.fn.nvEncGetEncodePresetConfig(
          session_, NV_ENC_CODEC_AV1_GUID, preset_guid, &preset_config);
    }
    if (status != NV_ENC_SUCCESS) {
      error = std::string("nvenc: preset config failed (") +
              nvenc_status_name(status) + ")";
      return false;
    }

    NV_ENC_CONFIG config = preset_config.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
    config.gopLength = 1;
    config.frameIntervalP = 1;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    config.rcParams.constQP.qpIntra = static_cast<uint32_t>(qp);
    config.rcParams.constQP.qpInterP = static_cast<uint32_t>(qp);
    config.rcParams.constQP.qpInterB = static_cast<uint32_t>(qp);

    NV_ENC_CONFIG_AV1& av1 = config.encodeCodecConfig.av1Config;
    av1.chromaFormatIDC = 1;
    av1.repeatSeqHdr = 1;
    av1.disableSeqHdr = 0;
    av1.enableIntraRefresh = 0;
    av1.idrPeriod = 1;
    av1.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    av1.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    av1.colorRange = 1;  // full range, matching the SVT path
    av1.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    av1.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    av1.numTileColumns = 0;
    av1.numTileRows = 0;

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_AV1_GUID;
    init.presetGUID = preset_guid;
    init.encodeWidth = static_cast<uint32_t>(width);
    init.encodeHeight = static_cast<uint32_t>(height);
    init.frameRateNum = 30;
    init.frameRateDen = 1;
    init.enablePTD = 0;
    init.enableEncodeAsync = 0;
    init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    init.encodeConfig = &config;
    status = api_.fn.nvEncInitializeEncoder(session_, &init);
    if (status != NV_ENC_SUCCESS) {
      error = std::string("nvenc: initialize failed (") +
              nvenc_status_name(status) + ")";
      return false;
    }

    NV_ENC_CREATE_INPUT_BUFFER input{};
    input.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    input.width = static_cast<uint32_t>(width);
    input.height = static_cast<uint32_t>(height);
    input.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    status = api_.fn.nvEncCreateInputBuffer(session_, &input);
    if (status != NV_ENC_SUCCESS) {
      error = std::string("nvenc: create input buffer failed (") +
              nvenc_status_name(status) + ")";
      return false;
    }
    input_ = input.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER output{};
    output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    output.size = static_cast<uint32_t>(width) * height * 4 + (1u << 20);
    status = api_.fn.nvEncCreateBitstreamBuffer(session_, &output);
    if (status != NV_ENC_SUCCESS) {
      error = std::string("nvenc: create bitstream buffer failed (") +
              nvenc_status_name(status) + ")";
      return false;
    }
    bitstream_ = output.bitstreamBuffer;

    width_ = width;
    height_ = height;
    qp_ = qp;
    return true;
  }

  static void fill_nv12(uint8_t* data, uint32_t pitch, const uint8_t* bgra,
                        std::size_t stride, int width, int height,
                        int padded_w, int padded_h) {
    uint8_t* y_plane = data;
    uint8_t* uv_plane = data + static_cast<std::size_t>(pitch) * padded_h;
    for (int y = 0; y < padded_h; ++y) {
      const int src_y = (std::min)(y, height - 1);
      const uint8_t* row =
          bgra + static_cast<std::size_t>(src_y) * stride;
      uint8_t* y_row = y_plane + static_cast<std::size_t>(y) * pitch;
      for (int x = 0; x < padded_w; ++x) {
        const int src_x = (std::min)(x, width - 1);
        const uint8_t* px = row + static_cast<std::size_t>(src_x) * 4;
        y_row[x] = clamp_u8(
            (54 * px[2] + 183 * px[1] + 18 * px[0] + 128) >> 8);
      }
    }
    for (int y = 0; y < padded_h / 2; ++y) {
      uint8_t* uv_row =
          uv_plane + static_cast<std::size_t>(y) * pitch;
      for (int x = 0; x < padded_w / 2; ++x) {
        int cb_sum = 0;
        int cr_sum = 0;
        for (int dy = 0; dy < 2; ++dy) {
          const int src_y = (std::min)(y * 2 + dy, height - 1);
          const uint8_t* row =
              bgra + static_cast<std::size_t>(src_y) * stride;
          for (int dx = 0; dx < 2; ++dx) {
            const int src_x = (std::min)(x * 2 + dx, width - 1);
            const uint8_t* px = row + static_cast<std::size_t>(src_x) * 4;
            cb_sum += ((-29 * px[2] - 99 * px[1] + 128 * px[0] + 128) >> 8) +
                      128;
            cr_sum += ((128 * px[2] - 116 * px[1] - 12 * px[0] + 128) >> 8) +
                      128;
          }
        }
        uv_row[x * 2] = clamp_u8(cb_sum / 4);
        uv_row[x * 2 + 1] = clamp_u8(cr_sum / 4);
      }
    }
  }

  void shutdown_buffers() {
    if (session_ == nullptr) return;
    if (bitstream_ != nullptr) {
      api_.fn.nvEncDestroyBitstreamBuffer(session_, bitstream_);
      bitstream_ = nullptr;
    }
    if (input_ != nullptr) {
      api_.fn.nvEncDestroyInputBuffer(session_, input_);
      input_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
    qp_ = 0;
  }

  void shutdown() {
    shutdown_buffers();
    if (session_ != nullptr && api_.module != nullptr) {
      api_.fn.nvEncDestroyEncoder(session_);
      session_ = nullptr;
    }
    device_.destroy();
  }

  av1_encoder_config config_;
  api_holder api_;
  nvenc_device device_;
  void* session_ = nullptr;
  NV_ENC_INPUT_PTR input_ = nullptr;
  NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int qp_ = 0;
  uint64_t frame_index_ = 0;
};

}  // namespace

int nvenc_preset_for(const std::string& speed) {
  if (speed == "fast") return 1;
  if (speed == "quality") return 5;
  if (speed == "balanced") return 3;
  return 3;  // auto
}

bool nvenc_av1_available() { return cached_probe().available; }

std::vector<av1_encoder_device> nvenc_av1_devices() {
  if (!nvenc_av1_available()) return {};
  av1_encoder_device device;
  device.id = "gpu-nvenc";
  device.name = "GPU · NVIDIA NVENC";
  device.hardware = true;
  device.available = true;
  device.detail = cached_probe().detail;
  return {device};
}

std::shared_ptr<av1_encoder> make_nvenc_av1_encoder(
    const av1_encoder_config& config, std::string* error) {
  if (error != nullptr) error->clear();
  if (!nvenc_av1_available()) {
    if (error != nullptr) {
      *error = "nvenc: no AV1-capable NVIDIA encoder was detected";
    }
    return nullptr;
  }
  return std::make_shared<nvenc_av1_encoder>(config);
}

}  // namespace molink
