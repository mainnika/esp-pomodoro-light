#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "tinyfsm.hpp"

static const char *TAG = "pomodoro";

static void periodic_timer_callback(void *arg);

#if CONFIG_WIFI_POWER_SAVE_MIN_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_WIFI_POWER_SAVE_MAX_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MAX_MODEM
#elif CONFIG_WIFI_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif /*CONFIG_POWER_SAVE_MODEM*/

esp_err_t wifi_connect(void);

extern "C"
{
    void app_main(void);
}

struct Off; // forward declaration
struct Toggle : tinyfsm::Event
{
};

struct Switch : tinyfsm::Fsm<Switch>
{
    virtual void react(Toggle const &) {};

    // alternative: enforce handling of Toggle in all states (pure virtual)
    // virtual void react(Toggle const &) = 0;

    virtual void entry(void) {}; /* entry actions in some states */
    void exit(void) {};          /* no exit actions */

    // alternative: enforce entry actions in all states (pure virtual)
    // virtual void entry(void) = 0;
};

// ----------------------------------------------------------------------------
// 3. State Declarations
//
struct On : Switch
{
    void entry() override { ESP_LOGI(TAG, "Switch is ON"); };
    void react(Toggle const &) override { transit<Off>(); };
};

struct Off : Switch
{
    void entry() override { ESP_LOGI(TAG, "Switch is OFF"); };
    void react(Toggle const &) override { transit<On>(); };
};

FSM_INITIAL_STATE(Switch, Off)

using fsm_handle = Switch;

Toggle toggle;
esp_timer_handle_t periodic_timer;

esp_err_t start_timer()
{
    esp_timer_create_args_t periodic_timer_args = {};

    periodic_timer_args.callback = &periodic_timer_callback;
    periodic_timer_args.name = "dispatch_timer";

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));

    /* Start the timers */
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000000)); // 1s

    return ESP_OK;
}

void app_main(void)
{
    fsm_handle::start();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(wifi_connect());
    ESP_ERROR_CHECK(start_timer());

    esp_wifi_set_ps(DEFAULT_PS_MODE);

#if CONFIG_PM_ENABLE
    // Configure dynamic frequency scaling:
    // maximum and minimum frequencies are set in sdkconfig,
    // automatic light sleep is enabled if tickless idle support is enabled.
    esp_pm_config_esp8266_t pm_config = {
        .light_sleep_enable = true};
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
#endif // CONFIG_PM_ENABLE
}

static void periodic_timer_callback(void *arg)
{
    int64_t time_since_boot = esp_timer_get_time();
    ESP_LOGI(TAG, "Periodic timer called, time since boot: %lld us", time_since_boot);

    fsm_handle::dispatch(toggle);
}