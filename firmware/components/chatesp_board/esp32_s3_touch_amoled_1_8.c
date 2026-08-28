#include <limits.h>
#include <stdio.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_co5300.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_touch_ft5x06.h"

#include "esp_codec_dev_defaults.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"
#include "bsp_err_check.h"
#include "bsp/display.h"
#include "bsp/touch.h"

static const char *TAG = "ESP32-S3-Touch-AMOLED-1.8";

#define BSP_LCD_CST816S_X_GAP (0x10)
#define BSP_BOARD_PROBE_TIMEOUT_MS (20)
#define BSP_TOUCH_ERROR_LOG_INTERVAL_MS (5000)
#define BSP_SH8601_QSPI_WRITE_CMD(command) \
    ((0x02UL << 24) | (((uint32_t)(command) & 0xFFUL) << 8))
#define BSP_IO_EXPANDER_I2C_ADDRESS (0x20)
#define BSP_IO_EXPANDER_INPUT_REG (0x00)
#define BSP_IO_EXPANDER_OUTPUT_REG (0x01)
#define BSP_IO_EXPANDER_DIRECTION_REG (0x03)
#define BSP_IO_EXPANDER_TIMEOUT_MS (100)

// Keep every TCA9554 bit in this adapter. All calls use a mask so a display
// reset cannot change the PWR input or another shared expander output.
#define BSP_EXIO_PANEL_RESET (1U << 0)
#define BSP_EXIO_TOUCH_RESET (1U << 1)
#define BSP_EXIO_PANEL_POWER (1U << 2)
#define BSP_EXIO_POWER_BUTTON (1U << 4)
#define BSP_EXIO_COLD_RESET_MASK \
    (BSP_EXIO_PANEL_RESET | BSP_EXIO_TOUCH_RESET | BSP_EXIO_PANEL_POWER)

static i2c_master_bus_handle_t i2c_handle = NULL; // I2C Handle
static bool i2c_initialized = false;
static i2c_master_dev_handle_t io_expander = NULL;
static bool cold_reset_finished = false;
static esp_err_t cold_reset_result = ESP_ERR_INVALID_STATE;
static uint8_t cold_reset_attempts = 0;
static bool revision_probed = false;
static bsp_board_revision_t board_revision = BSP_BOARD_REVISION_UNKNOWN;
static lv_indev_t *disp_indev = NULL;
sdmmc_card_t *bsp_sdcard = NULL; // Global uSD card handler
static esp_lcd_touch_handle_t tp = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL; // LCD panel handle
static esp_lcd_panel_io_handle_t io_handle = NULL;
static bool panel_display_on = false;
static bool panel_brightness_is_zero = true;
static uint16_t panel_x_gap = 0;
static uint32_t touch_error_logged_at_ms = 0;
static bsp_board_diagnostics_t board_diagnostics = {0};

static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL; /* Codec data interface */

static esp_err_t bsp_cold_reset_outputs(void);

static void board_diag_increment(uint32_t *counter)
{
    uint32_t current = __atomic_load_n(counter, __ATOMIC_RELAXED);
    while (current != UINT32_MAX &&
           !__atomic_compare_exchange_n(
               counter, &current, current + 1, false,
               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static uint32_t board_diag_load(const uint32_t *counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

static const co5300_lcd_init_cmd_t co5300_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

// The original-board path follows the official Waveshare BSP sequence. Keep
// brightness at zero until ChatESP has sent the complete black startup frame.
static const sh8601_lcd_init_cmd_t sh8601_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
};

#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }

/**************************************************************************************************
 *
 * I2C Function
 *
 **************************************************************************************************/
esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized)
    {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = BSP_I2C_NUM,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
    i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    bsp_i2c_init();
    return i2c_handle;
}

static esp_err_t bsp_i2c_device_probe(uint8_t addr)
{
    return i2c_master_probe(i2c_handle, addr, BSP_BOARD_PROBE_TIMEOUT_MS);
}

bsp_board_revision_t bsp_board_revision(void)
{
    if (revision_probed) {
        return board_revision;
    }

    revision_probed = true;
    const esp_err_t reset_error = bsp_cold_reset_outputs();
    if (reset_error != ESP_OK) {
        ESP_LOGE(
            TAG, "Cold panel reset failed (category %s)",
            esp_err_to_name(reset_error));
        return board_revision;
    }
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "Board revision unknown: I2C is not available");
        return board_revision;
    }

    if (bsp_i2c_device_probe(0x15) == ESP_OK) {
        board_revision = BSP_BOARD_REVISION_V2;
        ESP_LOGI(TAG, "Board revision: V2");
    } else if (bsp_i2c_device_probe(0x38) == ESP_OK) {
        board_revision = BSP_BOARD_REVISION_ORIGINAL;
        ESP_LOGI(TAG, "Board revision: original");
    } else {
        ESP_LOGW(TAG, "Board revision: unknown; V2 display fallback selected");
    }
    return board_revision;
}

