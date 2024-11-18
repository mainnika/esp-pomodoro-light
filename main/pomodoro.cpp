#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "gpio.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "tinyfsm.hpp"

static const char *TAG = "pomodoro";

#if CONFIG_WIFI_POWER_SAVE_MIN_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MIN_MODEM
#elif CONFIG_WIFI_POWER_SAVE_MAX_MODEM
#define DEFAULT_PS_MODE WIFI_PS_MAX_MODEM
#elif CONFIG_WIFI_POWER_SAVE_NONE
#define DEFAULT_PS_MODE WIFI_PS_NONE
#else
#define DEFAULT_PS_MODE WIFI_PS_NONE
#endif /*CONFIG_POWER_SAVE_MODEM*/

#define ESP_INTR_FLAG_DEFAULT 0

#define GPIO_ACTION_BUTTON GPIO_NUM_2

#define GPIO_LIGHT_GREEN GPIO_NUM_13
#define GPIO_LIGHT_YELLOW GPIO_NUM_12
#define GPIO_LIGHT_RED GPIO_NUM_14

// #define LONG_BREAK_ENABLE 1

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
struct LongBreakLastMinutes;

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
};
// Triggered when the timer finishes counting down.
struct TimerComplete : tinyfsm::Event
{
};
// Triggered to reset the timer to the initial state.
struct ResetTimer : tinyfsm::Event
{
};
struct TimerAction : tinyfsm::Event
{
};

static TimerReady timer_ready_event;
static StartTimer start_timer_event;
static CheckTimer check_timer_event;
static TimerComplete timer_complete_event;
static ResetTimer reset_timer_event;
static TimerAction timer_action_event;

struct Pomodoro : tinyfsm::Fsm<Pomodoro>
{
    static constexpr int64_t WORK_PERIOD_SECONDS = 45 * 60;
    static constexpr int64_t SHORT_BREAK_PERIOD_SECONDS = 15 * 60;
    static constexpr int64_t LONG_BREAK_PERIOD_SECONDS = 30 * 60;
    static constexpr int64_t LONG_BREAK_AFTER = 4;

    virtual void react(StartTimer const &) {};
    virtual void react(TimerComplete const &) {};
    virtual void react(ResetTimer const &) = 0;
    virtual void react(TimerReady const &) {};
    virtual void react(CheckTimer const &) {};
    virtual void react(TimerAction const &) {};

    virtual void entry(void) {};

    void exit(void) {};

private:
    size_t short_breaks;
    size_t long_breaks;
    int64_t counting_started_at;
    int64_t pause_started_at;
    bool timer_active;

public:
    void start_counting()
    {
        if (this->timer_active)
        {
            return;
        }
        if (this->pause_started_at > 0)
        {
            int64_t time_since_boot = esp_timer_get_time();

            this->counting_started_at += time_since_boot - this->pause_started_at;
            this->pause_started_at = 0;
            this->timer_active = true;

            ESP_LOGI(TAG, "pomodoro timer resumed counting at %" PRIu64 " sec", time_since_boot / 1000000);
            return;
        }

        int64_t time_since_boot = esp_timer_get_time();

        this->counting_started_at = time_since_boot;
        this->pause_started_at = 0;
        this->timer_active = true;

        ESP_LOGI(TAG, "pomodoro timer started counting at %" PRIu64 " sec", time_since_boot / 1000000);
        return;
    };

    void pause_counting()
    {
        if (!this->timer_active)
        {
            ESP_LOGI(TAG, "pomodoro timer cannot pause, not counting");
            return;
        }

        int64_t time_since_boot = esp_timer_get_time();

        this->timer_active = false;
        this->pause_started_at = time_since_boot;

        ESP_LOGI(TAG, "pomodoro timer paused counting at %" PRIu64 " sec", this->get_counting_seconds());
    };

    void reset_counting()
    {
        this->counting_started_at = 0;
        this->pause_started_at = 0;
        this->timer_active = false;

        ESP_LOGI(TAG, "pomodoro timer reset");
    };

    void reset_short_breaks()
    {
        this->short_breaks = 0;
    };

    int64_t get_counting_seconds()
    {
        if (this->pause_started_at > 0)
        {
            return (this->pause_started_at - this->counting_started_at) / 1000000;
        }
        if (this->counting_started_at > 0)
        {
            int64_t time_since_boot = esp_timer_get_time();
            int64_t elapsed_time_in_seconds = time_since_boot - this->counting_started_at;
            return elapsed_time_in_seconds / 1000000;
        }

        return 0;
    };

