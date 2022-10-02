
#include <stdio.h>
#include "common_audio/signal_processing/include/signal_processing_library.h"
#include "neteq/expand.h"
#include "neteq/merge.h"
#include "neteq/audio_multi_vector.h"
#include "neteq/sync_buffer.h"
#include "neteq/background_noise.h"
#include "neteq/random_vector.h"
#include "neteq/post_decode_vad.h"
#include "rtc_base/audio_frame.h"
#include "neteq/tick_timer.h"
#include "neteq/accelerate.h"
#include "neteq/buffer_level_filter.h"
#include "neteq/normal.h"
#include "neteq/preemptive_expand.h"
#include "neteq/delay_peak_detector.h"
#include "neteq/delay_manager.h"
#include "neteq/packet_buffer.h"

#define kMode_Normal 0
#define kMode_Expand 1
#define kMode_Merge  2
using namespace webrtc;

static const int kInputSizeMs = 50;
static const int kOutputSizeMs = 30;
static const size_t kMaxFrameSize = 5760;  // 120 ms @ 48 kHz.
static const size_t kSyncBufferSize = kMaxFrameSize + 60 * 48;
static const size_t kMaxPacketsInBuffer = 100;

RandomVector random_vector_;
std::unique_ptr<PostDecodeVad> vad_;
std::unique_ptr<BackgroundNoise> background_noise_;
std::unique_ptr<ExpandFactory> expand_factory_;
std::unique_ptr<Expand> expand_;
std::unique_ptr<Merge> merge_;
std::unique_ptr<Normal> normal_;
std::unique_ptr<AccelerateFactory> accelerate_factory_;
std::unique_ptr<Accelerate> accelerate_;
std::unique_ptr<PreemptiveExpand> preemptive_expand_;
std::unique_ptr<PreemptiveExpandFactory> preemptive_expand_factory_;

std::unique_ptr<TickTimer> tick_timer_;
std::unique_ptr<DelayManager> delay_manager_;
std::unique_ptr<DelayPeakDetector> delay_peak_detector_;
std::unique_ptr<BufferLevelFilter> buffer_level_filter_;

std::unique_ptr<PacketBuffer> packet_buffer_;
std::unique_ptr<int16_t[]> decoded_buffer_;
size_t decoded_buffer_length_ = kMaxFrameSize;
std::unique_ptr<AudioMultiVector> algorithm_buffer_;
std::unique_ptr<SyncBuffer> sync_buffer_;

bool first_packet_ = true;
uint32_t timestamp_;
uint32_t playout_timestamp_;

int fs_hz = 48000;
size_t channels = 1;
int fs_mult_ = fs_hz / 8000;
int samples_10ms = static_cast<size_t>(10 * 8 * fs_mult_);
int input_size_samples_ = static_cast<size_t>(kInputSizeMs * 8 * fs_mult_);
int output_size_samples_ = static_cast<size_t>(kOutputSizeMs * 8 * fs_mult_);
int decoder_frame_length_ = 3 * output_size_samples_;

void init_eq()
{
    // Reset random vector.
    random_vector_.Reset();
    // Reinit post-decode VAD with new sample rate.
    vad_.reset(new PostDecodeVad());
    vad_->Init();
    vad_->Enable();

    // Delete BackgroundNoise object and create a new one.
    background_noise_.reset(new BackgroundNoise(channels));

    expand_factory_.reset(new ExpandFactory);
    expand_.reset(expand_factory_->Create(background_noise_.get(),
                    sync_buffer_.get(), &random_vector_,
                    fs_hz, channels));

    merge_.reset(new Merge(fs_hz, channels, expand_.get(), sync_buffer_.get()));

    normal_.reset(new Normal(fs_hz, nullptr, *background_noise_, expand_.get()));

    accelerate_factory_.reset(new AccelerateFactory);
    accelerate_.reset(accelerate_factory_->Create(fs_hz, channels, *background_noise_));

    preemptive_expand_factory_.reset(new PreemptiveExpandFactory);
    preemptive_expand_.reset(preemptive_expand_factory_->Create(
        fs_hz, channels, *background_noise_, expand_->overlap_length()));

    tick_timer_.reset(new TickTimer);
    delay_peak_detector_.reset(new DelayPeakDetector(tick_timer_.get()));
    delay_manager_.reset(new DelayManager(kMaxPacketsInBuffer, delay_peak_detector_.get(), tick_timer_.get()));

    buffer_level_filter_.reset(new BufferLevelFilter);

	packet_buffer_.reset(new PacketBuffer(kMaxPacketsInBuffer));
    decoded_buffer_.reset(new int16_t[decoded_buffer_length_]);
    // Delete algorithm buffer and create a new one.
    algorithm_buffer_.reset(new AudioMultiVector(channels));
    // Delete sync buffer and create a new one.
    sync_buffer_.reset(new SyncBuffer(channels, kSyncBufferSize * fs_mult_));
    // Move index so that we create a small set of future samples (all 0).
    sync_buffer_->set_next_index(sync_buffer_->next_index() -
                    expand_->overlap_length());
}

int main(){

    init_eq();

    return 0;
}