esp_err_t bsp_board_diagnostics_get(bsp_board_diagnostics_t *snapshot)
{
    ESP_RETURN_ON_FALSE(
        snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
        "No board diagnostic output");
    snapshot->touch_read_ok = board_diag_load(&board_diagnostics.touch_read_ok);
    snapshot->touch_read_error =
        board_diag_load(&board_diagnostics.touch_read_error);
    snapshot->display_command_ok =
        board_diag_load(&board_diagnostics.display_command_ok);
    snapshot->display_command_error =
        board_diag_load(&board_diagnostics.display_command_error);
    snapshot->display_recovery_ok =
        board_diag_load(&board_diagnostics.display_recovery_ok);
    snapshot->display_recovery_error =
        board_diag_load(&board_diagnostics.display_recovery_error);
    snapshot->display_lock_timeout =
        board_diag_load(&board_diagnostics.display_lock_timeout);
    return ESP_OK;
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

esp_err_t bsp_sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};

    const sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const sdmmc_slot_config_t slot_config = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

#if !CONFIG_FATFS_LONG_FILENAMES
    ESP_LOGW(TAG, "Warning: Long filenames on SD card are disabled in menuconfig!");
#endif

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &bsp_sdcard);
}

esp_err_t bsp_sdcard_unmount(void)
{
    return esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
}


esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    if (i2s_data_if != NULL) {
        /* Audio was initialized before */
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    bool tx_enabled = false;
    bool rx_enabled = false;

    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    result = i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan);
    if (result != ESP_OK) {
        goto fail;
    }

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(22050);
    const i2s_std_config_t *p_i2s_cfg = &std_cfg_default;
    if (i2s_config != NULL) {
        p_i2s_cfg = i2s_config;
    }

    if (i2s_tx_chan != NULL) {
        result = i2s_channel_init_std_mode(i2s_tx_chan, p_i2s_cfg);
        if (result != ESP_OK) {
            goto fail;
        }
        result = i2s_channel_enable(i2s_tx_chan);
        if (result != ESP_OK) {
            goto fail;
        }
        tx_enabled = true;
    }

    if (i2s_rx_chan != NULL) {
        result = i2s_channel_init_std_mode(i2s_rx_chan, p_i2s_cfg);
        if (result != ESP_OK) {
            goto fail;
        }
        result = i2s_channel_enable(i2s_rx_chan);
        if (result != ESP_OK) {
            goto fail;
        }
        rx_enabled = true;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .tx_handle = i2s_tx_chan,
        .rx_handle = i2s_rx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (i2s_data_if == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    return ESP_OK;

fail:
    if (i2s_rx_chan != NULL) {
        if (rx_enabled) {
            i2s_channel_disable(i2s_rx_chan);
        }
        i2s_del_channel(i2s_rx_chan);
        i2s_rx_chan = NULL;
    }
    if (i2s_tx_chan != NULL) {
        if (tx_enabled) {
            i2s_channel_disable(i2s_tx_chan);
        }
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = NULL;
    }
    ESP_LOGE(TAG, "Audio interface initialization failed: %s", esp_err_to_name(result));
    return result;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        if (bsp_i2c_init() != ESP_OK) {
            return NULL;
        }
        /* Configure I2S peripheral and Power Amplifier */
        if (bsp_audio_init(NULL) != ESP_OK) {
            return NULL;
        }
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (i2c_ctrl_if == NULL) {
        audio_codec_delete_gpio_if(gpio_if);
        return NULL;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_TYPE_OUT,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (es8311_dev == NULL) {
        audio_codec_delete_ctrl_if(i2c_ctrl_if);
        audio_codec_delete_gpio_if(gpio_if);
        return NULL;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    esp_codec_dev_handle_t codec_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (codec_dev == NULL) {
        audio_codec_delete_codec_if(es8311_dev);
        audio_codec_delete_ctrl_if(i2c_ctrl_if);
        audio_codec_delete_gpio_if(gpio_if);
    }
    return codec_dev;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        if (bsp_i2c_init() != ESP_OK) {
            return NULL;
        }
        /* Configure I2S peripheral and Power Amplifier */
        if (bsp_audio_init(NULL) != ESP_OK) {
            return NULL;
        }
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (i2c_ctrl_if == NULL) {
        audio_codec_delete_gpio_if(gpio_if);
        return NULL;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };

    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (es8311_dev == NULL) {
        audio_codec_delete_ctrl_if(i2c_ctrl_if);
        audio_codec_delete_gpio_if(gpio_if);
        return NULL;
    }

    esp_codec_dev_cfg_t codec_es8311_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    esp_codec_dev_handle_t codec_dev = esp_codec_dev_new(&codec_es8311_dev_cfg);
    if (codec_dev == NULL) {
        audio_codec_delete_codec_if(es8311_dev);
        audio_codec_delete_ctrl_if(i2c_ctrl_if);
        audio_codec_delete_gpio_if(gpio_if);
    }
    return codec_dev;
}

#define LCD_CMD_BITS (8)
#define LCD_PARAM_BITS (8)
#define LCD_LEDC_CH (CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH)
#define LVGL_TICK_PERIOD_MS (CONFIG_BSP_DISPLAY_LVGL_TICK)
#define LVGL_MAX_SLEEP_MS (CONFIG_BSP_DISPLAY_LVGL_MAX_SLEEP)

esp_err_t bsp_display_brightness_init(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (panel_handle == NULL)
    {
        ESP_LOGE(TAG, "Panel handle is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (brightness_percent < 0 || brightness_percent > 100)
    {
        ESP_LOGE(TAG, "Invalid brightness percentage. Should be between 0 and 100.");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = ESP_OK;
    if (board_revision == BSP_BOARD_REVISION_ORIGINAL) {
        const uint8_t brightness =
            (uint8_t)((brightness_percent * 255) / 100);
        error = esp_lcd_panel_io_tx_param(
            io_handle, BSP_SH8601_QSPI_WRITE_CMD(0x51),
            &brightness, sizeof(brightness));
    } else {
        error = esp_lcd_panel_co5300_set_brightness(
            panel_handle, (uint8_t)brightness_percent);
    }
    if (error != ESP_OK) {
        board_diag_increment(&board_diagnostics.display_command_error);
        ESP_LOGE(
            TAG, "Panel brightness command failed (category %s)",
            esp_err_to_name(error));
        return error;
    }

    // A zero-brightness sleep can leave the CO5300 output stage inactive even
    // though no explicit display-off command was sent. Reassert display-on
    // after the nonzero brightness write on every wake. This command is safe
    // when the controller is already on and repairs the observed black panel.
    const bool waking_from_zero =
        brightness_percent > 0 && panel_brightness_is_zero;
    if (waking_from_zero)
    {
        error = esp_lcd_panel_disp_on_off(panel_handle, true);
        if (error != ESP_OK) {
            board_diag_increment(&board_diagnostics.display_command_error);
            ESP_LOGE(
                TAG, "Panel wake failed (category %s)",
                esp_err_to_name(error));
            return error;
        }
        if (!panel_display_on)
        {
            ESP_LOGI(TAG, "Panel on");
        }
        panel_display_on = true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    panel_brightness_is_zero = brightness_percent == 0;
    board_diag_increment(&board_diagnostics.display_command_ok);
    return ESP_OK;
}

esp_err_t bsp_display_recover(void)
{
    if (panel_handle == NULL)
    {
        ESP_LOGE(TAG, "Panel handle is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Do not pulse the shared reset outputs during an in-session wake. Replay
    // the selected controller table at zero brightness, then let the caller
    // send one complete frame before it restores brightness.
    const esp_err_t error = esp_lcd_panel_init(panel_handle);
    if (error != ESP_OK) {
        board_diag_increment(&board_diagnostics.display_recovery_error);
        ESP_LOGE(
            TAG, "Panel wake initialization failed (category %s)",
            esp_err_to_name(error));
        return error;
    }
    panel_display_on = true;
    panel_brightness_is_zero = true;
    board_diag_increment(&board_diagnostics.display_recovery_ok);
    ESP_LOGI(TAG, "Panel wake initialization complete");
    return ESP_OK;
}

esp_err_t bsp_display_backlight_off(void)
{
    ESP_LOGI(TAG, "Backlight off");
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    ESP_LOGI(TAG, "Backlight on");
    return bsp_display_brightness_set(100);
}

#if LVGL_VERSION_MAJOR >= 9
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}
#else
static void bsp_lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}
#endif

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    (void)config;
    ESP_RETURN_ON_ERROR(
        bsp_cold_reset_outputs(), TAG, "Cold panel reset is not safe");
    const bsp_board_revision_t revision = bsp_board_revision();
    panel_x_gap = revision == BSP_BOARD_REVISION_ORIGINAL
        ? 0
        : BSP_LCD_CST816S_X_GAP;

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = revision == BSP_BOARD_REVISION_ORIGINAL
        ? (spi_bus_config_t)SH8601_PANEL_BUS_QSPI_CONFIG(
              BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2,
              BSP_LCD_DATA3,
              BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8)
        : (spi_bus_config_t)CO5300_PANEL_BUS_QSPI_CONFIG(
              BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2,
              BSP_LCD_DATA3,
              BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8);
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO),
        TAG, "SPI bus start failed");

    esp_lcd_panel_io_spi_config_t io_config =
        revision == BSP_BOARD_REVISION_ORIGINAL
            ? (esp_lcd_panel_io_spi_config_t)
                  SH8601_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL)
            : (esp_lcd_panel_io_spi_config_t)
                  CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    // LVGL uses one draw buffer and waits for each flush. A deep queue only
    // keeps more temporary DMA copies alive and takes memory from TLS.
    io_config.trans_queue_depth = 2;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config,
            &io_handle),
        TAG, "Panel IO start failed");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
    };
    if (revision == BSP_BOARD_REVISION_ORIGINAL) {
        sh8601_vendor_config_t vendor_config = {
            .init_cmds = sh8601_init_cmds,
            .init_cmds_size =
                sizeof(sh8601_init_cmds) / sizeof(sh8601_init_cmds[0]),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        panel_config.vendor_config = &vendor_config;
        ESP_RETURN_ON_ERROR(
            esp_lcd_new_panel_sh8601(
                io_handle, &panel_config, &panel_handle),
            TAG, "SH8601 panel start failed");
    } else {
        co5300_vendor_config_t vendor_config = {
            .init_cmds = co5300_init_cmds,
            .init_cmds_size =
                sizeof(co5300_init_cmds) / sizeof(co5300_init_cmds[0]),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        panel_config.vendor_config = &vendor_config;
        ESP_RETURN_ON_ERROR(
            esp_lcd_new_panel_co5300(
                io_handle, &panel_config, &panel_handle),
            TAG, "CO5300 panel start failed");
    }
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_reset(panel_handle), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(panel_handle), TAG, "Panel initialization failed");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_set_gap(panel_handle, panel_x_gap, 0), TAG,
        "Panel gap setup failed");
    // Start the panel at zero brightness. LVGL can write the first black frame
    // without a visible white frame, and the UI raises brightness afterward.
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(panel_handle, true), TAG,
        "Panel-on command failed");
    panel_display_on = true;

    if (ret_panel)
    {
        *ret_panel = panel_handle;
    }
    if (ret_io)
    {
        *ret_io = io_handle;
    }
    return ESP_OK;
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    (void)config;
    ESP_RETURN_ON_FALSE(
        ret_touch != NULL, ESP_ERR_INVALID_ARG, TAG, "No touch output");
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    const bsp_board_revision_t revision = bsp_board_revision();
    if (revision == BSP_BOARD_REVISION_UNKNOWN) {
        ESP_LOGW(TAG, "Touch disabled for an unknown board revision");
        return ESP_ERR_NOT_FOUND;
    }

    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST, // Shared with LCD reset
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config;
    esp_err_t (*touch_new)(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *out_touch) = NULL;

    if (revision == BSP_BOARD_REVISION_V2) {
        tp_io_config =
            (esp_lcd_panel_io_i2c_config_t)
                ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
        touch_new = esp_lcd_touch_new_i2c_cst816s;
        ESP_LOGI(TAG, "Touch controller: CST820-compatible");
    } else {
        tp_io_config =
            (esp_lcd_panel_io_i2c_config_t)
                ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        touch_new = esp_lcd_touch_new_i2c_ft5x06;
        ESP_LOGI(TAG, "Touch controller: FT3168-compatible");
    }

    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
    const esp_err_t result = touch_new(tp_io_handle, &tp_cfg, ret_touch);
    if (result == ESP_OK) {
        // Managed reads log each I2C NACK before this adapter can apply its
        // bounded summary. Keep initialization logs, then make runtime reads
        // quiet and report only the local rate-limited category.
        esp_log_level_set(
            revision == BSP_BOARD_REVISION_V2 ? "CST816S" : "FT5x06",
            ESP_LOG_NONE);
    }
    return result;
}