    bool is_timer_active()
    {
        return this->timer_active;
    };

    bool is_paused()
    {
        return this->pause_started_at > 0;
    };

    bool is_started()
    {
        return this->counting_started_at > 0;
    };

    void add_short_break()
    {
        this->short_breaks++;
    };

    void add_long_break()
    {
        this->long_breaks++;
    };

    size_t get_short_breaks()
    {
        return this->short_breaks;
    };

    size_t get_long_breaks()
    {
        return this->long_breaks;
    };

    size_t get_short_breaks_left()
    {
        return Pomodoro::LONG_BREAK_AFTER - this->short_breaks;
    };
};

constexpr int64_t Pomodoro::WORK_PERIOD_SECONDS;
constexpr int64_t Pomodoro::SHORT_BREAK_PERIOD_SECONDS;
constexpr int64_t Pomodoro::LONG_BREAK_PERIOD_SECONDS;
constexpr int64_t Pomodoro::LONG_BREAK_AFTER;

// ----------------------------------------------------------------------------
// 3. State Declarations
//
// The off state where the timer is completely stopped.
struct Off : Pomodoro
{
    void entry() override
    {
        ESP_LOGI(TAG, "starting timer");
    };
    void react(ResetTimer const &) override
    {
        ESP_LOGI(TAG, "timer not ready");
    };
    void react(TimerReady const &) override
    {
        ESP_LOGI(TAG, "got timer ready event");

        return transit<Idle>();
    };

public:
};

// The initial state where the timer is ready to start.
struct Idle : Pomodoro
{
    void entry() override
    {
        this->reset_counting();
        ESP_LOGI(TAG, "timer is ready, idle");
    };
    void react(ResetTimer const &) override { return transit<Work>(); };

    void react(StartTimer const &) override { return transit<Work>(); };

    void react(TimerAction const &) override { return transit<Work>(); };
};

// The state where the timer is counting down the work period.
struct Work : Pomodoro
{
    void entry() override
    {
        this->reset_counting();
        ESP_LOGI(TAG, "let's get to work");
    };
    void react(ResetTimer const &) override { return transit<Idle>(); };

    void react(CheckTimer const &) override
    {
        if (!this->is_timer_active())
        {
            return;
        }

        int64_t elapsed_time_in_seconds = this->get_counting_seconds();
        if (elapsed_time_in_seconds < Pomodoro::WORK_PERIOD_SECONDS)
        {
            ESP_LOGI(TAG, "work time left: %" PRIu64 " sec", Pomodoro::WORK_PERIOD_SECONDS - elapsed_time_in_seconds);
            return;
        }

#ifdef LONG_BREAK_ENABLE
        bool is_last_work = this->get_short_breaks_left() == 0;
        if (is_last_work)
        {
            transit<LongBreak>();
        }
        else
#endif
        {
            transit<ShortBreak>();
        }
    };

    void react(TimerAction const &) override
    {
        if (!this->is_started())
        {
            this->start_counting();
            return;
        }

        if (this->is_paused())
        {
            this->start_counting();
            return;
        }

        this->pause_counting();
    };
};
// The state where the timer is counting down a short break period.
struct ShortBreak : Pomodoro
{
    void entry() override
    {
        this->reset_counting();
        this->add_short_break();
        ESP_LOGI(TAG, "short break, amount left: %" PRIu32 "", this->get_short_breaks_left());
    };
    void react(ResetTimer const &) override { transit<Idle>(); };

    void react(CheckTimer const &) override
    {
        if (!this->is_timer_active())
        {
            return;
        }

        int64_t elapsed_time_in_seconds = this->get_counting_seconds();
        if (elapsed_time_in_seconds < Pomodoro::SHORT_BREAK_PERIOD_SECONDS)
        {
            ESP_LOGI(TAG, "short break time left: %" PRIu64 " sec", Pomodoro::SHORT_BREAK_PERIOD_SECONDS - elapsed_time_in_seconds);
            return;
        }

        transit<Work>();
    };

