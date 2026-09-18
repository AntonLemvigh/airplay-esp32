/**
 * @file board.c
 * @brief Board implementation for Sonocotta HiFi-ESP32-Plus (rev J1+)
 *
 * Same layout as HiFi-ESP32 (I2S on 26/25/22, W5500 + OLED on a shared SPI
 * bus) but with a PCM5122 instead of the PCM5100. The PCM5122 is controlled
 * over I2C (SDA 21, SCL 27, address 0x4D) and needs a handful of registers
 * written at boot before it plays without an MCLK line, so this board brings
 * up the DAC I2C bus and registers the PCM51xx driver. GPIO 13 is the board's
 * DAC/AMP enable (XSMT), toggled from playback events like the other
 * Sonocotta boards do.
 */

#include "iot_board.h"

#include "dac.h"
#include "dac_pcm51xx.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "playback_events.h"
#include "settings.h"

#ifdef CONFIG_ETH_W5500_ENABLED
#include "driver/spi_master.h"
#endif

static const char TAG[] = "HiFiEsp32Plus";

static bool s_board_initialized = false;
static i2c_master_bus_handle_t s_i2c_dac_bus_handle = NULL;

#ifdef CONFIG_ETH_W5500_ENABLED
static bool s_spi_bus_initialized = false;
#endif

static void on_playback_event(playback_source_t source, playback_event_t event,
                              const playback_event_data_t *data,
                              void *user_data);

const char *iot_board_get_info(void) {
  return BOARD_NAME;
}

bool iot_board_is_init(void) {
  return s_board_initialized;
}

board_res_handle_t iot_board_get_handle(int id) {
  switch (id) {
  case BOARD_I2C_DAC_ID:
    return (board_res_handle_t)s_i2c_dac_bus_handle;
  case BOARD_SPI_ETH_ID:
  case BOARD_SPI_DISP_ID:
#ifdef CONFIG_ETH_W5500_ENABLED
    return s_spi_bus_initialized ? (board_res_handle_t)(intptr_t)BOARD_SPI_HOST
                                 : NULL;
#else
    return NULL;
#endif
  default:
    return NULL;
  }
}

esp_err_t iot_board_init(void) {
  esp_err_t err = ESP_OK;

  if (s_board_initialized) {
    ESP_LOGW(TAG, "Board already initialized");
    return ESP_OK;
  }

#ifdef CONFIG_ETH_W5500_ENABLED
  spi_bus_config_t spi_bus_cfg = {
      .mosi_io_num = BOARD_SPI_MOSI_GPIO,
      .miso_io_num = BOARD_SPI_MISO_GPIO,
      .sclk_io_num = BOARD_SPI_CLK_GPIO,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
  };
  err = spi_bus_initialize(BOARD_SPI_HOST, &spi_bus_cfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(err));
    return err;
  }
  s_spi_bus_initialized = true;
  ESP_LOGI(TAG, "SPI bus initialized: mosi=%d, miso=%d, clk=%d",
           BOARD_SPI_MOSI_GPIO, BOARD_SPI_MISO_GPIO, BOARD_SPI_CLK_GPIO);
#endif

  // DAC control bus
  i2c_master_bus_config_t i2c_cfg = {
      .i2c_port = BOARD_I2C_PORT,
      .sda_io_num = BOARD_I2C_SDA_GPIO,
      .scl_io_num = BOARD_I2C_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  err = i2c_new_master_bus(&i2c_cfg, &s_i2c_dac_bus_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize DAC I2C bus: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "DAC I2C bus %d initialized: sda=%d, scl=%d", BOARD_I2C_PORT,
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);

  dac_register(&dac_pcm51xx_ops);
  err = dac_init(s_i2c_dac_bus_handle);
  if (err != ESP_OK) {
    // Keep going: the I2S output still runs, the DAC just won't be controlled.
    ESP_LOGE(TAG, "Failed to initialize PCM5122: %s", esp_err_to_name(err));
  } else {
    float vol_db;
    if (ESP_OK == settings_get_volume(&vol_db)) {
      dac_set_volume(vol_db);
    }
  }

  playback_events_register(on_playback_event, NULL);

  s_board_initialized = true;
  ESP_LOGI(TAG, "HiFi-ESP32-Plus initialized");
  return ESP_OK;
}

esp_err_t iot_board_deinit(void) {
  if (!s_board_initialized) {
    return ESP_OK;
  }

  playback_events_unregister(on_playback_event);
  dac_set_power_mode(DAC_POWER_OFF);
  dac_deinit();

  if (s_i2c_dac_bus_handle) {
    i2c_del_master_bus(s_i2c_dac_bus_handle);
    s_i2c_dac_bus_handle = NULL;
  }

#ifdef CONFIG_ETH_W5500_ENABLED
  if (s_spi_bus_initialized) {
    spi_bus_free(BOARD_SPI_HOST);
    s_spi_bus_initialized = false;
  }
#endif

  s_board_initialized = false;
  return ESP_OK;
}

static void on_playback_event(playback_source_t source, playback_event_t event,
                              const playback_event_data_t *data,
                              void *user_data) {
  (void)source;
  (void)data;
  (void)user_data;

  switch (event) {
  case PLAYBACK_EVENT_CONNECTED:
  case PLAYBACK_EVENT_PAUSED:
    dac_set_power_mode(DAC_POWER_STANDBY);
    break;
  case PLAYBACK_EVENT_PLAYING:
    dac_set_power_mode(DAC_POWER_ON);
    break;
  case PLAYBACK_EVENT_DISCONNECTED:
    dac_set_power_mode(DAC_POWER_OFF);
    break;
  case PLAYBACK_EVENT_METADATA:
    break;
  }
}
