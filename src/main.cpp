#include <Arduino.h>

#define LED1_PIN 4
#define LED2_PIN 5
#define LED3_PIN 6
#define BTN1_PIN 8
#define BTN2_PIN 10

const uint32_t INTERVAL = 1000; // 1 second
uint32_t lastToggle = 0;
bool ledOn = false;


const uint32_t SPEEDS[3] = {1000, 500, 200}; // Speeds for the three LEDs
uint8_t currentSpeedIndex = 0;
uint32_t last2 = 0, last3 = 0;
bool s2 = false, s3 = false;

const uint32_t DEBOUNCE = 40; // Debounce time in milliseconds
bool paused = false;

struct Button
{

  uint8_t pin;
  bool lastState = false, lastRead = false;
  uint32_t lastChange = 0;
  bool pressedEdge()
  {
    bool currentState = digitalRead(pin) == HIGH, edgeDetected = false;
    if (currentState != lastRead)
    {
      lastChange = millis();
      lastRead = currentState;
    }

    if (millis() - lastChange > DEBOUNCE && currentState != lastState)
    {
      lastState = currentState;
      if (currentState)
      {
        edgeDetected = true; // Button pressed
      }
    }
    return edgeDetected;
  }
};

Button b1 = {BTN1_PIN}, b2 = {BTN2_PIN};

// Бонус : найпростіший неблокуючий планувальник задач 
struct Task {
  uint32_t period; // як часто запускати, мс
  uint32_t last;   // коли запускали востаннє
  byte pin;
  int (*run)(byte pin);   // що виконувати
};

int blinkFast(byte pin)
{
  static bool s;
  s = !s;
  digitalWrite(pin, s);
  return 0;
}

int blinkSlow(byte pin)
{
  static bool s;
  s = !s;
  digitalWrite(pin, s);
  return 0;
}

int heartbeat(byte pin) { Serial.printf("alive @ %lu ms\n", millis());  return 0;}

Task tasks[] = {
  {200,  0, LED2_PIN, blinkFast},
  {1000, 0, LED3_PIN, blinkSlow},
  {2000, 0, 0,        heartbeat},
};

void updateLED()
{
  uint32_t currentMillis = millis();
  if (currentMillis - lastToggle >= INTERVAL)
  {
    lastToggle = currentMillis;
    ledOn = !ledOn;
    digitalWrite(LED1_PIN, ledOn);
  }
}

void checkButton()
{
  bool pressed = digitalRead(BTN1_PIN) == HIGH;
  digitalWrite(LED1_PIN, pressed);
}

void setup()
{
  Serial.begin(115200);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(LED3_PIN, OUTPUT);
  pinMode(BTN1_PIN, INPUT_PULLDOWN);
  pinMode(BTN2_PIN, INPUT_PULLDOWN);
  Serial.printf("%d ", sizeof(SPEEDS));
  Serial.printf("%d: ", sizeof(SPEEDS) / sizeof(SPEEDS[0]));
  Serial.println("Step 5: BTN1=velocity LED2, BTN2=pause/resume");
}

void loop()
{
  // --- TASK A: button of velocity ---
  if (b1.pressedEdge())
  {
    currentSpeedIndex = (currentSpeedIndex + 1) % (sizeof(SPEEDS) / sizeof(SPEEDS[0]));
    tasks[0].period = SPEEDS[currentSpeedIndex];
    Serial.printf("Speed changed to %lu ms\n", SPEEDS[currentSpeedIndex]);
  }

  // --- TASK B: button of pause/resume ---
  if (b2.pressedEdge())
  {
    paused = !paused;
    digitalWrite(LED1_PIN, paused); // indicator of pause/resume state
    Serial.printf("Paused: %s\n", paused ? "true" : "false");
  }

  if (paused)
  {
    return; // Skip updating LEDs if paused
  }
  
  uint32_t currentMillis = millis();

  for (Task &task : tasks)
  {
    if (currentMillis - task.last >= task.period)
    {
      task.last = currentMillis;
      task.run(task.pin);
    }
  }
}