    void react(TimerAction const &) override
    {
        if (!this->is_started())
        {
            this->start_counting();
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
        this->reset_counting();
        this->reset_short_breaks();
        this->add_long_break();
        ESP_LOGI(TAG, "long break, amount taken: %" PRIu32 "", this->get_long_breaks());
    };
    void react(ResetTimer const &) override { transit<Idle>(); };

    void react(CheckTimer const &) override
    {
        if (!this->is_timer_active())
        {
            return;
        }

        int64_t elapsed_time_in_seconds = this->get_counting_seconds();
        if (elapsed_time_in_seconds < Pomodoro::LONG_BREAK_PERIOD_SECONDS - Pomodoro::SHORT_BREAK_PERIOD_SECONDS)
        {
            ESP_LOGI(TAG, "long break time left: %" PRIu64 " sec", Pomodoro::LONG_BREAK_PERIOD_SECONDS - elapsed_time_in_seconds);
            return;
        }

        transit<LongBreakLastMinutes>();
    };

    void react(TimerAction const &) override
    {
        if (!this->is_started())
        {
            this->start_counting();
            return;
        }

        transit<Work>();
    };
};
// The state where the timer is counting down last minutes of the long break period.
struct LongBreakLastMinutes : Pomodoro
{
    void entry() override
    {
        ESP_LOGI(TAG, "long break last minutes");
    };
    void react(ResetTimer const &) override { transit<Idle>(); };

    void react(CheckTimer const &) override
    {
        if (!this->is_timer_active())
        {
            return;
        }

        int64_t elapsed_time_in_seconds = this->get_counting_seconds();
        if (elapsed_time_in_seconds < Pomodoro::LONG_BREAK_PERIOD_SECONDS)
        {
            ESP_LOGI(TAG, "long break time left, it's last minutes: %" PRIu64 "", Pomodoro::LONG_BREAK_PERIOD_SECONDS - elapsed_time_in_seconds);
            return;
        }

        transit<Work>();
    };

    void react(TimerAction const &) override
    {
        if (!this->is_started())
        {
            this->start_counting();
            return;
        }

        transit<Work>();
    };
};

FSM_INITIAL_STATE(Pomodoro, Off)
using fsm_handle = Pomodoro;

static esp_timer_handle_t periodic_timer;
static QueueHandle_t gpio_evt_queue = nullptr;

static void periodic_timer_callback(void *arg);
static void IRAM_ATTR gpio_isr_handler(void *arg);
static void gpio_handle_evt_from_isr(void *arg);
static void led_visualize(int64_t time_since_boot);

static esp_err_t start_timer()
{
    fsm_handle::dispatch(timer_ready_event);
    fsm_handle::dispatch(start_timer_event);

    esp_timer_create_args_t periodic_timer_args = {};

    periodic_timer_args.callback = &periodic_timer_callback;
    periodic_timer_args.name = "dispatch_timer";

    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));

    /* Start the timers */
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 500000));

    return ESP_OK;
}

static void periodic_timer_callback(void *arg)
{
    int64_t time_since_boot = esp_timer_get_time();
    ESP_LOGI(TAG, "periodic timer called, time since boot: %" PRIu64 " sec", time_since_boot / 1000000);

    fsm_handle::dispatch(check_timer_event);

    led_visualize(time_since_boot);
}

static esp_err_t gpio_setup()
{
    gpio_config_t io_conf;

    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1 << GPIO_ACTION_BUTTON);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1 << GPIO_LIGHT_RED) | (1 << GPIO_LIGHT_YELLOW) | (1 << GPIO_LIGHT_GREEN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // change gpio intrrupt type for one pin
    gpio_set_intr_type(GPIO_ACTION_BUTTON, GPIO_INTR_ANYEDGE);

    // create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    // start gpio task
    xTaskCreate(gpio_handle_evt_from_isr, "gpio_handle_evt_from_isr", 2048, nullptr, 10, nullptr);

    // install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    // hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_ACTION_BUTTON, gpio_isr_handler, (void *)(GPIO_ACTION_BUTTON));

    return ESP_OK;
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, nullptr);
}

static void gpio_handle_evt_from_isr(void *arg)
{
    uint32_t gpio_num;
    static int64_t last_isr_time = 0;
    const int64_t debounce_time = 200000; // 200 ms debounce time

    for (;;)
    {
        if (xQueueReceive(gpio_evt_queue, &gpio_num, portMAX_DELAY))
        {
            int64_t current_time = esp_timer_get_time();
            if (current_time - last_isr_time < debounce_time)
            {
                continue; // Ignore the event if it's within the debounce time
            }
            last_isr_time = current_time;

            ESP_LOGI(TAG, "GPIO[%" PRIu32 "] evt received", gpio_num);
            switch (gpio_num)
            {
            case GPIO_ACTION_BUTTON:
                fsm_handle::dispatch(timer_action_event);
                break;
            default:
                break;
            }
        }
    }
}

