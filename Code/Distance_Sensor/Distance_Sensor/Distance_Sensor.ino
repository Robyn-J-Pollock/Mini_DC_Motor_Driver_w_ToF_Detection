#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <Ticker.h>
#include "src/vl53l8ch.h"

#define DISABLE_DRIVER_PRIORITY 3
#define DRIVER_PRIORITY 2
#define MOTOR_PRIORITY 2
#define SENSOR_READ_PRIORITY 2

//1000 mm over top expected distance
#define VL53LMZ_MAX_DISTANCE_MM 3000

#define SENSOR_RESOLUTION VL53LMZ_RESOLUTION_8X8
#define SENSOR_ZONES_PER_LINE 8
#define SENSOR_FREQUENCY 15
#define SENSOR_TIMEOUT_MS 198 //3*Sensor Period

//611 minimum samples/second
//25ms -> 40Hz -> 25 Conversions -> 1000Hz
#define ADC_CONVERSIONS 25
#define ADC_SAMPLE_HZ 1000

//Startup behavior for setup
#define BOOT_BLINK_MS 500

//Above human hearing to prevent whine
#define PWM_FREQ_HZ 30000
#define PWM_RES_BITS 10

#define KICK_MS 50
#define KICK_DUTY 726 //30% into motorDCToRegValue -> 71% actual -> 726

#define MINIMUM_DISTANCE_MM 250

const uint8_t bootPin = 3;

const uint8_t sdaPin = 2;
const uint8_t sclPin = 1;
const uint8_t intPin = 0;

const uint8_t driver[1] = {4};
const uint8_t pwma = 6;

TaskHandle_t sensorReadTaskHandle = NULL;
TaskHandle_t driverTaskHandle = NULL;
TaskHandle_t motorTaskHandle = NULL;
TaskHandle_t driverEventTaskHandle = NULL;

adc_continuous_result_t *adcResult = NULL;

enum Events : uint8_t {
  kClearEvent, kTimeoutEvent = 1, kUnderThresholdEvent = 2, kClearButMotorDisabled = 4
};

std::atomic<uint8_t> eventState{kUnderThresholdEvent};
std::atomic<bool> motorStateValid{false};

void parseSensorReport(VL53LMZ_ResultsData *Result);
inline uint32_t motorDCToRegValue(float dc);

VL53L8CH sensor(&Wire, -1);

Ticker bootBlink;
volatile bool bootLedOn = false;

void toggleBootLed() {
  bootLedOn = !bootLedOn;
  digitalWrite(bootPin, bootLedOn);
}

void IRAM_ATTR onSensorInt() {
  BaseType_t higherPriorityWoken = pdFALSE;
  vTaskNotifyGiveFromISR(sensorReadTaskHandle, &higherPriorityWoken);
  portYIELD_FROM_ISR(higherPriorityWoken);
}

void sensorReadTask(void *param) {
  //Static to keep it in global (Could be a few KB)
  static VL53LMZ_ResultsData Results;
  bool timedOut = false;
  TickType_t lastFrame = xTaskGetTickCount();

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SENSOR_TIMEOUT_MS));

    uint8_t ready = 0;
    uint8_t status = sensor.check_data_ready(&ready);

    //If new data is ready, read/print wait for poll sleep period
    if (status == 0 && ready) {
      timedOut = false;
      lastFrame = xTaskGetTickCount();
      status = sensor.get_ranging_data(&Results);
      parseSensorReport(&Results);
      continue;
    }
    else if (!timedOut && (xTaskGetTickCount() - lastFrame >= pdMS_TO_TICKS(SENSOR_TIMEOUT_MS))) {
        timedOut = true;
        xTaskNotify(driverEventTaskHandle, kTimeoutEvent, eSetValueWithOverwrite);
    }
  }
}

void ARDUINO_ISR_ATTR onADCDone() {
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(driverTaskHandle, &woken);
  portYIELD_FROM_ISR(woken);
}

void adcReadTask(void *param) {
  float lastReading = 0;
  uint32_t newTarget = 0;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (analogContinuousRead(&adcResult, 0)) {
      //2.4812V Max, 3079 Raw Max (expected) -> 3390 actual (Noise can increase)
      //Measured .0075 noise. Assuming (+/- 0.01)
      float newReading = adcResult[0].avg_read_raw / (3390.0);
      if (newReading > 1) newReading = 1;

      //Prevent motor from running at start/restarting from forced stop
      if (!motorStateValid) {
        if (newReading > 0.01) continue;

        motorStateValid = true;
      }

      //Reading changed
      if (abs(lastReading - newReading) > 0.01 || (newTarget != 0 && newReading < 0.005)) {
        lastReading = newReading;
        newTarget = motorDCToRegValue(lastReading);

        xTaskNotify(motorTaskHandle, newTarget, eSetValueWithOverwrite);
      }
    }
  }
}

inline uint32_t motorDCToRegValue(float dc) {
  if (dc > 1) return 1023;

  if (dc < 0.01) { //Turn off
    dc = 0;
  } else {
    //58% min so fan doesn't turn off
    //Adjust dc to 100 -> 58 range
    dc = 0.42*dc + 0.585;
  }

  return 1023*dc + 0.5;
}

