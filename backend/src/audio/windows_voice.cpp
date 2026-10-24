#include "audio/voice.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace molink {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kWireRate = 48000;
constexpr int kWireChannels = 1;

bool is_float32(const WAVEFORMATEX* format) {
  if (format == nullptr) return false;
  if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return format->wBitsPerSample == 32;
  }
  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* extended =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
           format->wBitsPerSample == 32;
  }
  return false;
}

bool is_pcm16(const WAVEFORMATEX* format) {
  if (format == nullptr) return false;
  if (format->wFormatTag == WAVE_FORMAT_PCM) {
    return format->wBitsPerSample == 16;
  }
  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* extended =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
           format->wBitsPerSample == 16;
  }
  return false;
}

class wasapi_voice final : public voice_engine {
 public:
  bool microphone_available() const override { return mic_available_; }

  bool start(sink capture, std::string& err) override {
    stop();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!open_render(err)) return false;
    bool mic_open = false;
    if (mic_available_) {
      std::string mic_error;
      mic_open = open_capture(mic_error);
      if (!mic_open) mic_available_ = false;
    }
    capture_ = std::move(capture);
    running_.store(true);
    render_thread_ = std::thread([this] { render_loop(); });
    if (mic_open) mic_thread_ = std::thread([this] { mic_loop(); });
    return true;
  }

  void play(const uint8_t* pcm, std::size_t len) override {
    if (pcm == nullptr || len < 2) return;
    const std::size_t samples = len / 2;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    for (std::size_t i = 0; i < samples; ++i) {
      int16_t value = 0;
      std::memcpy(&value, pcm + i * 2, 2);
      queue_.push_back(value);
    }
    // Never let the speaker backlog grow past two seconds; a slow render
    // device must not turn the call into an echo of its own past.
    while (queue_.size() > static_cast<std::size_t>(kWireRate * 2)) {
      queue_.pop_front();
    }
  }

  void stop() override {
    running_.store(false);
    if (render_thread_.joinable()) render_thread_.join();
    if (mic_thread_.joinable()) mic_thread_.join();
    if (render_client_ != nullptr) render_client_->Stop();
    if (mic_client_ != nullptr) mic_client_->Stop();
    render_.Reset();
    render_client_.Reset();
    mic_capture_.Reset();
    mic_client_.Reset();
    if (render_format_ != nullptr) {
      CoTaskMemFree(render_format_);
      render_format_ = nullptr;
    }
    if (mic_format_ != nullptr) {
      CoTaskMemFree(mic_format_);
      mic_format_ = nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      queue_.clear();
    }
    capture_ = nullptr;
  }

  bool probe_microphone(std::string& err) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
      err = "could not create the audio device enumerator";
      return false;
    }
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    if (FAILED(hr)) {
      err = "no default microphone";
      return false;
    }
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) {
      err = "could not open the microphone";
      return false;
    }
    WAVEFORMATEX* format = nullptr;
    hr = client->GetMixFormat(&format);
    if (FAILED(hr) || format == nullptr) {
      err = "could not read the microphone format";
      return false;
    }
    const bool supported = (is_float32(format) || is_pcm16(format)) &&
                           format->nSamplesPerSec == kWireRate;
    CoTaskMemFree(format);
    if (!supported) {
      err = "microphone format is not 48 kHz float/16-bit";
      return false;
    }
    mic_available_ = true;
    return true;
  }

 private:
  bool open_render(std::string& err) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
      err = "could not create the audio device enumerator";
      return false;
    }
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
      err = "no default playback device";
      return false;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(
                              render_client_.GetAddressOf()));
    if (FAILED(hr)) {
      err = "could not open the playback device";
      return false;
    }
    hr = render_client_->GetMixFormat(&render_format_);
    if (FAILED(hr) || render_format_ == nullptr) {
      err = "could not read the playback format";
      return false;
    }
    if ((!is_float32(render_format_) && !is_pcm16(render_format_)) ||
        render_format_->nSamplesPerSec != kWireRate) {
      err = "playback format is not 48 kHz float/16-bit";
      return false;
    }
    hr = render_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0,
                                    render_format_, nullptr);
    if (FAILED(hr)) {
      err = "playback initialization failed";
      return false;
    }
    hr = render_client_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(hr)) {
      err = "could not open the playback stream";
      return false;
    }
    hr = render_client_->Start();
    if (FAILED(hr)) {
      err = "could not start playback";
      return false;
    }
    return true;
  }

  bool open_capture(std::string& err) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
      err = "could not create the audio device enumerator";
      return false;
    }
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    if (FAILED(hr)) {
      err = "no default microphone";
      return false;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(mic_client_.GetAddressOf()));
    if (FAILED(hr)) {
      err = "could not open the microphone";
      return false;
    }
    hr = mic_client_->GetMixFormat(&mic_format_);
    if (FAILED(hr) || mic_format_ == nullptr) {
      err = "could not read the microphone format";
      return false;
    }
    if ((!is_float32(mic_format_) && !is_pcm16(mic_format_)) ||
        mic_format_->nSamplesPerSec != kWireRate) {
      err = "microphone format is not 48 kHz float/16-bit";
      return false;
    }
    hr = mic_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0,
                                 mic_format_, nullptr);
    if (FAILED(hr)) {
      err = "microphone initialization failed";
      return false;
    }
    hr = mic_client_->GetService(IID_PPV_ARGS(&mic_capture_));
    if (FAILED(hr)) {
      err = "could not open the microphone stream";
      return false;
    }
    hr = mic_client_->Start();
    if (FAILED(hr)) {
      err = "could not start the microphone";
      return false;
    }
    return true;
  }

  void render_loop() {
    const int channels = static_cast<int>(render_format_->nChannels);
    const bool is_float = is_float32(render_format_);
    UINT32 buffer_frames = 0;
    if (FAILED(render_client_->GetBufferSize(&buffer_frames))) return;
    while (running_.load()) {
      UINT32 padding = 0;
      if (FAILED(render_client_->GetCurrentPadding(&padding))) break;
      const UINT32 available = buffer_frames - padding;
      if (available == 0) {
        Sleep(5);
        continue;
      }
      BYTE* data = nullptr;
      if (FAILED(render_->GetBuffer(available, &data)) || data == nullptr) {
        break;
      }
      std::vector<int16_t> samples;
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        const std::size_t take =
            std::min<std::size_t>(available, queue_.size());
        samples.resize(take);
        for (std::size_t i = 0; i < take; ++i) {
          samples[i] = queue_.front();
          queue_.pop_front();
        }
      }
      for (UINT32 frame = 0; frame < available; ++frame) {
        const int16_t value =
            frame < samples.size() ? samples[frame] : static_cast<int16_t>(0);
        if (is_float) {
          auto* out = reinterpret_cast<float*>(data);
          const float normalized = static_cast<float>(value) / 32768.0f;
          for (int channel = 0; channel < channels; ++channel) {
            out[frame * channels + channel] = normalized;
          }
        } else {
          auto* out = reinterpret_cast<int16_t*>(data);
          for (int channel = 0; channel < channels; ++channel) {
            out[frame * channels + channel] = value;
          }
        }
      }
      render_->ReleaseBuffer(available, 0);
      Sleep(5);
    }
  }

  void mic_loop() {
    const int channels = static_cast<int>(mic_format_->nChannels);
    const bool is_float = is_float32(mic_format_);
    std::vector<uint8_t> packet;
    packet.reserve(kWireRate / 50 * 2);
    while (running_.load()) {
      Sleep(10);
      UINT32 available = 0;
      if (mic_capture_ == nullptr ||
          FAILED(mic_capture_->GetNextPacketSize(&available))) {
        break;
      }
      while (available != 0 && running_.load()) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(mic_capture_->GetBuffer(&data, &frames, &flags, nullptr,
                                           nullptr))) {
          break;
        }
        for (UINT32 frame = 0; frame < frames; ++frame) {
          float mixed = 0.0f;
          if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data != nullptr) {
            for (int channel = 0; channel < channels; ++channel) {
              if (is_float) {
                mixed += reinterpret_cast<const float*>(data)
                             [frame * channels + channel] /
                         static_cast<float>(channels);
              } else {
                mixed += reinterpret_cast<const int16_t*>(data)
                             [frame * channels + channel] /
                         (32768.0f * static_cast<float>(channels));
              }
            }
          }
          const float clamped = std::clamp(mixed, -1.0f, 1.0f);
          const int16_t sample = static_cast<int16_t>(clamped * 32767.0f);
          packet.push_back(static_cast<uint8_t>(sample & 0xff));
          packet.push_back(static_cast<uint8_t>((sample >> 8) & 0xff));
        }
        mic_capture_->ReleaseBuffer(frames);
        if (packet.size() >= static_cast<std::size_t>(kWireRate / 50 * 2)) {
          if (capture_) capture_(std::move(packet));
          packet.clear();
          packet.reserve(kWireRate / 50 * 2);
        }
        if (FAILED(mic_capture_->GetNextPacketSize(&available))) break;
      }
    }
    if (!packet.empty() && capture_) capture_(std::move(packet));
  }

  bool mic_available_ = false;
  std::atomic<bool> running_{false};
  sink capture_;
  std::thread render_thread_;
  std::thread mic_thread_;
  ComPtr<IAudioClient> render_client_;
  ComPtr<IAudioRenderClient> render_;
  ComPtr<IAudioClient> mic_client_;
  ComPtr<IAudioCaptureClient> mic_capture_;
  WAVEFORMATEX* render_format_ = nullptr;
  WAVEFORMATEX* mic_format_ = nullptr;
  std::mutex queue_mutex_;
  std::deque<int16_t> queue_;
};

}  // namespace

std::unique_ptr<voice_engine> make_voice_engine(std::string& err) {
  auto engine = std::make_unique<wasapi_voice>();
  engine->probe_microphone(err);
  return engine;
}

}  // namespace molink

#else

namespace molink {
std::unique_ptr<voice_engine> make_voice_engine(std::string& err) {
  err = "voice calls are not implemented on this platform";
  return nullptr;
}
}  // namespace molink

#endif
