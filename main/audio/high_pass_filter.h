/*
 * High-pass filter for voice audio
 * Removes low frequency noise (below 300Hz) to improve voice recognition
 *
 * Two implementations:
 *   1. Software (default): simple first-order RC high-pass filter
 *   2. ESP-DSP biquad: hardware-optimized second-order IIR filter via
 *      Espressif DSP library (enable CONFIG_MIC_HIGH_PASS_FILTER_USE_ESP_DSP)
 */

#ifndef HIGH_PASS_FILTER_H
#define HIGH_PASS_FILTER_H

#include <cstdint>
#include <cmath>
#include <cstring>

#ifdef CONFIG_MIC_HIGH_PASS_FILTER_USE_ESP_DSP
#include "dsps_biquad_gen.h"
#include "dsps_biquad.h"
#endif

class HighPassFilter {
public:
    // Initialize filter with cutoff frequency and sample rate
    // For voice: cutoff = 100Hz, sample_rate = 16000Hz
    HighPassFilter(float cutoff_freq = 100.0f, float sample_rate = 16000.0f) {
#ifdef CONFIG_MIC_HIGH_PASS_FILTER_USE_ESP_DSP
        // Normalized frequency for biquad: f = cutoff / sample_rate
        float freq = cutoff_freq / sample_rate;
        // Butterworth Q factor for maximally flat passband
        float q_factor = 0.707f;
        dsps_biquad_gen_hpf_f32(coeffs_, freq, q_factor);
        memset(delay_, 0, sizeof(delay_));
#else
        // Calculate filter coefficient using RC time constant
        // RC = 1 / (2 * PI * cutoff_freq)
        // alpha = RC / (RC + dt) where dt = 1/sample_rate
        float rc = 1.0f / (2.0f * M_PI * cutoff_freq);
        float dt = 1.0f / sample_rate;
        alpha_ = rc / (rc + dt);
        
        prev_input_ = 0;
        prev_output_ = 0;
#endif
    }
    
    // Process a single sample
    inline int16_t Process(int16_t input) {
#ifdef CONFIG_MIC_HIGH_PASS_FILTER_USE_ESP_DSP
        float in = static_cast<float>(input);
        float out;
        dsps_biquad_f32(&in, &out, 1, coeffs_, delay_);
        if (out > 32767.0f) out = 32767.0f;
        if (out < -32768.0f) out = -32768.0f;
        return static_cast<int16_t>(out);
#else
        // High-pass filter: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
        float output = alpha_ * (prev_output_ + input - prev_input_);
        
        prev_input_ = input;
        prev_output_ = output;
        
        // Clamp to int16_t range
        if (output > 32767.0f) output = 32767.0f;
        if (output < -32768.0f) output = -32768.0f;
        
        return static_cast<int16_t>(output);
#endif
    }
    
    // Process an array of samples in-place
    void ProcessBuffer(int16_t* data, size_t samples) {
#ifdef CONFIG_MIC_HIGH_PASS_FILTER_USE_ESP_DSP
        // Batch convert to float, run optimized biquad, convert back
        float* buf = new (std::nothrow) float[samples];
        if (!buf) return;

        for (size_t i = 0; i < samples; i++) {
            buf[i] = static_cast<float>(data[i]);
        }

        dsps_biquad_f32(buf, buf, static_cast<int>(samples), coeffs_, delay_);

        for (size_t i = 0; i < samples; i++) {
            float val = buf[i];
            if (val > 32767.0f) val = 32767.0f;
            if (val < -32768.0f) val = -32768.0f;
            data[i] = static_cast<int16_t>(val);
        }

        delete[] buf;
#else
        for (size_t i = 0; i < samples; i++) {
            data[i] = Process(data[i]);
        }
#endif
    }
    
    // Reset filter state
    void Reset() {
#ifdef CONFIG_MIC_HIGH_PASS_FILTER_USE_ESP_DSP
        memset(delay_, 0, sizeof(delay_));
#else
        prev_input_ = 0;
        prev_output_ = 0;
#endif
    }
    
private:
#ifdef CONFIG_MIC_HIGH_PASS_FILTER_USE_ESP_DSP
    float coeffs_[5];  // biquad coefficients: b0, b1, b2, a1, a2
    float delay_[2];   // biquad delay line
#else
    float alpha_;
    float prev_input_;
    float prev_output_;
#endif
};

#endif // HIGH_PASS_FILTER_H