/**************************************************************************************************
 *
 * IO Expander Function
 *
 **************************************************************************************************/
static esp_err_t bsp_io_expander_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C start failed");
    if (io_expander != NULL) {
        return ESP_OK;
    }
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_IO_EXPANDER_I2C_ADDRESS,
        .scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ,
        .scl_wait_us = 0,
        .flags = {},
    };
    return i2c_master_bus_add_device(i2c_handle, &config, &io_expander);
}

static esp_err_t bsp_io_expander_read_reg(uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(
        value != NULL, ESP_ERR_INVALID_ARG, TAG, "No expander read output");
    return i2c_master_transmit_receive(
        io_expander, &reg, sizeof(reg), value, sizeof(*value),
        BSP_IO_EXPANDER_TIMEOUT_MS);
}

static esp_err_t bsp_io_expander_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(
        io_expander, data, sizeof(data), BSP_IO_EXPANDER_TIMEOUT_MS);
}

static esp_err_t bsp_io_expander_update_reg(
    uint8_t reg, uint8_t mask, bool set_bits)
{
    uint8_t current = 0;
    ESP_RETURN_ON_ERROR(
        bsp_io_expander_read_reg(reg, &current), TAG,
        "IO expander register read failed");
    const uint8_t next = set_bits
        ? (uint8_t)(current | mask)
        : (uint8_t)(current & (uint8_t)~mask);
    if (next == current) {
        return ESP_OK;
    }
    return bsp_io_expander_write_reg(reg, next);
}

