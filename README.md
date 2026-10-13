# Mini_DC_Motor_Driver_w_ToF_Detection

## Overview

This project implements a mini DC motor driver controlled by an ESP32-C3-WROOM, with a Time-of-Flight (ToF) sensor providing a safety shut-off. When an object enters the configured exclusion distance, the motor is disabled and remains disabled until safe restart conditions are met.

The project demonstrates the use of vision sensing for motor control, with firmware structured around FreeRTOS tasks.

## Features

- **Split power domains:** USB VBUS supplies the digital side (ESP32 and ToF sensor), and a separate 5 V source supplies the analog (motor) side.
- **Variable speed control:** a potentiometer provides the driver signal that sets the motor power level.
- **8x8 ToF sensing at 15 Hz:** provided by a Light Ranger 12 Click breakout board (ST VL53L8CH).
- **Proximity shut-off:** the motor is disabled if any zone reports an object within **250 mm**.
- **Safe restart:** following a shut-off, the motor remains disabled until both of the following conditions are met:
  1. No object is detected within 250 mm.
  2. The driver signal has been returned to zero.
- **Safe startup:** on power-up, the motor is not enabled until the driver signal reads zero.
- **Sensor timeout:** the motor is disabled if no frame is received from the ToF sensor within 198 ms (three sensor periods).
- **Desktop visualizer (Qt):** displays a live topographical heat map of the ToF sensor data and reports the current system status.

## Hardware

| Component | Description |
| --- | --- |
| ESP32-C3-WROOM | Main controller |
| Light Ranger 12 Click (VL53L8CH) | ToF sensor; 8x8 zones, 15 Hz, I²C interface |
| Mini DC motor and motor driver | Driven by 30 kHz, 10-bit PWM |
| Potentiometer | Driver signal input, sampled by the ADC |
| USB (VBUS) | Digital-side supply |
| External 5 V supply | Analog-side (motor) supply |

### Pin Assignments (ESP32-C3)

| GPIO | Function |
| --- | --- |
| 0 | ToF sensor interrupt (INT), falling edge |
| 1 | I²C SCL |
| 2 | I²C SDA |
| 3 | `bootPin`, driven high at startup |
| 4 | Driver signal input (ADC, continuous mode) |
| 6 | Motor driver PWM (PWMA) |

The complete schematic is provided in [Distance_Sensor.kicad_sch](Distance_Sensor.kicad_sch) (KiCad).

## Firmware

The firmware source is located at [Code/Distance_Sensor/Distance_Sensor/Distance_Sensor.ino](Code/Distance_Sensor/Distance_Sensor/Distance_Sensor.ino) and targets the Arduino core for ESP32. The `src/` directory contains the ST VL53LMZ Ultra Lite Driver and the VL53L8CH wrapper.

The application is divided into four FreeRTOS tasks that communicate through task notifications and atomic variables. The Arduino `loop()` function is unused.

| Task | Priority | Responsibility |
| --- | --- | --- |
| `sensorReadTask` | 2 | Triggered by the sensor INT pin. Reads each frame, transmits it over serial, evaluates the closest measured distance, and reports a timeout if frames stop arriving. |
| `adcReadTask` | 2 | Triggered on completion of each continuous ADC batch (25 conversions at 1 kHz, 40 updates/s). Converts the driver signal into a PWM target and enforces the zero-signal restart requirement. |
| `motorTask` | 2 | Applies the PWM target. When starting from rest, applies a 50 ms kick at a minimum of 71% duty to ensure reliable spin-up. |
| `driverEventTask` | 3 (highest) | Manages the safety state. On a fault (proximity violation or sensor timeout), stops ADC sampling, sets the motor output to zero, and invalidates the motor state. Resumes ADC sampling once the fault clears. |

**Duty cycle mapping:** driver signal readings below 1% are treated as off. Readings above this threshold are mapped to approximately 58–100% duty, as the motor stalls below roughly 58%.

### Serial Protocol

Data is transmitted at **460800 baud**. Each frame consists of a status line followed by eight rows of eight distance values (mm).

| Status | Meaning |
| --- | --- |
| `0` | Clear; motor enabled |
| `1` | Sensor timeout |
| `2` | Object detected within 250 mm |
| `4` | Clear; awaiting a zero driver signal before enabling the motor |

Within each row, a value prefixed with `E` indicates a lower-confidence measurement (target status other than 5 or 9), and `X` indicates that no target was detected in that zone.

## Qt Visualizer

The Qt project is located in [QT/Distance_Map](QT/Distance_Map). A deployed executable is available at:

```
QT/Distance_Map/deploy/Distance_Map/appDistance_Map.exe
```

The application parses the serial stream described above, renders an 8x8 topographical heat map, and displays the current system status.

## Future Improvements

- **Single power input:** the digital side currently operates from USB VBUS, and the motor requires a separate 5 V supply. Operating both from a single source would require protection between the two domains, such as reverse-current protection between USB VBUS and the external supply, along with filtering and decoupling to prevent motor noise and inrush current from causing brownouts on the ESP32.
- **Improved analog/digital isolation:** further separate the motor supply and ground from the logic domain.
- **Memory and stack optimization:** review stack and RAM usage to reduce code size and size each task stack appropriately.
- **Motor enclosure:** design a housing for the motor.
- **Faster sensor with hardware thresholds:** adopt a sensor that supports threshold-based interrupts and a higher sample rate, so that a proximity trip does not depend on reading and parsing a full frame. The VL53L8CH provides a detection-threshold plugin, but when enabled, the sensor only asserts an interrupt for frames in which an object is within the threshold. This prevents streaming every frame to the Qt visualizer while also using the interrupt for proximity detection. The firmware therefore services an interrupt on every frame and evaluates the threshold in software.
- **Custom PCB:** develop a production-ready PCB from the existing schematic.
- **Hardware interlock:** add a shut-off path independent of the firmware, such as gating the motor driver enable or standby pin from the sensor or a watchdog. This ensures the motor stops even if the ESP32 hangs or resets.
- **Motor current sensing:** detect stall or jam conditions, including obstructions outside the ToF sensor's field of view.


## AI Use Disclosure

- **Qt project:** created entirely by Claude as a visual reference.
- **Firmware:** written by Robyn Pollock, using Claude and traditional methods as reference tools.
- **KiCad schematic:** created without the use of AI.
