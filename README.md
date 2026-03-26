# Cumin Lander

## Overview
Based on Mohit Bhoite’s circuit sculpture, this project is a custom desk clock and environmental monitor. It synchronizes its internal clock via a temporary BLE connection on startup, allowing for accurate timekeeping without a dedicated RTC module.

## Hardware Components
* **Seeed Studio XIAO nRF52840:** A compact Bluetooth LE-enabled microcontroller that manages all non-blocking logic and system synchronization.
* **BME280 Sensor:** Polled every 1000ms to capture live temperature, humidity, and atmospheric pressure.
* **SSD1306 OLED (128x32):** Displays a 24-hour clock alongside real-time environmental metrics.
* **3.7V 14250 Li-ion Battery:** A rechargeable power source integrated via the XIAO’s onboard battery management pins.

## Operation & Initialization

**Time Sync Process:**
On boot, the display initializes to a default `00:00:00` state while the XIAO begins advertising via BLE. A connected device sends the current time as a 6-digit string (`HHMMSS`) via Bluetooth Serial. Once parsed, the internal clock "unlocks," the display updates from the zeroed state, and the BLE radio shuts down to conserve battery.

**Blue Status LED Modes:**
* **Phase 1 (Solid ON):** Waiting for a BLE connection and time sync while the clock remains at `00:00:00`.
* **Phase 2 (Quick Flash):** Connection established; rapidly flashing while waiting for the serial time payload.
* **Phase 3 (1Hz Pulse):** Time is successfully set and BLE is disabled for stable, long-term operation.