void motorKick(uint32_t *target) {
  if (eventState) return;
  ledcWrite(pwma, (*target > KICK_DUTY ? *target : KICK_DUTY));

  TickType_t kickEnd = xTaskGetTickCount() + pdMS_TO_TICKS(KICK_MS);

  while (*target != 0) {
    int32_t remaining = (int32_t)(kickEnd - xTaskGetTickCount());
    if (remaining <= 0) break;
    xTaskNotifyWait(0, 0, target, remaining);
  }
}

void motorTask(void *param) {
  uint32_t current = 0;

  for (;;) {
    uint32_t target;
    xTaskNotifyWait(0, 0, &target, portMAX_DELAY);

    if (current == 0 && target > 0) {
      motorKick(&target);
    }

    if (eventState) target = 0;
    ledcWrite(pwma, target);
    current = target;
  }
}

void driverEventTask(void *param) {
  for (;;) {
    uint32_t newEvent = kClearEvent;
    xTaskNotifyWait(0, 0, &newEvent, portMAX_DELAY);

    if (newEvent && !eventState) {
      motorStateValid = false;
      eventState |= newEvent;

      analogContinuousStop();  
      xTaskNotify(motorTaskHandle, 0, eSetValueWithOverwrite);
    }
    else if (!newEvent && eventState) {
      eventState = newEvent;
      analogContinuousStart();
    }
    else if (newEvent && eventState) {
      eventState |= newEvent;
    }
  }
}

void setup() {

  pinMode(bootPin, OUTPUT);
  digitalWrite(bootPin, LOW);

  //Runs from the esp_timer task, so it keeps blinking while setup blocks
  bootBlink.attach_ms(BOOT_BLINK_MS, toggleBootLed);

#if ARDUINO_USB_CDC_ON_BOOT
  //Native USB blocks on print when no host is reading; drop output instead
  Serial.setTxTimeoutMs(0);
#endif

  Serial.begin(460800);

  Wire.begin(sdaPin, sclPin);

  sensor.begin();

  sensor.init();
  sensor.set_resolution(SENSOR_RESOLUTION);
  sensor.set_ranging_frequency_hz(SENSOR_FREQUENCY); //Max for 8x8

  ledcAttach(pwma, PWM_FREQ_HZ, PWM_RES_BITS);

  xTaskCreate(adcReadTask, "Driver Reading", 4096, NULL, DRIVER_PRIORITY, &driverTaskHandle);
  xTaskCreate(motorTask, "Motor", 2048, NULL, MOTOR_PRIORITY, &motorTaskHandle);
  xTaskCreate(driverEventTask, "Disable Driver", 2048, NULL, DISABLE_DRIVER_PRIORITY, &driverEventTaskHandle);

  analogContinuous(driver, sizeof(driver), ADC_CONVERSIONS, ADC_SAMPLE_HZ, &onADCDone);

  ledcWrite(pwma, 0);

  //Start ranging before the read task exists so they don't share the I2C bus mid-start
  sensor.start_ranging();

  xTaskCreate(sensorReadTask, "Read Sensor", 8192, NULL, SENSOR_READ_PRIORITY, &sensorReadTaskHandle);

  pinMode(intPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(intPin), onSensorInt, FALLING);

  bootBlink.detach();
  digitalWrite(bootPin, HIGH);
}

void parseSensorReport(VL53LMZ_ResultsData *Result) {
  long closestReading = VL53LMZ_MAX_DISTANCE_MM;

  uint8_t events = eventState;
  if (eventState) {
    Serial.println(eventState);
  }
  else {
    Serial.println(motorStateValid ? 0 : kClearButMotorDisabled);
  }

  for (uint8_t rowOffset = 0; rowOffset < SENSOR_RESOLUTION; rowOffset += SENSOR_ZONES_PER_LINE) {

    // Print distance
    // Signed so the countdown can reach -1 and exit
    for (int8_t columnIndex = (SENSOR_ZONES_PER_LINE - 1); columnIndex >= 0; columnIndex--) {
      uint16_t zoneIndex = VL53LMZ_NB_TARGET_PER_ZONE * (rowOffset + columnIndex);
      uint8_t targetStatus = Result->target_status[zoneIndex];

      // 255 means no target detected, safe to ignore for target checks
      if (targetStatus != 255) {
        //5 and 9 are reliable signals, E before unreliable signals for possible interpolation
        if (targetStatus != 5 && targetStatus != 9) {
          Serial.print(F("E"));
        }

        long newReading = (long)Result->distance_mm[zoneIndex];
        
        if (newReading < closestReading) {
          closestReading = newReading;
        }

        Serial.print(newReading);
      }
      else {
        Serial.print(F("X"));
      }
      Serial.print(F(" "));
    }

    Serial.println();
  }

  if (closestReading < MINIMUM_DISTANCE_MM) {
    xTaskNotify(driverEventTaskHandle, kUnderThresholdEvent, eSetValueWithOverwrite);
  }
  else {
    xTaskNotify(driverEventTaskHandle, kClearEvent, eSetValueWithOverwrite);
  }
}

void loop() {

}