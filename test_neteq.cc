
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

static const int kOutputSizeMs = 10;
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
uint32_t timestamp_ = 0;
uint32_t playout_timestamp_;

int fs_hz_ = 48000;
size_t channels = 1;
int fs_mult_ = fs_hz_ / 8000;
int samples_10ms = static_cast<size_t>(10 * 8 * fs_mult_);
int output_size_samples_ = static_cast<size_t>(kOutputSizeMs * 8 * fs_mult_);
int decoder_frame_length_ = 3 * output_size_samples_;
Modes last_mode_ = kModeNormal;

void init_param(){
    fs_hz_ = 48000;
    channels = 1;
    fs_mult_ = fs_hz_ / 8000;
    samples_10ms = static_cast<size_t>(10 * 8 * fs_mult_);
    output_size_samples_ = static_cast<size_t>(kOutputSizeMs * 8 * fs_mult_);
    decoder_frame_length_ = 3 * output_size_samples_;
    last_mode_ = kModeNormal;
}

struct RTPHeader {
  RTPHeader() = default;
  RTPHeader(const RTPHeader& other) = default;
  RTPHeader& operator=(const RTPHeader& other) = default;
  uint8_t payloadType{0};
  uint16_t sequenceNumber{0};
  uint32_t timestamp{0};
};

  enum ErrorCodes {
    kNoError = 0,
    kOtherError,
    kUnknownRtpPayloadType,
    kDecoderNotFound,
    kInvalidPointer,
    kAccelerateError,
    kPreemptiveExpandError,
    kComfortNoiseErrorCode,
    kDecoderErrorCode,
    kOtherDecoderError,
    kInvalidOperation,
    kDtmfParsingError,
    kDtmfInsertError,
    kSampleUnderrun,
    kDecodedTooMuch,
    kRedundancySplitError,
    kPacketBufferCorruption
  };

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
                    fs_hz_, channels));

    merge_.reset(new Merge(fs_hz_, channels, expand_.get(), sync_buffer_.get()));

    normal_.reset(new Normal(fs_hz_, nullptr, *background_noise_, expand_.get()));

    accelerate_factory_.reset(new AccelerateFactory);
    accelerate_.reset(accelerate_factory_->Create(fs_hz_, channels, *background_noise_));

    preemptive_expand_factory_.reset(new PreemptiveExpandFactory);
    preemptive_expand_.reset(preemptive_expand_factory_->Create(
        fs_hz_, channels, *background_noise_, expand_->overlap_length()));

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

int InsertPacket(const RTPHeader& rtp_header,
                    rtc::ArrayView<const uint8_t> payload,
                    uint32_t receive_timestamp) 
{
    if (payload.empty()){
        return kInvalidPointer;
    }

    Packet packet;
    packet.payload_type = rtp_header.payloadType;
    packet.sequence_number = rtp_header.sequenceNumber;
    packet.timestamp = rtp_header.timestamp;
    packet.payload.SetData(payload.data(), payload.size());

    // Store these for later use, since the first packet may very well disappear
    // before we need these values.
    uint32_t main_timestamp = packet.timestamp;
    uint8_t main_payload_type = packet.payload_type;
    uint16_t main_sequence_number = packet.sequence_number;

    bool update_sample_rate_and_channels = first_packet_;

    if (update_sample_rate_and_channels){

        packet_buffer_->Flush();

        sync_buffer_->IncreaseEndTimestamp(main_timestamp - timestamp_);

        timestamp_ = main_timestamp;
        // const size_t packet_length_samples = 1 * decoder_frame_length_;
        // delay_manager_->SetPacketAudioLength(
        //     rtc::dchecked_cast<int>((1000 * packet_length_samples) / fs_hz_));
    }

    const int ret = packet_buffer_->InsertPacket(std::move(packet));
    if (ret == PacketBuffer::kFlushed) {
        // Reset DSP timestamp etc. if packet buffer flushed.
        update_sample_rate_and_channels = true;
    } else if (ret != PacketBuffer::kOK) {
        return kOtherError;
    }

    if (first_packet_) {
        first_packet_ = false;
    }

    delay_manager_->LastDecodedWasCngOrDtmf(0/*dec_info->IsComfortNoise() ||
                                            dec_info->IsDtmf()*/);
    if (delay_manager_->last_pack_cng_or_dtmf() == 0) {

        // Update statistics.
        if ((int32_t)(main_timestamp - timestamp_) >= 0/* && !new_codec_*/) {
            // Only update statistics if incoming packet is not older than last played
            // out packet, and if new codec flag is not set.
            delay_manager_->Update(main_sequence_number, main_timestamp, fs_hz_);
        }
    } else if (delay_manager_->last_pack_cng_or_dtmf() == -1) {
        // This is first "normal" packet after CNG or DTMF.
        // Reset packet time counter and measure time until next packet,
        // but don't update statistics.
        delay_manager_->set_last_pack_cng_or_dtmf(0);
        delay_manager_->ResetPacketIatCount();
    }

    return 0;
}

int main(){

    init_eq();

    const size_t kPayloadLength = 100;
    const uint8_t kPayloadType = 0;
    const uint16_t kFirstSequenceNumber = 0x1234;
    const uint32_t kFirstTimestamp = 0x12345678;
    const uint32_t kFirstReceiveTime = 17;
    uint8_t payload[kPayloadLength] = {0};

    RTPHeader rtp_header;
    rtp_header.payloadType = kPayloadType;
    rtp_header.sequenceNumber = kFirstSequenceNumber;
    rtp_header.timestamp = kFirstTimestamp;

    InsertPacket(rtp_header, payload, kFirstReceiveTime);
    
    for (int i = 1; i < 10000; ++i){
        tick_timer_->Increment();

        if (i % 5 == 0)
        {
            rtp_header.timestamp += 480;
            rtp_header.sequenceNumber += 1;
            InsertPacket(rtp_header, payload, kFirstReceiveTime);
        }
        if (i % 3 == 0)
        {
            rtp_header.timestamp += 480;
            rtp_header.sequenceNumber += 1;
            InsertPacket(rtp_header, payload, kFirstReceiveTime);
        }
    }
    delay_manager_->ShowHistogram();
    printf("target level:%d\n",delay_manager_->TargetLevel()>>8);

    return 0;
}
