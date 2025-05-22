#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "audio_player.h"
//SD
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>
//GPIO
#include "driver/gpio.h"
#include <sys/time.h>
#include "esp_timer.h"


#include "driver/adc.h"


static const char *TAG = "usb_audio_player";
static const char *SDTAG = "SD TEST";

#define PAUSE_GPIO GPIO_NUM_0
#define RESUME_GPIO GPIO_NUM_45
#define STOP_GPIO GPIO_NUM_48

#define SD_PIN_NUM_CLK 3
#define SD_PIN_NUM_CMD 4
#define SD_PIN_NUM_D0  14
#define MOUNT_POINT "/sdcard"

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2
#define SPIFFS_BASE             "/sdcard"

#define BIT1_SPK_START          (0x01 << 0)
#define DEFAULT_VOLUME          20
#define DEFAULT_UAC_FREQ        48000
#define DEFAULT_UAC_BITS        16
#define DEFAULT_UAC_CH          2

#define MAX_FILES 2048

char *music_file_name;
char *music_files[MAX_FILES];  
int music_count = 0;

static QueueHandle_t s_event_queue = NULL;
static uac_host_device_handle_t s_spk_dev_handle = NULL;
static uint32_t s_spk_curr_freq = DEFAULT_UAC_FREQ;
static uint8_t s_spk_curr_bits = DEFAULT_UAC_BITS;
static uint8_t s_spk_curr_ch = DEFAULT_UAC_CH;
static FILE *s_fp = NULL;
static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg);
/**
 * @brief event group
 *
 * APP_EVENT            - General control event
 * UAC_DRIVER_EVENT     - UAC Host Driver event, such as device connection
 * UAC_DEVICE_EVENT     - UAC Host Device event, such as rx/tx completion, device disconnection
 */
typedef enum {
    APP_EVENT = 0,
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

/**
 * @brief event queue
 *
 * This event is used for delivering the UAC Host event from callback to the uac_lib_task
 */
typedef struct {
    event_group_t event_group;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_handle_t handle;
            uac_host_driver_event_t event;
            void *arg;
        } device_evt;
    };
} s_event_queue_t;


void button_task(void *arg)
{
    ESP_LOGI(TAG, "button_task started");

    while (1) {
        if (gpio_get_level(PAUSE_GPIO) == 0) {
            while (gpio_get_level(PAUSE_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            ESP_LOGI(TAG, "next song--------------------------------------------------------");
            int index = random() % music_count; 
            music_file_name = music_files[index];
            // audio_player_pause();
            audio_player_stop();
            // fclose(s_fp);
            // s_fp = fopen(music_file_name, "rb");
            // audio_player_play(s_fp);
            // audio_player_stop();
            vTaskDelay(pdMS_TO_TICKS(80));
        }

        vTaskDelay(pdMS_TO_TICKS(30)); 
    }
}

void button_gpio_init()
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PAUSE_GPIO) | (1ULL << RESUME_GPIO) | (1ULL << STOP_GPIO),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&io_conf);
}


