#include "pcm5102.h"

#include <stdbool.h>

#include "driver/i2s_std.h"
#include "esp_log.h"

static const char* TAG = "pcm5102";

#define PCM5102_PIN_WS GPIO_NUM_4    // LRCK
#define PCM5102_PIN_DOUT GPIO_NUM_5  // DIN
#define PCM5102_PIN_BCK GPIO_NUM_6   // BCK (DCK on the module)

#define DEFAULT_SAMPLE_RATE_HZ 44100
// 8 x 480 frames = 3840 frames, about 87 ms at 44.1 kHz: long enough to ride
// out an SD read (the card is slow) while audio_task refills its buffer.
#define DMA_DESC_NUM 8
#define DMA_FRAME_NUM 480
#define WRITE_TIMEOUT_MS 1000
#define VOLUME_MAX 100
#define GAIN_UNITY 32768  // Q15: 1.0
#define GAIN_SHIFT 15

static i2s_chan_handle_t s_tx_chan = NULL;
static volatile uint32_t s_volume_gain = GAIN_UNITY;  // Q15, from the volume curve
static volatile bool s_muted = false;

static uint32_t gain_from_volume(uint8_t volume) {
  if (volume > VOLUME_MAX) {
    volume = VOLUME_MAX;
  }
  // Quadratic curve: volume 100 -> 1.0, volume 50 -> 0.25.
  return ((uint32_t)volume * volume * GAIN_UNITY) / (VOLUME_MAX * VOLUME_MAX);
}

esp_err_t pcm5102_init(void) {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = DMA_DESC_NUM;
  chan_cfg.dma_frame_num = DMA_FRAME_NUM;
  chan_cfg.auto_clear = true;  // silence, not a repeated buffer, when nothing is written

  esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
    return err;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_SAMPLE_RATE_HZ),
      .slot_cfg =
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,  // the PCM5102 runs from BCK (SCK tied to GND)
              .bclk = PCM5102_PIN_BCK,
              .ws = PCM5102_PIN_WS,
              .dout = PCM5102_PIN_DOUT,
              .din = I2S_GPIO_UNUSED,
          },
  };

  err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    return err;
  }

  err = i2s_channel_enable(s_tx_chan);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    return err;
  }

  ESP_LOGI(TAG, "I2S ready: BCK=%d WS=%d DOUT=%d, %d Hz stereo 16-bit", PCM5102_PIN_BCK,
           PCM5102_PIN_WS, PCM5102_PIN_DOUT, DEFAULT_SAMPLE_RATE_HZ);
  return ESP_OK;
}

esp_err_t pcm5102_set_sample_rate(uint32_t sample_rate_hz) {
  if (s_tx_chan == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  // The clock can only be reconfigured while the channel is disabled.
  esp_err_t err = i2s_channel_disable(s_tx_chan);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(err));
    return err;
  }

  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
  err = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set %u Hz: %s", (unsigned)sample_rate_hz, esp_err_to_name(err));
  }

  // Enable again even if the reconfiguration failed, so the channel isn't left stopped.
  esp_err_t enable_err = i2s_channel_enable(s_tx_chan);
  if (enable_err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(enable_err));
    return enable_err;
  }
  return err;
}

esp_err_t pcm5102_write(int16_t* samples, size_t frames) {
  if (s_tx_chan == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (samples == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t sample_count = frames * 2;  // two channels
  uint32_t gain = s_muted ? 0 : s_volume_gain;
  if (gain != GAIN_UNITY) {
    for (size_t i = 0; i < sample_count; i++) {
      samples[i] = (int16_t)(((int32_t)samples[i] * (int32_t)gain) >> GAIN_SHIFT);
    }
  }

  size_t bytes_written = 0;
  return i2s_channel_write(s_tx_chan, samples, sample_count * sizeof(int16_t), &bytes_written,
                           WRITE_TIMEOUT_MS);
}

void pcm5102_set_volume(uint8_t volume) {
  s_volume_gain = gain_from_volume(volume);
}

void pcm5102_set_mute(bool muted) {
  s_muted = muted;
}
