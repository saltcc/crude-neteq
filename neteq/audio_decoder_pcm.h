/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_AUDIO_CODING_CODECS_G711_AUDIO_DECODER_PCM_H_
#define MODULES_AUDIO_CODING_CODECS_G711_AUDIO_DECODER_PCM_H_

#include "neteq/audio_decoder.h"
#include "rtc_base/checks.h"
#include "rtc_base/constructormagic.h"

namespace webrtc {

class AudioDecoderPcmU final : public AudioDecoder {
public:
    explicit AudioDecoderPcmU(size_t num_channels) : num_channels_(num_channels) {
      RTC_DCHECK_GE(num_channels, 1);
    }

    size_t Channels() const override;
    int SampleRateHz() const override;
    int PacketDuration(const uint8_t* encoded, size_t encoded_len) const override;
    void Reset() override;

protected:
    int DecodeInternal(const uint8_t* encoded,
                      size_t encoded_len,
                      int sample_rate_hz,
                      int16_t* decoded,
                      SpeechType* speech_type) override;

private:
    const size_t num_channels_;
    RTC_DISALLOW_COPY_AND_ASSIGN(AudioDecoderPcmU);
};

class AudioDecoderPcmA final : public AudioDecoder {
public:
    explicit AudioDecoderPcmA(size_t num_channels) : num_channels_(num_channels) {
      RTC_DCHECK_GE(num_channels, 1);
    }
    size_t Channels() const override;
    int SampleRateHz() const override;
    int PacketDuration(const uint8_t* encoded, size_t encoded_len) const override;
    void Reset() override;

protected:
    int DecodeInternal(const uint8_t* encoded,
                      size_t encoded_len,
                      int sample_rate_hz,
                      int16_t* decoded,
                      SpeechType* speech_type) override;

private:
    const size_t num_channels_;
    RTC_DISALLOW_COPY_AND_ASSIGN(AudioDecoderPcmA);
};

}

#endif
