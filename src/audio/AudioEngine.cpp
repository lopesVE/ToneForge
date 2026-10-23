#include "audio/AudioEngine.h"

#include <windows.h>
#include <xmmintrin.h>

#include <algorithm>
#include <cstring>

#include "engine/Rig.h"
#include "miniaudio.h"

namespace {
const ma_backend kBackends[] = {ma_backend_wasapi, ma_backend_dsound, ma_backend_winmm};
constexpr int kMaxChunk = 1024;
}  // namespace

struct AudioEngine::Impl {
    ma_context ctx{};
    bool ctxReady = false;
    int ctxBackend = -1;
    ma_device device{};
    bool deviceReady = false;
    std::vector<ma_device_info> inInfos, outInfos;

    Rig* rig = nullptr;
    int inputChannel = 0;
    float mono[kMaxChunk], L[kMaxChunk], R[kMaxChunk];
    double qpcToSec = 0;

    bool ensureContext(int backend) {
        if (ctxReady && ctxBackend == backend) return true;
        if (deviceReady) return false;
        if (ctxReady) { ma_context_uninit(&ctx); ctxReady = false; }
        ma_backend b = kBackends[std::clamp(backend, 0, 2)];
        ma_context_config cc = ma_context_config_init();
        if (ma_context_init(&b, 1, &cc, &ctx) != MA_SUCCESS) return false;
        ctxReady = true;
        ctxBackend = backend;
        return true;
    }

    static void callback(ma_device* dev, void* out, const void* in, ma_uint32 frames) {
        _mm_setcsr(_mm_getcsr() | 0x8040);  // flush denormals to zero
        auto* self = static_cast<Impl*>(dev->pUserData);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const float* src = static_cast<const float*>(in);
        float* dst = static_cast<float*>(out);
        const ma_uint32 inCh = dev->capture.channels;
        for (ma_uint32 done = 0; done < frames;) {
            const int n = int(std::min<ma_uint32>(kMaxChunk, frames - done));
            for (int i = 0; i < n; ++i) {
                const float* f = src + size_t(done + ma_uint32(i)) * inCh;
                if (inCh < 2) self->mono[i] = f[0];
                else if (self->inputChannel == 2) self->mono[i] = 0.5f * (f[0] + f[1]);
                else self->mono[i] = f[std::min<ma_uint32>(ma_uint32(self->inputChannel), inCh - 1)];
            }
            self->rig->process(self->mono, self->L, self->R, n);
            for (int i = 0; i < n; ++i) {
                dst[size_t(done + ma_uint32(i)) * 2] = self->L[i];
                dst[size_t(done + ma_uint32(i)) * 2 + 1] = self->R[i];
            }
            done += ma_uint32(n);
        }
        QueryPerformanceCounter(&t1);
        double used = double(t1.QuadPart - t0.QuadPart) * self->qpcToSec;
        double avail = double(frames) / dev->sampleRate;
        float load = float(used / avail);
        float prev = self->rig->cpuLoad.load(std::memory_order_relaxed);
        self->rig->cpuLoad.store(prev + (load - prev) * 0.05f, std::memory_order_relaxed);
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    impl_->qpcToSec = 1.0 / double(f.QuadPart);
}

AudioEngine::~AudioEngine() {
    stop();
    if (impl_->ctxReady) ma_context_uninit(&impl_->ctx);
}

const std::vector<std::string>& AudioEngine::backendNames() {
    static const std::vector<std::string> names = {"WASAPI", "DirectSound", "WinMM"};
    return names;
}

void AudioEngine::refreshDevices(int backend) {
    inputs_.clear();
    outputs_.clear();
    impl_->inInfos.clear();
    impl_->outInfos.clear();
    if (!impl_->ensureContext(backend)) return;
    ma_device_info *pOut = nullptr, *pIn = nullptr;
    ma_uint32 nOut = 0, nIn = 0;
    if (ma_context_get_devices(&impl_->ctx, &pOut, &nOut, &pIn, &nIn) != MA_SUCCESS) return;
    for (ma_uint32 i = 0; i < nIn; ++i) { impl_->inInfos.push_back(pIn[i]); inputs_.push_back(pIn[i].name); }
    for (ma_uint32 i = 0; i < nOut; ++i) { impl_->outInfos.push_back(pOut[i]); outputs_.push_back(pOut[i].name); }
}

bool AudioEngine::start(const AudioConfig& cfg, Rig& rig) {
    stop();
    status_ = AudioStatus{};
    if (!impl_->ensureContext(cfg.backend)) {
        status_.error = "Não consegui iniciar o backend de áudio.";
        return false;
    }
    if (impl_->inInfos.empty() && impl_->outInfos.empty()) refreshDevices(cfg.backend);

    ma_device_config dc = ma_device_config_init(ma_device_type_duplex);
    dc.sampleRate = ma_uint32(cfg.sampleRate);
    dc.periodSizeInFrames = ma_uint32(cfg.bufferSize);
    dc.periods = 2;
    dc.performanceProfile = ma_performance_profile_low_latency;
    dc.capture.format = ma_format_f32;
    dc.capture.channels = 2;
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.dataCallback = &Impl::callback;
    dc.pUserData = impl_.get();
    dc.noPreSilencedOutputBuffer = MA_TRUE;
    dc.noClip = MA_TRUE;
    dc.wasapi.noAutoConvertSRC = MA_TRUE;
    if (cfg.exclusive && cfg.backend == 0) {
        dc.capture.shareMode = ma_share_mode_exclusive;
        dc.playback.shareMode = ma_share_mode_exclusive;
    }
    for (auto& d : impl_->inInfos)
        if (cfg.inputName == d.name) dc.capture.pDeviceID = &d.id;
    for (auto& d : impl_->outInfos)
        if (cfg.outputName == d.name) dc.playback.pDeviceID = &d.id;

    impl_->rig = &rig;
    impl_->inputChannel = cfg.inputChannel;
    rig.prepare(double(cfg.sampleRate));
    ma_result r = ma_device_init(&impl_->ctx, &dc, &impl_->device);
    if (r != MA_SUCCESS) {
        status_.error = std::string("Falha ao abrir o dispositivo: ") + ma_result_description(r);
        return false;
    }
    impl_->deviceReady = true;
    if (impl_->device.sampleRate != ma_uint32(cfg.sampleRate)) rig.prepare(double(impl_->device.sampleRate));
    r = ma_device_start(&impl_->device);
    if (r != MA_SUCCESS) {
        status_.error = std::string("Falha ao iniciar o áudio: ") + ma_result_description(r);
        stop();
        return false;
    }
    status_.running = true;
    status_.sampleRate = int(impl_->device.sampleRate);
    ma_uint32 inPeriod = impl_->device.capture.internalPeriodSizeInFrames;
    ma_uint32 outPeriod = impl_->device.playback.internalPeriodSizeInFrames;
    ma_uint32 outPeriods = std::max<ma_uint32>(1, impl_->device.playback.internalPeriods);
    status_.bufferSize = int(outPeriod);
    status_.latencyMs = float(1000.0 * (inPeriod + outPeriod * outPeriods) / impl_->device.sampleRate);
    status_.inputName = impl_->device.capture.name;
    status_.outputName = impl_->device.playback.name;
    return true;
}

void AudioEngine::stop() {
    if (impl_->deviceReady) {
        ma_device_uninit(&impl_->device);
        impl_->deviceReady = false;
    }
    status_.running = false;
}
