// SPDX-License-Identifier: GPL-3.0-or-later
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include "audio_core/sink/apple_sink.h"
#include "audio_core/sink/null_sink.h"
#include "common/logging.h"

namespace AudioCore::Sink {
namespace {
class AppleStream final : public SinkStream {
    struct Gate { std::mutex mutex; AppleStream* stream{}; };
public:
    AppleStream(Core::System& system, u32 channels, StreamType type)
        : SinkStream(system, type), gate(std::make_shared<Gate>()) {
        system_channels = channels;
        device_channels = 2;
        gate->stream = this;
        AudioStreamBasicDescription format{};
        format.mSampleRate = TargetSampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        format.mBytesPerPacket = format.mBytesPerFrame = 2 * sizeof(s16);
        format.mFramesPerPacket = 1;
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 16;
        OSStatus status = AudioQueueNewOutput(&format, Callback, this, nullptr, nullptr, 0, &queue);
        if (status != noErr) {
            LOG_ERROR(Audio_Sink, "AudioQueueNewOutput failed: {}", status);
            queue = nullptr;
            return;
        }
        for (auto& buffer : buffers) {
            status = AudioQueueAllocateBuffer(queue, BufferBytes, &buffer);
            if (status != noErr) {
                LOG_ERROR(Audio_Sink, "AudioQueueAllocateBuffer failed: {}", status);
                AudioQueueDispose(queue, true);
                queue = nullptr;
                return;
            }
        }
        auto state = gate;
        observer = [NSNotificationCenter.defaultCenter
            addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil
            usingBlock:^(NSNotification* notification) {
                std::lock_guard lock(state->mutex);
                auto* stream = state->stream;
                if (!stream || !stream->queue) return;
                auto kind = [notification.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
                if (kind == AVAudioSessionInterruptionTypeBegan) {
                    stream->interrupted = true;
                    stream->SignalPause();
                    AudioQueuePause(stream->queue);
                } else {
                    stream->interrupted = false;
                    auto options = [notification.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
                    if (stream->wanted && (options & AVAudioSessionInterruptionOptionShouldResume))
                        stream->StartLocked();
                }
            }];
    }
    ~AppleStream() override { Finalize(); }
    void Finalize() override {
        std::lock_guard lock(gate->mutex);
        gate->stream = nullptr;
        if (observer) { [NSNotificationCenter.defaultCenter removeObserver:observer]; observer = nil; }
        wanted = false;
        SignalPause();
        if (queue) {
            // Immediate disposal waits for the queue callback before releasing this stream.
            AudioQueueDispose(queue, true);
            queue = nullptr;
        }
    }
    void Start(bool = false) override {
        std::lock_guard lock(gate->mutex);
        wanted = true;
        if (!interrupted) StartLocked();
    }
    void Stop() override {
        std::lock_guard lock(gate->mutex);
        wanted = false;
        SignalPause();
        if (queue) AudioQueuePause(queue);
    }
private:
    void StartLocked() {
        if (!queue || !paused) return;
        NSError* error = nil;
        auto* session = AVAudioSession.sharedInstance;
        if (![session setCategory:AVAudioSessionCategoryPlayback error:&error] ||
            ![session setActive:YES error:&error]) {
            LOG_ERROR(Audio_Sink, "iOS audio session failed: {}", error.localizedDescription.UTF8String);
            return;
        }
        if (!primed) {
            for (auto buffer : buffers) {
                std::memset(buffer->mAudioData, 0, BufferBytes);
                buffer->mAudioDataByteSize = BufferBytes;
                auto result = AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
                if (result != noErr) {
                    LOG_ERROR(Audio_Sink, "AudioQueue initial enqueue failed: {}", result);
                    AudioQueueReset(queue);
                    return;
                }
            }
            primed = true;
        }
        paused = false;
        auto result = AudioQueueStart(queue, nullptr);
        if (result != noErr) {
            SignalPause();
            LOG_ERROR(Audio_Sink, "AudioQueueStart failed: {}", result);
        }
    }
    static void Callback(void* context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
        auto* stream = static_cast<AppleStream*>(context);
        if (stream->paused) {
            std::memset(buffer->mAudioData, 0, BufferBytes);
        } else {
            stream->ProcessAudioOutAndRender(
                {static_cast<s16*>(buffer->mAudioData), Frames * 2}, Frames);
        }
        buffer->mAudioDataByteSize = BufferBytes;
        // Requeue even when paused so an in-flight callback cannot lose a buffer on resume.
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
    static constexpr UInt32 Frames = 512;
    static constexpr UInt32 BufferBytes = Frames * 2 * sizeof(s16);
    AudioQueueRef queue{};
    std::array<AudioQueueBufferRef, 3> buffers{};
    std::shared_ptr<Gate> gate;
    id observer = nil;
    bool wanted{}, interrupted{}, primed{};
};
class AppleSink final : public Sink {
public:
    SinkStream* AcquireSinkStream(Core::System& system, u32 channels,
                                const std::string&, StreamType type) override {
        system_channels = channels;
        if (type == StreamType::In) {
            LOG_WARNING(Audio_Sink, "iOS microphone capture is not supported");
            streams.push_back(std::make_unique<NullSinkStreamImpl>(system, type));
        } else {
            streams.push_back(std::make_unique<AppleStream>(system, channels, type));
        }
        streams.back()->SetDeviceVolume(device_volume);
        streams.back()->SetSystemVolume(system_volume);
        return streams.back().get();
    }
    void CloseStream(SinkStream* stream) override {
        std::erase_if(streams, [stream](const auto& candidate) { return candidate.get() == stream; });
    }
    void CloseStreams() override { streams.clear(); }
    f32 GetDeviceVolume() const override { return device_volume; }
    void SetDeviceVolume(f32 volume) override {
        device_volume = volume;
        for (auto& stream : streams) stream->SetDeviceVolume(volume);
    }
    void SetSystemVolume(f32 volume) override {
        system_volume = volume;
        for (auto& stream : streams) stream->SetSystemVolume(volume);
    }
private:
    std::vector<SinkStreamPtr> streams;
    f32 device_volume{1.0f}, system_volume{1.0f};
};
}
std::unique_ptr<Sink> CreateAppleSink() { return std::make_unique<AppleSink>(); }
}
