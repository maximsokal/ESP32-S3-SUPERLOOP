# Модуль 6.1 — RTOS Introduction (ESP32-S3 / ESP-IDF 5.5)

Домашнє завдання до уроку **6.1 «Чому не можна писати все в superloop»**. Основа — домашня робота **2.3** з цього репозиторію. Повністю **нативний ESP-IDF 5.5, мова C, FreeRTOS**, без Arduino та PlatformIO.

## Перехід від superloop до FreeRTOS

Раніше всі дії відбувалися послідовно в `loop()` через `millis()` і масив `Task tasks[]`. Тепер `app_main()` налаштовує GPIO та створює **чотири** задачі `xTaskCreate()` без прив'язки до ядра:

| Задача | Пріоритет | Що робить |
|---|---:|---|
| `buttons` | 2 | Опитування кнопок кожні 10 мс через `vTaskDelayUntil()`, debounce 40 мс |
| `led2` | 1 | GPIO5, періоди 1000/500/200 мс, перемикання кнопкою BTN1 |
| `led3` | 1 | GPIO6, незалежне блимання кожні 1000 мс |
| `heartbeat` | 1 | Лог кожні 2000 мс та контроль мінімального залишку стеку |

**BTN2** вмикає або вимикає паузу, яку показує LED1 (GPIO4). Під час паузи LED2/LED3 зберігають останні рівні, а heartbeat зупиняється. При відновленні запускається новий повний інтервал. BTN1 змінює швидкість LED2, зокрема під час паузи; це не збиває період LED3 чи heartbeat.

У вихідному коді 2.3 LED2 стартував з `tasks[0].period = 200`, хоча `currentSpeedIndex = 0` відповідав 1000 мс. Тут цю неузгодженість виправлено: початково **1000 мс**, потім BTN1 циклічно вибирає **500 → 200 → 1000 мс**.

## Підключення ESP32-S3

| Елемент | GPIO | Схема |
|---|---:|---|
| LED1 — пауза | 4 | GPIO → резистор → LED → GND |
| LED2 — швидкість | 5 | GPIO → резистор → LED → GND |
| LED3 — незалежний | 6 | GPIO → резистор → LED → GND |
| BTN1 — швидкість | 8 | Кнопка між GPIO та **3.3 V** |
| BTN2 — пауза | 10 | Кнопка між GPIO та **3.3 V** |

Використовується внутрішнє `GPIO_PULLDOWN_ENABLE`; натиснута кнопка читається як HIGH. Кнопки до 5 V підключати не можна. Для LED потрібен струмообмежувальний резистор, наприклад 330–1000 Ω.

## Збірка та запуск

Відкрити **ESP-IDF 5.5.x PowerShell** або інший термінал із налаштованим ESP-IDF:

```powershell
git clone -b hw/6.1-rtos-esp-idf https://github.com/maximsokal/ESP32-S3-SUPERLOOP.git
cd ESP32-S3-SUPERLOOP
idf.py set-target esp32s3
idf.py build
idf.py -p COM4 flash monitor
```

Замінити COM4 на реальний порт плати. Вихід із монітора — **Ctrl+]**. `sdkconfig.defaults` задає FreeRTOS tick 1000 Гц та перевірку переповнення стеку з canary. Створений `sdkconfig` не комітиться.

## Демонстрація для LMS

1. Показати `main/main.c`: `app_main()`, чотири виклики `xTaskCreate()` через обгортку `create_task()`, відсутність Arduino `setup()/loop()`.
2. Прошити ESP32-S3, увімкнути Serial Monitor 115200. Показати `6.1 RTOS started` і періодичні `alive @ ... ms`.
3. Показати LED2 та LED3, натиснути BTN1 кілька разів: LED2 змінює період, LED3 та heartbeat продовжують роботу.
4. Натиснути BTN2: LED1 увімкнений, інші два LED зберігають стан, heartbeat припиняється. Натиснути BTN2 знову: робота відновлюється.
5. Показати захист від дребезгу (одне стабільне натискання — одна дія), натискання BTN1 під час паузи, а після 10 heartbeat — `stack HWM bytes`.

**Приклад очікуваних повідомлень (не фактичні вимірювання):**

```text
I (...) module_6_1: 6.1 RTOS started | BTN1: LED2 speed | BTN2: pause/resume
I (...) module_6_1: alive @ ... ms
I (...) module_6_1: LED2 period: 500 ms
I (...) module_6_1: Paused: true
I (...) module_6_1: Paused: false
I (...) module_6_1: alive @ ... ms
```

Фізичну демонстрацію та відео потрібно отримати з реальної плати; текст вище не підміняє апаратного тесту.

## Технічні пояснення

- `vTaskDelayUntil()` тримає фіксований період опитування двох кнопок, а `xTaskNotifyWait()` блокує тільки свою задачу до події чи тайм-ауту — без busy-wait.
- Стан швидкості та паузи змінює лише `buttons`; іншим задачам передається **повне актуальне значення стану** через `xTaskNotify(..., eSetValueWithOverwrite)`. Mutex тут не потрібен.
- Подія зміни швидкості надходить тільки `led2`, тож не відтерміновує heartbeat. Команда паузи доставляється всім робочим задачам і будить їх без очікування кінця періоду.
- `uxTaskGetStackHighWaterMark()` дозволяє бачити **найменший вільний залишок** стека задач. Canary перевіряє переповнення на перемиканні контексту. Стек у ESP-IDF задають у **байтах**.
- Реальний час тут **soft real-time**: невеликі затримки планувальника можливі; точність необхідно перевіряти на залізі.

## CI

`.github/workflows/esp-idf-build.yml` запускає компіляцію для ESP32-S3 на ESP-IDF серії 5.5 після push у цю гілку. Статус фактичного виконання дивитися у вкладці **Actions**.

Джерела:
- [ESP-IDF 5.5 FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/freertos_idf.html)
- [ESP-IDF 5.5 GPIO](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/peripherals/gpio.html)
- [ESP-IDF 5.5 Build System](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/build-system.html)
