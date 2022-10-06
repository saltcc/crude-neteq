
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
#include "neteq/decision_logic.h"

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
std::unique_ptr<DecisionLogic> decision_logic_;

std::unique_ptr<PacketBuffer> packet_buffer_;
std::unique_ptr<int16_t[]> decoded_buffer_;
size_t decoded_buffer_length_ = kMaxFrameSize;
std::unique_ptr<AudioMultiVector> algorithm_buffer_;
std::unique_ptr<SyncBuffer> sync_buffer_;

bool first_packet_ = true;
uint32_t timestamp_ = 0;
uint32_t playout_timestamp_ = 0;

int fs_hz_ = 48000;
size_t channels = 1;
int fs_mult_ = fs_hz_ / 8000;
int samples_10ms = static_cast<size_t>(10 * 8 * fs_mult_);
int output_size_samples_ = static_cast<size_t>(kOutputSizeMs * 8 * fs_mult_);
int decoder_frame_length_ = 2 * output_size_samples_;
Modes last_mode_ = kModeNormal;
Operations last_operation_;

const bool enable_muted_state_ = true;
bool new_codec_ = false;
const bool no_time_stretching_ = true;
bool reset_decoder_ = false;

void init_param(){
    fs_hz_ = 48000;
    channels = 1;
    fs_mult_ = fs_hz_ / 8000;
    samples_10ms = static_cast<size_t>(10 * 8 * fs_mult_);
    output_size_samples_ = static_cast<size_t>(kOutputSizeMs * 8 * fs_mult_);
    decoder_frame_length_ = 2 * output_size_samples_;
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

enum SpeechType {
    kSpeech = 1,
    kComfortNoise = 2,
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

    decision_logic_.reset(DecisionLogic::Create(
        fs_hz_, output_size_samples_, no_time_stretching_,
        /*decoder_database_.get(),*/ *packet_buffer_.get(), delay_manager_.get(),
        buffer_level_filter_.get(), tick_timer_.get()));
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
        const int number_of_primary_packets = 1;
        if (number_of_primary_packets > 0) {
            const size_t packet_length_samples =
                number_of_primary_packets * decoder_frame_length_;
            if (packet_length_samples != decision_logic_->packet_length_samples()) {
                decision_logic_->set_packet_length_samples(packet_length_samples);
                delay_manager_->SetPacketAudioLength(
                    rtc::dchecked_cast<int>((1000 * packet_length_samples) / fs_hz_));
            }
        }

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

int ExtractPackets(size_t required_samples,
                              PacketList* packet_list) {
    bool first_packet = true;
    uint8_t prev_payload_type = 0;
    uint32_t prev_timestamp = 0;
    uint16_t prev_sequence_number = 0;
    bool next_packet_available = false;

    const Packet* next_packet = packet_buffer_->PeekNextPacket();
    RTC_DCHECK(next_packet);
    if (!next_packet) {
        //RTC_LOG(LS_ERROR) << "Packet buffer unexpectedly empty.";
        return -1;
    }
    uint32_t first_timestamp = next_packet->timestamp;
    size_t extracted_samples = 0;

  // Packet extraction loop.
    do {
        timestamp_ = next_packet->timestamp;
        std::optional<Packet> packet = packet_buffer_->GetNextPacket();
        // |next_packet| may be invalid after the |packet_buffer_| operation.
        next_packet = nullptr;
        if (!packet) {
            //RTC_LOG(LS_ERROR) << "Should always be able to extract a packet here";
            assert(false);  // Should always be able to extract a packet here.
            return -1;
        }
        // const uint64_t waiting_time_ms = packet->waiting_time->ElapsedMs();
        // stats_.StoreWaitingTime(waiting_time_ms);
        // RTC_DCHECK(!packet->empty());

        if (first_packet) {
            first_packet = false;
            // if (nack_enabled_) {
            //     RTC_DCHECK(nack_);
            //     // TODO(henrik.lundin): Should we update this for all decoded packets?
            //     nack_->UpdateLastDecodedPacket(packet->sequence_number,
            //                                 packet->timestamp);
            // }
            prev_sequence_number = packet->sequence_number;
            prev_timestamp = packet->timestamp;
            prev_payload_type = packet->payload_type;
        }
        const bool has_cng_packet = false;
        // const bool has_cng_packet =
        //     decoder_database_->IsComfortNoise(packet->payload_type);
        // Store number of extracted samples.
        size_t packet_duration = 0;
        // if (packet->frame) {
        //     packet_duration = packet->frame->Duration();
        //     // TODO(ossu): Is this the correct way to track Opus FEC packets?
        //     if (packet->priority.codec_level > 0) {
        //         stats_.SecondaryDecodedSamples(
        //             rtc::dchecked_cast<int>(packet_duration));
        //     }
        // } else if (!has_cng_packet) {
        //     RTC_LOG(LS_WARNING) << "Unknown payload type "
        //                         << static_cast<int>(packet->payload_type);
        //     RTC_NOTREACHED();
        // }

        if (packet_duration == 0) {
            // Decoder did not return a packet duration. Assume that the packet
            // contains the same number of samples as the previous one.
            packet_duration = decoder_frame_length_;
        }
        extracted_samples = packet->timestamp - first_timestamp + packet_duration;

        //stats_.JitterBufferDelay(extracted_samples, waiting_time_ms);

        packet_list->push_back(std::move(*packet));  // Store packet in list.
        packet = std::nullopt;  // Ensure it's never used after the move.

        // Check what packet is available next.
        next_packet = packet_buffer_->PeekNextPacket();
        next_packet_available = false;
        if (next_packet && prev_payload_type == next_packet->payload_type &&
            !has_cng_packet) {
            int16_t seq_no_diff = next_packet->sequence_number - prev_sequence_number;
            size_t ts_diff = next_packet->timestamp - prev_timestamp;
            if (seq_no_diff == 1 ||
                (seq_no_diff == 0 && ts_diff == decoder_frame_length_)) {
                    // The next sequence number is available, or the next part of a packet
                    // that was split into pieces upon insertion.
                    next_packet_available = true;
            }
            prev_sequence_number = next_packet->sequence_number;
        }
    } while (extracted_samples < required_samples && next_packet_available);

    if (extracted_samples > 0) {
        // Delete old packets only when we are going to decode something. Otherwise,
        // we could end up in the situation where we never decode anything, since
        // all incoming packets are considered too old but the buffer will also
        // never be flooded and flushed.
        packet_buffer_->DiscardAllOldPackets(timestamp_/*, &stats_*/);
    }

    return rtc::dchecked_cast<int>(extracted_samples);
}

int GetDecision(Operations* operation, PacketList* packet_list) 
{
    uint32_t end_timestamp = sync_buffer_->end_timestamp();
    if (!new_codec_) {
        const uint32_t five_seconds_samples = 5 * fs_hz_;
        packet_buffer_->DiscardOldPackets(end_timestamp, five_seconds_samples);
    }

    const Packet* packet = packet_buffer_->PeekNextPacket();

    uint64_t generated_noise_samples = 0;

    const int samples_left = static_cast<int>(sync_buffer_->FutureLength() -
                                                expand_->overlap_length());
    if (last_mode_ == kModeAccelerateSuccess ||
        last_mode_ == kModeAccelerateLowEnergy ||
        last_mode_ == kModePreemptiveExpandSuccess ||
        last_mode_ == kModePreemptiveExpandLowEnergy) {
        // Subtract (samples_left + output_size_samples_) from sampleMemory.
        decision_logic_->AddSampleMemory(
            -(samples_left + rtc::dchecked_cast<int>(output_size_samples_)));
    }

    *operation = decision_logic_->GetDecision(
        *sync_buffer_, *expand_, decoder_frame_length_, packet, last_mode_,
        false, generated_noise_samples, &reset_decoder_);

    // Check if we already have enough samples in the |sync_buffer_|. If so,
    // change decision to normal, unless the decision was merge, accelerate, or
    // preemptive expand.
    if (samples_left >= rtc::dchecked_cast<int>(output_size_samples_) &&
        *operation != kMerge && *operation != kAccelerate &&
        *operation != kFastAccelerate && *operation != kPreemptiveExpand) {
        *operation = kNormal;
        return 0;
    }

    decision_logic_->ExpandDecision(*operation);

    // Check conditions for reset.
    if (new_codec_ || *operation == kUndefined) {
        // The only valid reason to get kUndefined is that new_codec_ is set.
        assert(new_codec_);

        timestamp_ = packet->timestamp;
        *operation = kNormal;

        // Adjust |sync_buffer_| timestamp before setting |end_timestamp| to the
        // new value.
        sync_buffer_->IncreaseEndTimestamp(timestamp_ - end_timestamp);
        end_timestamp = timestamp_;
        new_codec_ = false;
        // decision_logic_->SoftReset();
        buffer_level_filter_->Reset();
        delay_manager_->Reset();
    }

    size_t required_samples = output_size_samples_;
    const size_t samples_10_ms = static_cast<size_t>(80 * fs_mult_);
    const size_t samples_20_ms = 2 * samples_10_ms;
    const size_t samples_30_ms = 3 * samples_10_ms;

    switch (*operation) {
        case kExpand: {
            timestamp_ = end_timestamp;
            return 0;
        }
        case kRfc3389CngNoPacket:
        case kCodecInternalCng: {
            return 0;
        }
        case kDtmf: {
            return 0;
        }
        case kAccelerate:
        case kFastAccelerate: {
            // In order to do an accelerate we need at least 30 ms of audio data.
            if (samples_left >= static_cast<int>(samples_30_ms)) {
                // Already have enough data, so we do not need to extract any more.
                decision_logic_->set_sample_memory(samples_left);
                decision_logic_->set_prev_time_scale(true);
                return 0;
            } else if (samples_left >= static_cast<int>(samples_10_ms) &&
                        decoder_frame_length_ >= samples_30_ms) {
                // Avoid decoding more data as it might overflow the playout buffer.
                *operation = kNormal;
                return 0;
            } else if (samples_left < static_cast<int>(samples_20_ms) &&
                        decoder_frame_length_ < samples_30_ms) {
                // Build up decoded data by decoding at least 20 ms of audio data. Do
                // not perform accelerate yet, but wait until we only need to do one
                // decoding.
                required_samples = 2 * output_size_samples_;
                *operation = kNormal;
            }
            // If none of the above is true, we have one of two possible situations:
            // (1) 20 ms <= samples_left < 30 ms and decoder_frame_length_ < 30 ms; or
            // (2) samples_left < 10 ms and decoder_frame_length_ >= 30 ms.
            // In either case, we move on with the accelerate decision, and decode one
            // frame now.
            break;
        }
        case kPreemptiveExpand: {
            // In order to do a preemptive expand we need at least 30 ms of decoded
            // audio data.
            if ((samples_left >= static_cast<int>(samples_30_ms)) ||
                (samples_left >= static_cast<int>(samples_10_ms) &&
                decoder_frame_length_ >= samples_30_ms)) {
                // Already have enough data, so we do not need to extract any more.
                // Or, avoid decoding more data as it might overflow the playout buffer.
                // Still try preemptive expand, though.
                decision_logic_->set_sample_memory(samples_left);
                decision_logic_->set_prev_time_scale(true);
                return 0;
            }
            if (samples_left < static_cast<int>(samples_20_ms) &&
                decoder_frame_length_ < samples_30_ms) {
                // Build up decoded data by decoding at least 20 ms of audio data.
                // Still try to perform preemptive expand.
                required_samples = 2 * output_size_samples_;
            }
            // Move on with the preemptive expand decision.
            break;
        }
        case kMerge: {
            required_samples =
                std::max(merge_->RequiredFutureSamples(), required_samples);
            break;
        }
        default: {
        }
    }

    // Get packets from buffer.
    int extracted_samples = 0;
    if (packet) {
        sync_buffer_->IncreaseEndTimestamp(packet->timestamp - end_timestamp);

        extracted_samples = ExtractPackets(required_samples, packet_list);
        if (extracted_samples < 0) {
            return kPacketBufferCorruption;
        }
    }

    if (*operation == kAccelerate || *operation == kFastAccelerate ||
        *operation == kPreemptiveExpand) {
        decision_logic_->set_sample_memory(samples_left + extracted_samples);
        decision_logic_->set_prev_time_scale(true);
    }

    if (*operation == kAccelerate || *operation == kFastAccelerate) {
        // Check that we have enough data (30ms) to do accelerate.
        if (extracted_samples + samples_left < static_cast<int>(samples_30_ms)) {
            // TODO(hlundin): Write test for this.
            // Not enough, do normal operation instead.
            *operation = kNormal;
        }
    }

    timestamp_ = end_timestamp;

    return 0;
}

int Decode(PacketList* packet_list,
                      Operations* operation,
                      int* decoded_length) 
{
    return decoder_frame_length_;
}


int GetAudioInternal(AudioFrame* audio_frame, bool* muted)
{
    Operations operation;
    PacketList packet_list;

    tick_timer_->Increment();

    if (enable_muted_state_ && expand_->Muted() && packet_buffer_->Empty()) {
        RTC_DCHECK_EQ(last_mode_, kModeExpand);
        audio_frame->Reset();
        RTC_DCHECK(audio_frame->muted());  // Reset() should mute the frame.
        playout_timestamp_ += static_cast<uint32_t>(output_size_samples_);
        audio_frame->sample_rate_hz_ = fs_hz_;
        audio_frame->samples_per_channel_ = output_size_samples_;
        audio_frame->timestamp_ =
            first_packet_
                ? 0
                : (playout_timestamp_) - static_cast<uint32_t>(audio_frame->samples_per_channel_);
        audio_frame->num_channels_ = sync_buffer_->Channels();
        *muted = true;
        return 0;
    }

    int return_value = GetDecision(&operation, &packet_list);
    if (return_value != 0) {
        last_mode_ = kModeError;
        return return_value;
    }

    SpeechType speech_type;
    int length = 0;

    int decode_return_value = Decode(&packet_list, &operation, &length);

    algorithm_buffer_->Clear();
    switch (operation) {
        case kNormal: {
            //DoNormal(decoded_buffer_.get(), length, speech_type, play_dtmf);
            break;
        }
        case kMerge: {
            //DoMerge(decoded_buffer_.get(), length, speech_type, play_dtmf);
            break;
        }
        case kExpand: {
            // RTC_DCHECK_EQ(return_value, 0);
            // if (!current_rtp_payload_type_ || !DoCodecPlc()) {
            //     return_value = DoExpand(play_dtmf);
            // }
            // RTC_DCHECK_GE(sync_buffer_->FutureLength() - expand_->overlap_length(),
            //                 output_size_samples_);
            break;
        }
        case kAccelerate:
        case kFastAccelerate: {
            // const bool fast_accelerate =
            //     enable_fast_accelerate_ && (operation == kFastAccelerate);
            // return_value = DoAccelerate(decoded_buffer_.get(), length, speech_type,
            //                             play_dtmf, fast_accelerate);
            break;
        }
        case kPreemptiveExpand: {
            // return_value = DoPreemptiveExpand(decoded_buffer_.get(), length,
            //                                     speech_type, play_dtmf);
            // break;
        }
        // case kRfc3389Cng:
        // case kRfc3389CngNoPacket: {
        //     return_value = DoRfc3389Cng(&packet_list, play_dtmf);
        //     break;
        // }
        // case kCodecInternalCng: {
        //     // This handles the case when there is no transmission and the decoder
        //     // should produce internal comfort noise.
        //     // TODO(hlundin): Write test for codec-internal CNG.
        //     DoCodecInternalCng(decoded_buffer_.get(), length);
        //     break;
        // }
        // case kDtmf: {
        //     // TODO(hlundin): Write test for this.
        //     return_value = DoDtmf(dtmf_event, &play_dtmf);
        //     break;
        // }
        case kUndefined: {
            //RTC_LOG(LS_ERROR) << "Invalid operation kUndefined.";
            assert(false);  // This should not happen.
            last_mode_ = kModeError;
            return kInvalidOperation;
        }
    }  // End of switch.

    last_operation_ = operation;
    if (return_value < 0) {
        return return_value;
    }

    // Copy from |algorithm_buffer| to |sync_buffer_|.
    sync_buffer_->PushBack(*algorithm_buffer_);

    // Extract data from |sync_buffer_| to |output|.
    size_t num_output_samples_per_channel = output_size_samples_;
    size_t num_output_samples = output_size_samples_ * sync_buffer_->Channels();
    if (num_output_samples > AudioFrame::kMaxDataSizeSamples) {
        // RTC_LOG(LS_WARNING) << "Output array is too short. "
        //                     << AudioFrame::kMaxDataSizeSamples << " < "
        //                     << output_size_samples_ << " * "
        //                     << sync_buffer_->Channels();
        num_output_samples = AudioFrame::kMaxDataSizeSamples;
        num_output_samples_per_channel =
            AudioFrame::kMaxDataSizeSamples / sync_buffer_->Channels();
    }
    sync_buffer_->GetNextAudioInterleaved(num_output_samples_per_channel,
                                            audio_frame);
    audio_frame->sample_rate_hz_ = fs_hz_;

    if (sync_buffer_->FutureLength() < expand_->overlap_length()) {
        // The sync buffer should always contain |overlap_length| samples, but now
        // too many samples have been extracted. Reinstall the |overlap_length|
        // lookahead by moving the index.
        const size_t missing_lookahead_samples =
            expand_->overlap_length() - sync_buffer_->FutureLength();
        RTC_DCHECK_GE(sync_buffer_->next_index(), missing_lookahead_samples);
        sync_buffer_->set_next_index(sync_buffer_->next_index() -
                                    missing_lookahead_samples);
    }

    if (audio_frame->samples_per_channel_ != output_size_samples_) {
        // RTC_LOG(LS_ERROR) << "audio_frame->samples_per_channel_ ("
        //                 << audio_frame->samples_per_channel_
        //                 << ") != output_size_samples_ (" << output_size_samples_
        //                 << ")";
        // TODO(minyue): treatment of under-run, filling zeros
        audio_frame->Mute();
        return kSampleUnderrun;
    }

    // Update the background noise parameters if last operation wrote data
    // straight from the decoder to the |sync_buffer_|. That is, none of the
    // operations that modify the signal can be followed by a parameter update.
    if ((last_mode_ == kModeNormal) || (last_mode_ == kModeAccelerateFail) ||
        (last_mode_ == kModePreemptiveExpandFail) ||
        (last_mode_ == kModeRfc3389Cng) ||
        (last_mode_ == kModeCodecInternalCng)) {
        background_noise_->Update(*sync_buffer_, *vad_.get());
    }

    if (last_mode_ != kModeExpand && last_mode_ != kModeCodecPlc) {
        // If last operation was not expand, calculate the |playout_timestamp_| from
        // the |sync_buffer_|. However, do not update the |playout_timestamp_| if it
        // would be moved "backwards".
        uint32_t temp_timestamp =
            sync_buffer_->end_timestamp() -
            static_cast<uint32_t>(sync_buffer_->FutureLength());
        if (static_cast<int32_t>(temp_timestamp - playout_timestamp_) > 0) {
            playout_timestamp_ = temp_timestamp;
        }
    } else {
        // Use dead reckoning to estimate the |playout_timestamp_|.
        playout_timestamp_ += static_cast<uint32_t>(output_size_samples_);
    }

    audio_frame->timestamp_ =
        first_packet_
            ? 0
            : playout_timestamp_ - static_cast<uint32_t>(audio_frame->samples_per_channel_);


    if (decode_return_value)
        return decode_return_value;
    return return_value;
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
