/*
 * High-pass filter for voice audio
 * Removes low frequency noise (below 300Hz) to improve voice recognition
 */

#ifndef HIGH_PASS_FILTER_H
#define HIGH_PASS_FILTER_H

#include <cstdint>
#include <cmath>

class HighPassFilter {
public:
    // Initialize filter with cutoff frequency and sample rate
    // For voice: cutoff = 300Hz, sample_rate = 16000Hz
    HighPassFilter(float cutoff_freq = 300.0f, float sample_rate = 16000.0f) {
        // Calculate filter coefficient using RC time constant
        // RC = 1 / (2 * PI * cutoff_freq)
        // alpha = RC / (RC + dt) where dt = 1/sample_rate
        float rc = 1.0f / (2.0f * M_PI * cutoff_freq);
        float dt = 1.0f / sample_rate;
        alpha_ = rc / (rc + dt);
        
        prev_input_ = 0;
        prev_output_ = 0;
    }
    
    // Process a single sample
    inline int16_t Process(int16_t input) {
        // High-pass filter: y[n] = alpha * (y[n-1] + x[n] - x[n-1])
        float output = alpha_ * (prev_output_ + input - prev_input_);
        
        prev_input_ = input;
        prev_output_ = output;
        
        // Clamp to int16_t range
        if (output > 32767.0f) output = 32767.0f;
        if (output < -32768.0f) output = -32768.0f;
        
        return static_cast<int16_t>(output);
    }
    
    // Process an array of samples in-place
    void ProcessBuffer(int16_t* data, size_t samples) {
        for (size_t i = 0; i < samples; i++) {
            data[i] = Process(data[i]);
        }
    }
    
    // Reset filter state
    void Reset() {
        prev_input_ = 0;
        prev_output_ = 0;
    }
    
private:
    float alpha_;
    float prev_input_;
    float prev_output_;
};

#endif // HIGH_PASS_FILTER_H