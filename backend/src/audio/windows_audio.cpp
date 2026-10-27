#include "audio/audio.h"

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
#include <thread>
#include <utility>
#include <vector>

namespace molink {
namespace {

using Microsoft::WRL::ComPtr;

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

class wasapi_loopback final : public audio_source {
 public:
  bool available() const override { return available_; }
  audio_format format() const override { return format_; }

  /// Checks the default render endpoint and records its sample rate without
  /// keeping the device open.
  bool probe(std::string& err) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) {
      err = "could not open the audio client";
      return false;
    }
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr) {
      err = "could not read the playback format";
      return false;
    }
    const bool supported = is_float32(mix) || is_pcm16(mix);
    if (supported) {
      format_.sample_rate = static_cast<int>(mix->nSamplesPerSec);
      format_.channels = static_cast<int>(mix->nChannels);
      format_.bits = 16;
    }
    CoTaskMemFree(mix);
    if (!supported) {
      err = "unsupported playback format";
      return false;
    }
    available_ = true;
    return true;
  }

  bool start(sink callback, std::string& err) override {
    if (running_.load()) return true;
    if (!available_) {
      err = "no loopback audio device";
      return false;
    }
    if (!open(err)) return false;
    sink_ = std::move(callback);
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() override {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    close();
  }

 private:
  bool open(std::string& err) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
                          reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) {
      err = "could not open the audio client";
      return false;
    }
    hr = client_->GetMixFormat(&mix_);
    if (FAILED(hr) || mix_ == nullptr) {
      err = "could not read the playback format";
      return false;
    }
    if (!is_float32(mix_) && !is_pcm16(mix_)) {
      err = "unsupported playback format";
      return false;
    }
    format_.sample_rate = static_cast<int>(mix_->nSamplesPerSec);
    format_.channels = static_cast<int>(mix_->nChannels);
    format_.bits = 16;
    // 100 ms shared-mode buffer; the loopback client delivers whatever the
    // render engine has mixed since the last call.
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, mix_,
                             nullptr);
    if (FAILED(hr)) {
      err = "loopback initialization failed";
      return false;
    }
    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) {
      err = "could not open the loopback capture";
      return false;
    }
    hr = client_->Start();
    if (FAILED(hr)) {
      err = "could not start loopback capture";
      return false;
    }
    return true;
  }

  void close() {
    if (client_ != nullptr) client_->Stop();
    capture_.Reset();
    client_.Reset();
    if (mix_ != nullptr) {
      CoTaskMemFree(mix_);
      mix_ = nullptr;
    }
  }

  void run() {
    std::vector<uint8_t> pcm;
    while (running_.load()) {
      // The device can disappear under us: the user switches the output, the
      // audio service restarts, or the engine invalidates the stream. Losing
      // sound for the rest of the session because of one reset is far worse
      // than a short gap, so reopen with a backoff instead of giving up.
      if (capture_ == nullptr) {
        std::string open_error;
        if (!open(open_error)) {
          close();
          Sleep(reopen_delay_ms_);
          reopen_delay_ms_ = std::min(reopen_delay_ms_ * 2u, 2000u);
          continue;
        }
        reopen_delay_ms_ = 250;
      }
      const int channels = format_.channels;
      Sleep(10);
      UINT32 packet = 0;
      if (FAILED(capture_->GetNextPacketSize(&packet))) {
        close();
        continue;
      }
      while (packet != 0 && running_.load()) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(
                capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
          close();
          break;
        }
        if (frames > 0) {
          if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            pcm.assign(static_cast<std::size_t>(frames) * channels * 2, 0);
          } else if (is_float32(mix_)) {
            const auto* source = reinterpret_cast<const float*>(data);
            const std::size_t samples =
                static_cast<std::size_t>(frames) * channels;
            pcm.resize(samples * 2);
            for (std::size_t i = 0; i < samples; ++i) {
              const float value = std::clamp(source[i], -1.0f, 1.0f);
              const int16_t converted =
                  static_cast<int16_t>(value * 32767.0f);
              pcm[i * 2] = static_cast<uint8_t>(converted & 0xff);
              pcm[i * 2 + 1] =
                  static_cast<uint8_t>((converted >> 8) & 0xff);
            }
          } else {
            const std::size_t bytes =
                static_cast<std::size_t>(frames) * channels * 2;
            pcm.assign(data, data + bytes);
          }
          if (sink_) sink_(std::move(pcm));
        }
        capture_->ReleaseBuffer(frames);
        if (FAILED(capture_->GetNextPacketSize(&packet))) {
          close();
          break;
        }
      }
    }
  }

  bool available_ = false;
  audio_format format_;
  sink sink_;
  /// Wait before retrying a failed open; doubles up to two seconds.
  unsigned reopen_delay_ms_ = 250;
  std::atomic<bool> running_{false};
  std::thread thread_;
  ComPtr<IAudioClient> client_;
  ComPtr<IAudioCaptureClient> capture_;
  WAVEFORMATEX* mix_ = nullptr;
};

}  // namespace

std::unique_ptr<audio_source> make_audio_source(std::string& err) {
  auto source = std::make_unique<wasapi_loopback>();
  // Probe once at startup so the welcome can advertise the capability and the
  // exact sample rate before anyone asks for audio.
  if (!source->probe(err)) return nullptr;
  return source;
}

}  // namespace molink

#else

namespace molink {
std::unique_ptr<audio_source> make_audio_source(std::string& err) {
  err = "audio capture is not implemented on this platform";
  return nullptr;
}
}  // namespace molink

#endif