static esp_err_t bsp_cold_reset_outputs(void)
{
    if (cold_reset_finished) {
        return cold_reset_result;
    }
    cold_reset_finished = true;
    cold_reset_attempts = 1;
    cold_reset_result = bsp_io_expander_init();
    if (cold_reset_result == ESP_OK) {
        cold_reset_result = bsp_io_expander_update_reg(
            BSP_IO_EXPANDER_OUTPUT_REG, BSP_EXIO_COLD_RESET_MASK, false);
    }
    if (cold_reset_result == ESP_OK) {
        cold_reset_result = bsp_io_expander_update_reg(
            BSP_IO_EXPANDER_DIRECTION_REG, BSP_EXIO_COLD_RESET_MASK, false);
    }
    if (cold_reset_result == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Always make one bounded safe-release attempt after the low request.
    // A failed release must stop panel creation instead of leaving reset low.
    if (io_expander != NULL) {
        const esp_err_t release_result = bsp_io_expander_update_reg(
            BSP_IO_EXPANDER_OUTPUT_REG, BSP_EXIO_COLD_RESET_MASK, true);
        if (release_result != ESP_OK) {
            cold_reset_result = release_result;
        }
    }
    if (cold_reset_result == ESP_OK) {
        ESP_LOGI(
            TAG, "Panel and touch cold reset complete after %u attempt",
            (unsigned)cold_reset_attempts);
    }
    return cold_reset_result;
}

esp_err_t bsp_power_button_init(void)
{
    ESP_RETURN_ON_ERROR(
        bsp_io_expander_init(), TAG, "IO expander start failed");
    return bsp_io_expander_update_reg(
        BSP_IO_EXPANDER_DIRECTION_REG, BSP_EXIO_POWER_BUTTON, true);
}

esp_err_t bsp_power_button_is_pressed(bool *pressed)
{
    ESP_RETURN_ON_FALSE(
        pressed != NULL, ESP_ERR_INVALID_ARG, TAG,
        "No PWR button output");
    ESP_RETURN_ON_FALSE(
        io_expander != NULL, ESP_ERR_INVALID_STATE, TAG,
        "PWR button is not initialized");
    uint8_t levels = 0;
    ESP_RETURN_ON_ERROR(
        bsp_io_expander_read_reg(BSP_IO_EXPANDER_INPUT_REG, &levels),
        TAG, "PWR button read failed");
    *pressed = (levels & BSP_EXIO_POWER_BUTTON) != 0;
    return ESP_OK;
}

esp_err_t bsp_mode_button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

esp_err_t bsp_mode_button_is_pressed(bool *pressed)
{
    ESP_RETURN_ON_FALSE(pressed != NULL, ESP_ERR_INVALID_ARG, TAG, "No mode button output");
    *pressed = gpio_get_level(GPIO_NUM_0) == 0;
    return ESP_OK;
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    bsp_display_config_t disp_config = {0};

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&disp_config, &panel_handle, &io_handle));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = cfg->buffer_size,
        .double_buffer = cfg->double_buffer,

        .monochrome = false,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif

        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .sw_rotate = true,
            .buff_dma = cfg->flags.buff_dma,
