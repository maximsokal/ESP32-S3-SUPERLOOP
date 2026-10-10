#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_PAUSE_GPIO GPIO_NUM_4
#define LED2_GPIO      GPIO_NUM_5
#define LED3_GPIO      GPIO_NUM_6
#define BUTTON1_GPIO   GPIO_NUM_8
#define BUTTON2_GPIO   GPIO_NUM_10

#define BUTTON_POLL_MS      10U
#define BUTTON_DEBOUNCE_MS  40U
#define HEARTBEAT_MS       2000U
#define LED3_PERIOD_MS     1000U

// One notification contains the complete latest control state.
#define CONTROL_SPEED_MASK 0x03UL
#define CONTROL_PAUSED_BIT (1UL << 8)

static const char *TAG = "module_6_1";
static const uint32_t LED2_SPEEDS_MS[] = {1000U, 500U, 200U};
#define SPEED_COUNT (sizeof(LED2_SPEEDS_MS) / sizeof(LED2_SPEEDS_MS[0]))

typedef struct {
    gpio_num_t pin;
    bool last_raw;
    bool stable;
    TickType_t last_transition;
} button_t;

typedef struct {
    gpio_num_t pin;
    bool variable_speed;
    uint32_t initial_period_ms;
} led_config_t;

static const led_config_t led2_config = {LED2_GPIO, true, 1000U};
static const led_config_t led3_config = {LED3_GPIO, false, LED3_PERIOD_MS};

static TaskHandle_t led2_handle;
static TaskHandle_t led3_handle;
static TaskHandle_t heartbeat_handle;
static TaskHandle_t button_handle;

static bool button_pressed_edge(button_t *button, TickType_t now)
{
    const bool raw = gpio_get_level(button->pin) == 1;

    if (raw != button->last_raw) {
        button->last_raw = raw;
        button->last_transition = now;
    }

    if (raw != button->stable &&
        (TickType_t)(now - button->last_transition) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
        button->stable = raw;
        return raw; // Debounced rising edge only.
    }
    return false;
}

static uint32_t encode_control(bool paused, uint32_t speed_index)
{
    return (paused ? CONTROL_PAUSED_BIT : 0U) | (speed_index & CONTROL_SPEED_MASK);
}

static void notify_control(TaskHandle_t task, uint32_t state)
{
    configASSERT(task != NULL);
    const BaseType_t result = xTaskNotify(task, state, eSetValueWithOverwrite);
    configASSERT(result == pdPASS);
}

static void button_task(void *arg)
{
    (void)arg;
    button_t speed_button = {.pin = BUTTON1_GPIO};
    button_t pause_button = {.pin = BUTTON2_GPIO};
    uint32_t speed_index = 0U;
    bool paused = false;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(BUTTON_POLL_MS));
        const TickType_t now = xTaskGetTickCount();
        const bool speed_changed = button_pressed_edge(&speed_button, now);
        const bool pause_changed = button_pressed_edge(&pause_button, now);

        if (speed_changed) {
            speed_index = (speed_index + 1U) % SPEED_COUNT;
            ESP_LOGI(TAG, "LED2 period: %lu ms", (unsigned long)LED2_SPEEDS_MS[speed_index]);
        }
        if (pause_changed) {
            paused = !paused;
            ESP_ERROR_CHECK(gpio_set_level(LED_PAUSE_GPIO, paused ? 1U : 0U));
            ESP_LOGI(TAG, "Paused: %s", paused ? "true" : "false");
        }
        if (speed_changed || pause_changed) {
            const uint32_t state = encode_control(paused, speed_index);
            notify_control(led2_handle, state);
            // Speed changes must not reset LED3 or heartbeat timing.
            if (pause_changed) {
                notify_control(led3_handle, state);
                notify_control(heartbeat_handle, state);
            }
        }
    }
}

static void led_task(void *arg)
{
    const led_config_t *cfg = (const led_config_t *)arg;
    uint32_t period_ms = cfg->initial_period_ms;
    bool paused = false;
    bool level = false;

    for (;;) {
        uint32_t state = 0U;
        const TickType_t wait = paused ? portMAX_DELAY : pdMS_TO_TICKS(period_ms);
        if (xTaskNotifyWait(0U, UINT32_MAX, &state, wait) == pdTRUE) {
            paused = (state & CONTROL_PAUSED_BIT) != 0U;
            if (cfg->variable_speed) {
                const uint32_t index = state & CONTROL_SPEED_MASK;
                configASSERT(index < SPEED_COUNT);
                period_ms = LED2_SPEEDS_MS[index];
            }
            // Re-arm a full period after speed change or pause/resume.
            continue;
        }
        if (!paused) {
            level = !level;
            ESP_ERROR_CHECK(gpio_set_level(cfg->pin, level ? 1U : 0U));
        }
    }
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    bool paused = false;
    uint32_t heartbeats = 0U;
    for (;;) {
        uint32_t state = 0U;
        const TickType_t wait = paused ? portMAX_DELAY : pdMS_TO_TICKS(HEARTBEAT_MS);
        if (xTaskNotifyWait(0U, UINT32_MAX, &state, wait) == pdTRUE) {
            paused = (state & CONTROL_PAUSED_BIT) != 0U;
            continue; // Pause/resume re-arms heartbeat.
        }
        if (!paused) {
            const int64_t uptime_ms = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "alive @ %lld ms", (long long)uptime_ms);
            if (++heartbeats % 10U == 0U) {
                ESP_LOGI(TAG, "stack HWM bytes: buttons=%u led2=%u led3=%u heartbeat=%u",
                         (unsigned)uxTaskGetStackHighWaterMark(button_handle),
                         (unsigned)uxTaskGetStackHighWaterMark(led2_handle),
                         (unsigned)uxTaskGetStackHighWaterMark(led3_handle),
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
    }
}

static void create_task(TaskFunction_t function, const char *name,
                        uint32_t stack_bytes, UBaseType_t priority,
                        void *arg, TaskHandle_t *handle)
{
    if (xTaskCreate(function, name, stack_bytes, arg, priority, handle) != pdPASS) {
        ESP_LOGE(TAG, "Could not create task %s", name);
        abort();
    }
}

void app_main(void)
{
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << LED_PAUSE_GPIO) | (1ULL << LED2_GPIO) | (1ULL << LED3_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << BUTTON1_GPIO) | (1ULL << BUTTON2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));
    ESP_ERROR_CHECK(gpio_config(&inputs));
    ESP_ERROR_CHECK(gpio_set_level(LED_PAUSE_GPIO, 0));
    ESP_ERROR_CHECK(gpio_set_level(LED2_GPIO, 0));
    ESP_ERROR_CHECK(gpio_set_level(LED3_GPIO, 0));

    // Recipients must exist before the button task can notify them.
    create_task(led_task, "led2", 2048U, 1U, (void *)&led2_config, &led2_handle);
    create_task(led_task, "led3", 2048U, 1U, (void *)&led3_config, &led3_handle);
    create_task(heartbeat_task, "heartbeat", 3072U, 1U, NULL, &heartbeat_handle);
    create_task(button_task, "buttons", 3072U, 2U, NULL, &button_handle);

    ESP_LOGI(TAG, "6.1 RTOS started | BTN1: LED2 speed | BTN2: pause/resume");
    ESP_LOGI(TAG, "LED2: 1000/500/200 ms | LED3: 1000 ms | heartbeat: 2000 ms");
}