static void sd_init(void) {
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(SDTAG, "Initializing SD card using SDMMC 1-bit mode");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // 1-bit mode

    slot_config.clk = SD_PIN_NUM_CLK;   // CLK
    slot_config.cmd = SD_PIN_NUM_CMD;   // CMD
    slot_config.d0  = SD_PIN_NUM_D0;    // D0
    // D1~D3 不使用

    gpio_set_pull_mode(slot_config.clk, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(slot_config.cmd, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(slot_config.d0, GPIO_PULLUP_ONLY);

    ESP_LOGI(SDTAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(SDTAG, "Failed to mount filesystem. (%s)", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(SDTAG, "Filesystem mounted");
    sdmmc_card_print_info(stdout, card);
}

static esp_err_t _audio_player_mute_fn(AUDIO_PLAYER_MUTE_SETTING setting)
{
    if (s_spk_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "mute setting: %s", setting == AUDIO_PLAYER_MUTE ? "mute" : "unmute");
    // some uac devices may not support mute, so we not check the return value
    if (setting == AUDIO_PLAYER_UNMUTE) {
        uac_host_device_set_volume(s_spk_dev_handle, DEFAULT_VOLUME);
        uac_host_device_set_mute(s_spk_dev_handle, false);
    } else {
        uac_host_device_set_volume(s_spk_dev_handle, 0);
        uac_host_device_set_mute(s_spk_dev_handle, true);
    }
    return ESP_OK;
}

static esp_err_t _audio_player_write_fn(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (s_spk_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *bytes_written = 0;
    // int16_t *samples = (int16_t *)audio_buffer;
    // ESP_LOGI(TAG, "First two samples: L[0]=%d, R[0]=%d, L[1]=%d, R[1]=%d",
    //          samples[0], samples[1], samples[2], samples[3]);
    esp_err_t ret = uac_host_device_write(s_spk_dev_handle, audio_buffer, len, timeout_ms);
    if (ret == ESP_OK) {
        *bytes_written = len;
    }
    return ret;
}

static esp_err_t _audio_player_std_clock(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    if (s_spk_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rate == s_spk_curr_freq && bits_cfg == s_spk_curr_bits && ch == s_spk_curr_ch) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Re-config: speaker rate %"PRIu32", bits %"PRIu32", mode %s", rate, bits_cfg, ch == 1 ? "MONO" : (ch == 2 ? "STEREO" : "INVALID"));
    ESP_ERROR_CHECK(uac_host_device_stop(s_spk_dev_handle));
    const uac_host_stream_config_t stm_config = {
        .channels = ch,
        .bit_resolution = bits_cfg,
        .sample_freq = rate,
    };
    s_spk_curr_freq = rate;
    s_spk_curr_bits = bits_cfg;
    s_spk_curr_ch = ch;
    return uac_host_device_start(s_spk_dev_handle, &stm_config);
}

static void _audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    ESP_LOGI(TAG, "ctx->audio_event = %d", ctx->audio_event);
    switch (ctx->audio_event) {
    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE: {
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_IDLE");
        if (s_spk_dev_handle == NULL) {
            break;
        }
        ESP_ERROR_CHECK(uac_host_device_suspend(s_spk_dev_handle));
        ESP_LOGW(TAG, "Play in loop"); 
        s_fp = fopen(music_file_name, "rb");
        if (s_fp) {
            ESP_LOGI(TAG, "2 Playing '%s'", music_file_name);
            audio_player_play(s_fp);
        } else {
            ESP_LOGE(TAG, "unable to open filename callback '%s'", music_file_name);
        }
        break;
    }
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_PLAY");
        if (s_spk_dev_handle == NULL) {
            break;
        }
        ESP_ERROR_CHECK(uac_host_device_resume(s_spk_dev_handle));
        break;
    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
        ESP_LOGI(TAG, "AUDIO_PLAYER_REQUEST_PAUSE");
        break;
    default:
        break;
    }
}

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        // stop audio player first
        s_spk_dev_handle = NULL;
        audio_player_stop();
        ESP_LOGI(TAG, "UAC Device disconnected");
        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
        return;
    }
    // Send uac device event to the event queue
    s_event_queue_t evt_queue = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };
    // should not block here
    xQueueSend(s_event_queue, &evt_queue, 0);
}

static void uac_host_lib_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    // Send uac driver event to the event queue
    s_event_queue_t evt_queue = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    xQueueSend(s_event_queue, &evt_queue, 0);
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive(arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // In this example, there is only one client registered
        // So, once we deregister the client, this call must succeed with ESP_OK
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB Host shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

static void uac_lib_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
    s_event_queue_t evt_queue = {0};
    while (1) {
        if (xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY)) {
            if (UAC_DRIVER_EVENT ==  evt_queue.event_group) {
                uac_host_driver_event_t event = evt_queue.driver_evt.event;
                uint8_t addr = evt_queue.driver_evt.addr;
                uint8_t iface_num = evt_queue.driver_evt.iface_num;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_TX_CONNECTED: {
                    uac_host_dev_info_t dev_info;
                    uac_host_device_handle_t uac_device_handle = NULL;
                    const uac_host_device_config_t dev_config = {
                        .addr = addr,
                        .iface_num = iface_num,
                        .buffer_size = 16000,
                        .buffer_threshold = 4000,
                        .callback = uac_device_callback,
                        .callback_arg = NULL,
                    };
                    ESP_ERROR_CHECK(uac_host_device_open(&dev_config, &uac_device_handle));
                    ESP_ERROR_CHECK(uac_host_get_device_info(uac_device_handle, &dev_info));
                    ESP_LOGI(TAG, "UAC Device connected: SPK");
                    uac_host_printf_device_param(uac_device_handle);
                    // Start usb speaker with the default configuration
                    const uac_host_stream_config_t stm_config = {
                        .channels = s_spk_curr_ch,
                        .bit_resolution = s_spk_curr_bits,
                        .sample_freq = s_spk_curr_freq,
                    };
                    ESP_ERROR_CHECK(uac_host_device_start(uac_device_handle, &stm_config));
                    s_spk_dev_handle = uac_device_handle;
                    s_fp = fopen(music_file_name, "rb");
                    if (s_fp) {
                        ESP_LOGI(TAG, "1 Playing '%s'", music_file_name);
                        audio_player_play(s_fp);
                    } else {
                        ESP_LOGE(TAG, "unable to open filename uac play'%s'", music_file_name);
                    }
                    break;
                }
                case UAC_HOST_DRIVER_EVENT_RX_CONNECTED: {
                    // we don't support MIC in this example
                    ESP_LOGI(TAG, "UAC Device connected: MIC");
                    break;
                }
                default:
                    break;
                }
            } else if (UAC_DEVICE_EVENT == evt_queue.event_group) {
                uac_host_device_event_t event = evt_queue.device_evt.event;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                    s_spk_curr_bits = DEFAULT_UAC_BITS;
                    s_spk_curr_freq = DEFAULT_UAC_FREQ;
                    s_spk_curr_ch = DEFAULT_UAC_CH;
                    ESP_LOGI(TAG, "UAC Device disconnected");
                    break;
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                    break;
                case UAC_HOST_DEVICE_EVENT_TX_DONE:
                    break;
                case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                    break;
                default:
                    break;
                }
            } else if (APP_EVENT == evt_queue.event_group) {
                break;
            }
        }
    }

    ESP_LOGI(TAG, "UAC Driver uninstall");
    ESP_ERROR_CHECK(uac_host_uninstall());
}

