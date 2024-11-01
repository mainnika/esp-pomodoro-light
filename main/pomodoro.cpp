#include <string.h>
#include <inttypes.h>

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
struct Idle;
struct Work;
struct ShortBreak;
struct LongBreak;

// Triggered when the timer is ready to start.
struct TimerReady : tinyfsm::Event
{
};
// Triggered to start the work period.
struct StartTimer : tinyfsm::Event
{
};
// Triggered to check the timer.
struct CheckTimer : tinyfsm::Event
{
    int64_t time_since_boot;
};
// Triggered when the timer finishes counting down.
struct TimerComplete : tinyfsm::Event
{
};
// Triggered to reset the timer to the initial state.
struct ResetTimer : tinyfsm::Event
{
};

struct Pomodoro : tinyfsm::Fsm<Pomodoro>
{
    virtual void react(StartTimer const &) {};
    virtual void react(TimerComplete const &) {};
    virtual void react(ResetTimer const &) = 0;
    virtual void react(TimerReady const &) {};
    virtual void react(CheckTimer const &) {};

    virtual void entry(void)
    {
        int64_t time_since_boot = esp_timer_get_time();
        ESP_LOGI(TAG, "Pomodoro timer started at %" PRIu64 " us", time_since_boot);

        Pomodoro::state_entry_time = time_since_boot;
    }; /* entry actions in some states */
    void exit(void) {}; /* no exit actions */

protected:
    static size_t short_breaks;
    static size_t long_breaks;
    static int64_t state_entry_time;

    static constexpr int64_t WORK_PERIOD = 1 * 60 * 1000000;    // 1 minute
    static constexpr int64_t SHORT_BREAK_PERIOD = 10 * 1000000; // 10 seconds
    static constexpr int64_t LONG_BREAK_PERIOD = 30 * 1000000;  // 30 seconds
};

size_t Pomodoro::short_breaks = 0;
size_t Pomodoro::long_breaks = 0;
int64_t Pomodoro::state_entry_time = 0;
constexpr int64_t Pomodoro::WORK_PERIOD;
constexpr int64_t Pomodoro::SHORT_BREAK_PERIOD;
constexpr int64_t Pomodoro::LONG_BREAK_PERIOD;

// ----------------------------------------------------------------------------
// 3. State Declarations
//
// The off state where the timer is completely stopped.
struct Off : Pomodoro
{
    void entry() override
    {
        Pomodoro::entry();
        ESP_LOGI(TAG, "starting timer");
    };
    void react(ResetTimer const &) override {};

    void react(TimerReady const &) override { transit<Idle>(); };
};

// The initial state where the timer is ready to start.
struct Idle : Pomodoro
{
    void entry() override
    {
        Pomodoro::entry();
        ESP_LOGI(TAG, "timer is ready, idle");
    };
    void react(ResetTimer const &) override { transit<Idle>(); };

    void react(StartTimer const &) override { transit<Work>(); };
};
// The state where the timer is counting down the work period.
struct Work : Pomodoro
{
    void entry() override
    {
        Pomodoro::entry();
        ESP_LOGI(TAG, "timer is counting");
    };
    void react(ResetTimer const &) override { transit<Idle>(); };

    void react(CheckTimer const &check_timer_event) override
    {
        int64_t time_since_boot = check_timer_event.time_since_boot;
        if (time_since_boot - Pomodoro::state_entry_time < Pomodoro::WORK_PERIOD)
        {
            ESP_LOGI(TAG, "work time left: %" PRIu64 "", Pomodoro::WORK_PERIOD - (time_since_boot - Pomodoro::state_entry_time));
            return;
        }

        if (Pomodoro::short_breaks == 4)
        {
            transit<LongBreak>();
        }
        else
        {
            transit<ShortBreak>();
        }
    };
};
// The state where the timer is counting down a short break period.
struct ShortBreak : Pomodoro
{
    void entry() override
    {
        Pomodoro::entry();
        Pomodoro::short_breaks++;
        ESP_LOGI(TAG, "short break %d", Pomodoro::short_breaks);
    };
    void react(ResetTimer const &) override { transit<Idle>(); };

    void react(CheckTimer const &check_timer_event) override
    {
        int64_t time_since_boot = check_timer_event.time_since_boot;
        if (time_since_boot - Pomodoro::state_entry_time < Pomodoro::SHORT_BREAK_PERIOD)
        {
            ESP_LOGI(TAG, "short break time left: %" PRIu64 "", Pomodoro::SHORT_BREAK_PERIOD - (time_since_boot - Pomodoro::state_entry_time));
            return;
        }

        transit<Work>();
    };
};
// The state where the timer is counting down a long break period.
struct LongBreak : Pomodoro
{
    void entry() override
    {
        Pomodoro::entry();
        Pomodoro::short_breaks = 0;
        ESP_LOGI(TAG, "long break");
    };
    void react(ResetTimer const &) override { transit<Idle>(); };

    void react(CheckTimer const &check_timer_event) override
    {
        int64_t time_since_boot = check_timer_event.time_since_boot;
        if (time_since_boot - Pomodoro::state_entry_time < Pomodoro::LONG_BREAK_PERIOD)
        {
            ESP_LOGI(TAG, "long break time left: %" PRIu64 "", Pomodoro::LONG_BREAK_PERIOD - (time_since_boot - Pomodoro::state_entry_time));
            return;
        }

        transit<Work>();
    };
};

FSM_INITIAL_STATE(Pomodoro, Off)

using fsm_handle = Pomodoro;

esp_timer_handle_t periodic_timer;
TimerReady timer_ready_event;
CheckTimer check_timer_event;
StartTimer start_timer_event;
ResetTimer reset_timer_event;

esp_err_t start_timer()
{
    fsm_handle::dispatch(timer_ready_event);
    fsm_handle::dispatch(start_timer_event);

    esp_timer_create_args_t periodic_timer_args = {};

    periodic_timer_args.callback = &periodic_timer_callback;
    periodic_timer_args.name = "dispatch_timer";

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));

    /* Start the timers */
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000000));

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
    ESP_LOGI(TAG, "Periodic timer called, time since boot: %" PRIu64 " us", time_since_boot);

    check_timer_event.time_since_boot = time_since_boot;
    fsm_handle::dispatch(check_timer_event);
}