#if CONFIG_BSP_DISPLAY_LVGL_PSRAM
            .buff_spiram = cfg->flags.buff_spiram,
#endif
#if CONFIG_BSP_DISPLAY_LVGL_FULL_REFRESH
            .full_refresh = 1,
#elif CONFIG_BSP_DISPLAY_LVGL_DIRECT_MODE
            .direct_mode = 1,
#endif
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
        }};
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp)
    {
        return NULL;
    }

#if LVGL_VERSION_MAJOR >= 9
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#else
    lv_disp_t *disp_v8 = (lv_disp_t *)disp;
    if (disp_v8 && disp_v8->driver)
    {
        disp_v8->driver->rounder_cb = bsp_lvgl_rounder_cb;
    }
#endif

    return disp;
}

static void bsp_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->state = LV_INDEV_STATE_RELEASED;
    esp_lcd_touch_handle_t touch =
        (esp_lcd_touch_handle_t)lv_indev_get_driver_data(indev);
    if (touch == NULL) {
        board_diag_increment(&board_diagnostics.touch_read_error);
        return;
    }

    esp_err_t error = esp_lcd_touch_read_data(touch);
    esp_lcd_touch_point_data_t point = {0};
    uint8_t point_count = 0;
    if (error == ESP_OK) {
        error = esp_lcd_touch_get_data(
            touch, &point, &point_count, 1);
    }
    if (error != ESP_OK) {
        board_diag_increment(&board_diagnostics.touch_read_error);
        const uint32_t now_ms =
            (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (touch_error_logged_at_ms == 0 ||
            now_ms - touch_error_logged_at_ms >=
                BSP_TOUCH_ERROR_LOG_INTERVAL_MS) {
            touch_error_logged_at_ms = now_ms;
            ESP_LOGW(
                TAG, "Touch read unavailable (category %s)",
                esp_err_to_name(error));
        }
        return;
    }

    board_diag_increment(&board_diagnostics.touch_read_ok);
    if (point_count > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}

static lv_indev_t *bsp_display_indev_init(lv_display_t *disp)
{
    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(NULL, &tp));
    BSP_NULL_CHECK(tp, NULL);

    if (!lvgl_port_lock(1000)) {
        board_diag_increment(&board_diagnostics.display_lock_timeout);
        return NULL;
    }
    lv_indev_t *indev = lv_indev_create();
    if (indev != NULL) {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_disp(indev, disp);
        lv_indev_set_driver_data(indev, tp);
        lv_indev_set_read_cb(indev, bsp_touchpad_read);
    }
    lvgl_port_unlock();
    return indev;
}

/**********************************************************************************************************
 *
 * Display Function
 *
 **********************************************************************************************************/
lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * LVGL_BUFFER_HEIGHT,
        .double_buffer = false,
        .flags = {
            // The QSPI panel driver otherwise allocates a temporary DMA
            // buffer for each flush. Keep a smaller persistent internal
            // buffer so audio, Wi-Fi, and BLE have enough internal memory.
            .buff_dma = true,
            .buff_spiram = false,
        }};
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    lv_display_t *disp = NULL;

    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(lvgl_port_init(&cfg->lvgl_port_cfg));

    /* The LVGL task starts before the default display exists. Hold its lock
     * while panel polling commands and brightness setup finish. */
    if (!lvgl_port_lock(0))
    {
        return NULL;
    }
    disp = bsp_display_lcd_init(cfg);
    if (disp != NULL && bsp_display_brightness_init() != ESP_OK)
    {
        disp = NULL;
    }
    lvgl_port_unlock();

    return disp;
}

esp_err_t bsp_display_start_touch(lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "No display for touch");
    if (disp_indev != NULL) {
        return ESP_OK;
    }
    disp_indev = bsp_display_indev_init(disp);
    return disp_indev != NULL ? ESP_OK : ESP_FAIL;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    const bool locked = lvgl_port_lock(timeout_ms);
    if (!locked) {
        board_diag_increment(&board_diagnostics.display_lock_timeout);
    }
    return locked;
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}