void scan_music_files_recursive(const char *base_path) {
    DIR *dir = opendir(base_path);
    struct dirent *entry;

    if (!dir) {
        ESP_LOGE(TAG, "Failed to open dir: %s", base_path);
        return;
    }

    while ((entry = readdir(dir)) != NULL && music_count < MAX_FILES) {
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);

        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                scan_music_files_recursive(full_path);
            }
        } else {
            if (strstr(entry->d_name, ".MP3") || strstr(entry->d_name, ".mp3") ||
                strstr(entry->d_name, ".WAV") || strstr(entry->d_name, ".wav")) {
                // ignore filename with startsWith "."
                if (entry->d_name[0] == '.') {
                    continue;
                }
                music_files[music_count] = strdup(full_path);
                music_count++;
            }
        }
    }

    closedir(dir);
}

void scan_music_files() {
    music_count = 0;
    scan_music_files_recursive("/sdcard");
    ESP_LOGI(TAG, "Found %d music(s)", music_count);
    // ESP_LOGI(TAG, "%s", music_files[0]);

    unsigned int seed = (unsigned int)esp_timer_get_time();
    srandom(seed);
    int index = random() % music_count; 
    music_file_name = music_files[index]; 
}

void app_main(void)
{
    sd_init();
    scan_music_files();

    button_gpio_init();
    xTaskCreate(button_task, "button_task", 4096, NULL, 8, NULL);

    s_event_queue = xQueueCreate(10, sizeof(s_event_queue_t));
    assert(s_event_queue != NULL);

    audio_player_config_t config = {.mute_fn = _audio_player_mute_fn,
                                    .write_fn = _audio_player_write_fn,
                                    .clk_set_fn = _audio_player_std_clock,
                                    .priority = 1
                                   };
    ESP_ERROR_CHECK(audio_player_new(config));
    ESP_ERROR_CHECK(audio_player_callback_register(_audio_player_callback, NULL));

    static TaskHandle_t uac_task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(uac_lib_task, "uac_events", 4096, NULL,
                                             USER_TASK_PRIORITY, &uac_task_handle, 0);
    assert(ret == pdTRUE);
    ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, (void *)uac_task_handle,
                                  USB_HOST_TASK_PRIORITY, NULL, 0);
    assert(ret == pdTRUE);
    

    
    while (1) {
        vTaskDelay(1000);
    }

}