static void led_visualize(int64_t time_since_boot)
{
    enum led_state
    {
        OFF,
        IDLE,
        WORK,
        SHORT_BREAK,
#ifdef LONG_BREAK_ENABLE
        LONG_BREAK,
        LONG_BREAK_LAST_MINUTES,
#endif
    };
    led_state state = OFF;
    state = fsm_handle::is_in_state<Idle>() ? IDLE : state;
    state = fsm_handle::is_in_state<Work>() ? WORK : state;
    state = fsm_handle::is_in_state<ShortBreak>() ? SHORT_BREAK : state;

#ifdef LONG_BREAK_ENABLE
    state = fsm_handle::is_in_state<LongBreak>() ? LONG_BREAK : state;
    state = fsm_handle::is_in_state<LongBreakLastMinutes>() ? LONG_BREAK_LAST_MINUTES : state;
#endif

    bool is_paused = Pomodoro::current_state_ptr->is_paused();
    bool is_started = Pomodoro::current_state_ptr->is_started();

    int64_t seconds_since_boot = time_since_boot / 1000000;
    bool is_even = seconds_since_boot % 2 == 0;
    uint32_t led_on = 0;
    uint32_t led_off = 1;
    uint32_t led_blink = is_even ? led_on : led_off;

    switch (state)
    {
    case OFF:
        gpio_set_level(GPIO_LIGHT_RED, led_on);
        gpio_set_level(GPIO_LIGHT_YELLOW, led_on);
        gpio_set_level(GPIO_LIGHT_GREEN, led_on);
        return;
    case IDLE:
        gpio_set_level(GPIO_LIGHT_RED, led_off);
        gpio_set_level(GPIO_LIGHT_YELLOW, led_blink);
        gpio_set_level(GPIO_LIGHT_GREEN, led_off);
        return;
    case WORK:
        if (!is_started)
        {
            gpio_set_level(GPIO_LIGHT_RED, led_on);
            gpio_set_level(GPIO_LIGHT_YELLOW, led_on);
            gpio_set_level(GPIO_LIGHT_GREEN, led_off);
            return;
        }
        if (is_paused)
        {
            gpio_set_level(GPIO_LIGHT_RED, led_off);
            gpio_set_level(GPIO_LIGHT_YELLOW, led_blink);
            gpio_set_level(GPIO_LIGHT_GREEN, led_on);
            return;
        }
        gpio_set_level(GPIO_LIGHT_RED, led_off);
        gpio_set_level(GPIO_LIGHT_YELLOW, led_off);
        gpio_set_level(GPIO_LIGHT_GREEN, led_on);
        return;

    case SHORT_BREAK:
#ifdef LONG_BREAK_ENABLE
        if (!is_started)
        {
            gpio_set_level(GPIO_LIGHT_RED, led_off);
            gpio_set_level(GPIO_LIGHT_YELLOW, led_on);
            gpio_set_level(GPIO_LIGHT_GREEN, led_on);
            return;
        }
        gpio_set_level(GPIO_LIGHT_RED, led_blink);
        gpio_set_level(GPIO_LIGHT_YELLOW, led_off);
        gpio_set_level(GPIO_LIGHT_GREEN, led_off);
        return;
    case LONG_BREAK:
#endif
        if (!is_started)
        {
            gpio_set_level(GPIO_LIGHT_RED, led_off);
            gpio_set_level(GPIO_LIGHT_YELLOW, led_on);
            gpio_set_level(GPIO_LIGHT_GREEN, led_on);
            return;
        }
        gpio_set_level(GPIO_LIGHT_RED, led_on);
        gpio_set_level(GPIO_LIGHT_YELLOW, led_off);
        gpio_set_level(GPIO_LIGHT_GREEN, led_off);
        return;
#ifdef LONG_BREAK_ENABLE
    case LONG_BREAK_LAST_MINUTES:
        if (!is_started)
        {
            gpio_set_level(GPIO_LIGHT_RED, led_blink);
            gpio_set_level(GPIO_LIGHT_YELLOW, led_off);
            gpio_set_level(GPIO_LIGHT_GREEN, led_off);
            return;
        }
        gpio_set_level(GPIO_LIGHT_RED, led_blink);
        gpio_set_level(GPIO_LIGHT_YELLOW, led_off);
        gpio_set_level(GPIO_LIGHT_GREEN, led_off);
        return;
#endif
    }
}

void app_main(void)
{
    fsm_handle::start();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(wifi_connect());
    ESP_ERROR_CHECK(start_timer());
    ESP_ERROR_CHECK(gpio_setup());

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
