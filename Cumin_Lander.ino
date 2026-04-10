/*********************************************************************
 Project: Cumin Lander
 Refactored to include:
 - Init startup routine. Shows 00h00m00s until time is sent via bluetooth serial
 - Added different LED flashing modes depending on initialization state
 - Fixed counting jumps
 - Added a visual battery voltage check on startup
 - Added voltage monitoring every second & low voltage warning if <3.5V (Lower end for 14250 unprotected battery)
 
 Credit: Adapted from Mohit Bhoite's original Cumin Lander project
 *********************************************************************/

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TimeLib.h>
#include <Adafruit_BME280.h>
#include "DSEG7_Classic_Mini_Regular_15.h"

#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

// BLE Service Declarations
BLEDfu bledfu;    // OTA DFU service
BLEDis bledis;    // device information
BLEUart bleuart;  // uart over ble
BLEBas blebas;    // battery

// Pin Definitions
#define RED_LED   D2
#define GREEN_LED D3 

// OLED Definitions
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define SEALEVELPRESSURE_HPA (1013.25)

// Hardware Objects
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
GFXcanvas1 canvas(128, 32);
Adafruit_BME280 bme; 

// Time Variables
char timeHour[8];
char timeMinute[8];
char timeSecond[8];

uint8_t myHour;
uint8_t myMinute;
uint8_t mySecond;
time_t lastTimeDrawn = 0; 

// Manual time keeper (The source of truth for the clock)
time_t elapsedSeconds = 0; 
time_t getTeensyTime(); 

// Sensor Data Storage
float currentTempC = 0.0;
float currentHumi = 0.0;
float currentPressure_hPa = 1013.25; 
float currentAltitude_m = 0.0;
float currentVbat = 0.0;

// Non-Blocking Timing Variables
unsigned long lastClockTickTime = 0;
const long clockTickInterval = 1000; 

unsigned long lastSensorReadTime = 0;
const long sensorReadInterval = 1000; 

// CRITICAL: Clock Control Flag
bool isClockRunning = false;
bool isBleActive = false; 

// --- BLUE LED FLASHING CONTROL (FINAL TIMING) ---
bool isBlueLEDFlashing = false;
unsigned long lastBlueLEDChangeTime = 0;
// Phase 3: RESTORED TIMING: ON for 500ms, OFF for 500ms (Total 1000ms cycle = once every second)
const int blueLEDOnTime = 500; 
const int blueLEDOffTime = 500; 
bool blueLEDState = false; // Tracks if the LED should be ON (LOW) or OFF (HIGH)

// --- BLUE LED CONFIRMATION FLASH (PHASE 2) ---
bool isQuickFlashing = false;
unsigned long lastQuickFlashTime = 0;
const int quickFlashOnTime = 50;  // Quick flash ON time
const int quickFlashOffTime = 50; // Quick flash OFF time
const int quickFlashCount = 5;    // Number of quick flashes
uint8_t currentQuickFlash = 0;

// --- RED/GREEN LED FLASHING CONTROL (Existing) ---
unsigned long lastLEDEventTime = 0;
const long ledFlashInterval = 1500; 
unsigned long nextLEDChangeTime = 0; 

// LED Flashing State Tracker
enum {
  STATE_FLASH_GREEN,
  STATE_FLASH_RED
} flashColorState = STATE_FLASH_GREEN;

// LED Flashing State Variables
bool isFlashing = false; 
uint8_t flashCount = 0;  
const uint8_t maxFlashes = 5;
const int flashOnTime = 10;
const int flashOffTime = 50;

uint8_t myTime[6];

// --- FORWARD DECLARATIONS ---
void initBLE(void);
void startAdv(void);
void connect_callback(uint16_t conn_handle);
void disconnect_callback(uint16_t conn_handle, uint8_t reason);
void handleBLETimeUpdate(void);
void readAndStoreBMEData(void);
void updateAndDrawDisplay(void);
void handleLEDFlashing(void);
void handleBlueLEDFlashing(void); 
void enableBLE(void); 
void disableBLE(void); 


void setup()
{
  Serial.begin(99600); 
  delay(100);
  
  // Pin setup
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(LED_BLUE, OUTPUT); 

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(LED_BLUE, HIGH); // Start with Blue LED OFF (HIGH = inactive)

  // Time initialization - Set to EPOCH (00:00:00) 
  hourFormat12();
  setTime(0, 0, 0, 1, 1, 1970); 
  elapsedSeconds = now(); 
  setSyncProvider(getTeensyTime); 

  // BME280 Initialization
  unsigned status = bme.begin(0x76);
  if (!status) {
    Serial.println("Could not find a valid BME280 sensor, check wiring!");
  }

  // OLED Initialization
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }
  display.clearDisplay();
  display.display();

  // CRITICAL: Immediately enable BLE to listen for time sync
  initBLE(); 
  enableBLE(); // This turns the blue LED to SOLID ON (Phase 1)
  
  // Initial draw 
  readAndStoreBMEData();
  lastTimeDrawn = now() - 1; 
  updateAndDrawDisplay();
}

time_t getTeensyTime()
{
  return elapsedSeconds;
}


void loop()
{
  // 1. ABSOLUTE HIGHEST PRIORITY: Dedicated Clock Tick 
  if (isClockRunning) {
      while (millis() - lastClockTickTime >= clockTickInterval) {
          elapsedSeconds++; 
          lastClockTickTime += clockTickInterval; 
      }
  }

  // 2. CRITICAL: Handle BLE UART (ONLY runs if BLE is still active)
  if (isBleActive) {
      handleBLETimeUpdate(); 
  }
  
  // 3. NON-BLOCKING: LED Flashing State Machines
  handleLEDFlashing(); 
  handleBlueLEDFlashing(); // Handles all Blue LED status phases
  
  // 4. NON-BLOCKING: Sensor Read (checks every 1000ms)
  if (millis() - lastSensorReadTime >= sensorReadInterval) {
    readAndStoreBMEData(); 
    lastSensorReadTime = millis();
  }

  // 5. NON-BLOCKING: Display Update 
  if (!isClockRunning || (now() != lastTimeDrawn)) {
     updateAndDrawDisplay();
  }
}

// --- BLUE LED STATUS MANAGER (Handles all 3 phases) ---
void handleBlueLEDFlashing(void) {
    unsigned long currentTime = millis();
    
    // --- Phase 2: Quick Confirmation Flash (Highest Priority when active) ---
    if (isQuickFlashing) {
        if (currentTime - lastQuickFlashTime >= (blueLEDState == LOW ? quickFlashOnTime : quickFlashOffTime)) {
            lastQuickFlashTime = currentTime;
            
            if (blueLEDState == LOW) { // Currently ON, time to turn OFF
                digitalWrite(LED_BLUE, HIGH);
                blueLEDState = HIGH;
            } else { // Currently OFF, time to turn ON
                digitalWrite(LED_BLUE, LOW);
                blueLEDState = LOW;
                currentQuickFlash++;
            }
            
            // End the Quick Flash sequence
            if (currentQuickFlash > quickFlashCount) {
                isQuickFlashing = false;
                
                // Keep the LED solid ON after the quick flash, waiting for time input (Phase 1 continuation)
                digitalWrite(LED_BLUE, LOW); 
            }
        }
        return; // Don't run Phase 3 logic while quick flashing
    }

    // --- Phase 3: Stable Mode Status Flash ---
    if (!isBleActive && isBlueLEDFlashing) { // Only run if BLE is shut down and flashing is enabled
        // Check if it's time to change the state (using the 500ms / 500ms cycle)
        if (currentTime - lastBlueLEDChangeTime >= (blueLEDState == LOW ? blueLEDOnTime : blueLEDOffTime)) {
            lastBlueLEDChangeTime = currentTime;
            
            if (blueLEDState == LOW) { // Currently ON, time to turn OFF
                digitalWrite(LED_BLUE, HIGH);
                blueLEDState = HIGH;
            } else { // Currently OFF, time to turn ON
                digitalWrite(LED_BLUE, LOW);
                blueLEDState = LOW;
            }
        }
    }
}

// --- PHASE 1 START ---
void enableBLE(void) {
    isBleActive = true;
    isBlueLEDFlashing = false; 
    isQuickFlashing = false; 
    digitalWrite(LED_BLUE, LOW); // Indicate BLE is active (Blue LED SOLID ON)
    startAdv(); 
    Serial.println("BLE Enabled: Waiting for Time Synchronization.");
}

// --- PHASE 3 START (Transition from Phase 1/2) ---
void disableBLE(void) {
    Bluefruit.disconnect(0xFFFF); 
    Bluefruit.Advertising.stop();
    isBleActive = false;
    
    // START THE LONG BLUE LED STATUS FLASHING CYCLE
    isBlueLEDFlashing = true;
    lastBlueLEDChangeTime = millis();
    blueLEDState = LOW; // Start the cycle with the LED ON
    digitalWrite(LED_BLUE, LOW); 
    
    Serial.println("BLE Shutdown: Clock Stability Mode Activated.");
}


void handleBLETimeUpdate(void) {
  static uint8_t serialindex = 0; 
  
  while (bleuart.available())
  {
    char ch = (char) bleuart.read();
    Serial.write(ch);  

    if (ch >= '0' && ch <= '9') {
      myTime[serialindex] = ch - '0';
      serialindex++;

      if (serialindex == 6) {
        myHour   = myTime[0] * 10 + myTime[1];
        myMinute = myTime[2] * 10 + myTime[3];
        mySecond = myTime[4] * 10 + myTime[5];

        tmElements_t tm;
        tm.Hour   = myHour;
        tm.Minute = myMinute;
        tm.Second = mySecond;
        tm.Day    = 1;
        tm.Month  = 1;
        tm.Year   = CalendarYrToTm(2024); 
        
        time_t newTime = makeTime(tm);
        
        setTime(myHour, myMinute, mySecond, 1, 1, 2024); 
        
        elapsedSeconds = newTime;
        lastClockTickTime = millis(); 

        isClockRunning = true;
        disableBLE(); // Transition to Phase 3
        
        Serial.print("Time Set and Clock UNLOCKED. BLE radio is OFF.");

        serialindex = 0; 
      }
    }
    else if (ch == '\r' || ch == '\n') {
      serialindex = 0;
    }
  } 
} 

void readAndStoreBMEData(void) {
  float newTemp = bme.readTemperature();
  float newHumi = bme.readHumidity();
  float newPressure = bme.readPressure() / 100.0F;
  float newAltitude = bme.readAltitude(SEALEVELPRESSURE_HPA);

  if (!isnan(newTemp)) {
      currentTempC = newTemp;
      currentHumi = newHumi;
      currentPressure_hPa = newPressure;
      currentAltitude_m = newAltitude;
  }

  // --- NEW BATTERY LEVEL LOGIC ---
  pinMode(VBAT_ENABLE, OUTPUT);
  digitalWrite(VBAT_ENABLE, LOW); // Turn on the divider
  delay(1); // Stabilization delay
  
  // Hardware multiplier for XIAO Sense (1M/510k divider)
  currentVbat = (float)analogRead(PIN_VBAT) * (3.6 / 1024.0) * 2.96;
  
  digitalWrite(VBAT_ENABLE, HIGH); // Turn off the divider
  // --- END BATTERY LEVEL LOGIC ---
}

void handleLEDFlashing(void) {
  unsigned long currentTime = millis();

  if (!isFlashing && (currentTime - lastLEDEventTime >= ledFlashInterval)) {
    isFlashing = true;
    flashCount = 0; 
    flashColorState = STATE_FLASH_GREEN; 
    nextLEDChangeTime = currentTime; 
    digitalWrite(GREEN_LED, LOW); 
    digitalWrite(RED_LED, LOW);
  }

  if (isFlashing) {
    if (currentTime >= nextLEDChangeTime) {
      if (flashCount < maxFlashes * 2) {
        if (flashCount % 2 == 0) { 
          if (flashColorState == STATE_FLASH_GREEN) {
            digitalWrite(GREEN_LED, HIGH);
          } else { 
            digitalWrite(RED_LED, HIGH);
          }
          nextLEDChangeTime = currentTime + flashOnTime; 
        } 
        else { 
          if (flashColorState == STATE_FLASH_GREEN) {
            digitalWrite(GREEN_LED, LOW);
          } else { 
            digitalWrite(RED_LED, LOW);
          }
          nextLEDChangeTime = currentTime + flashOffTime; 
          
          if (flashCount == (maxFlashes * 2) - 1) { 
            if (flashColorState == STATE_FLASH_GREEN) {
              flashColorState = STATE_FLASH_RED;
              flashCount = 0; 
            }
            else {
              isFlashing = false;
              lastLEDEventTime = currentTime; 
              flashCount = 0;
              return; 
            }
          }
        }
        flashCount++;
      } else {
        isFlashing = false;
        lastLEDEventTime = currentTime; 
        flashCount = 0;
      }
    }
  }
}

void updateAndDrawDisplay(void) {
  time_t t = now(); 
  
  if (!isClockRunning) {
      sprintf(timeHour, "00");
      sprintf(timeMinute, "00");
      sprintf(timeSecond, "00"); 
  } else {
      sprintf(timeHour, "%02d", hour(t));
      sprintf(timeMinute, "%02d", minute(t));
      sprintf(timeSecond, "%02d", second(t)); 
  }

  canvas.fillScreen(0);
  canvas.setTextColor(SSD1306_WHITE, SSD1306_BLACK); 
  canvas.setRotation(1); 

  canvas.drawLine(0, 10, 32, 10, SSD1306_WHITE);
  canvas.drawLine(0, 84, 32, 84, SSD1306_WHITE);

  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setCursor(0, 0);
  //Dynamic header - shows voltage on startup for 5 seconds, then reverts to UI. Shows LOW if voltage is lower than 3.5V.
  if (millis() < 5000) {
      canvas.print(currentVbat, 2); canvas.print("V"); 
  } else if (currentVbat < 3.5) {
      if ((millis() / 1000) % 2 == 0) { canvas.print("  LOW"); } 
      else { canvas.print(currentVbat, 2); canvas.print("V"); }
  } else {
      canvas.print("/////"); 
  }
  
  canvas.setCursor(0, 15);
  canvas.print("  24H");

  canvas.setFont(&DSEG7_Classic_Mini_Regular_15);
  canvas.setCursor(5, 40);
  canvas.print(timeHour);
  canvas.setCursor(5, 60);
  canvas.print(timeMinute);
  canvas.setCursor(5, 80);
  canvas.print(timeSecond);

  canvas.setFont();
  canvas.setTextSize(1);

  if (!isnan(currentTempC)) {
    canvas.setCursor(0, 90);
    canvas.print("  "); canvas.print((int)currentTempC); canvas.print("C");
  }

  if (!isnan(currentHumi)) {
    canvas.setCursor(0, 105);
    canvas.print("  "); canvas.print((int)currentHumi); canvas.print("%");
    canvas.setCursor(0, 120);
    canvas.print(""); canvas.print((int)currentPressure_hPa); canvas.print("hPa");
  }

  display.drawBitmap(0, 0, canvas.getBuffer(), 128, 32, 1, 0);
  display.display();
  
  lastTimeDrawn = t; 
}


// --- CRITICAL CHANGE HERE (LINE 575) ---
void initBLE(void)
{
  Bluefruit.autoConnLed(false); // <--- Prevents Bluefruit library from overriding manual control
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);

  bledfu.begin();
  bledis.setManufacturer("Adafruit Industries");
  bledis.setModel("Bluefruit Feather52");
  bledis.begin();
  bleuart.begin();
  blebas.begin();
  blebas.write(100);
}

void startAdv(void)
{
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  // CRITICAL: Stop Advertising when connected (allows us to control the connection LED)
  Bluefruit.Advertising.restartOnDisconnect(false); 
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
}

// --- PHASE 2 START (Connection Established) ---
void connect_callback(uint16_t conn_handle)
{
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char central_name[32] = { 0 };
  connection->getPeerName(central_name, sizeof(central_name));
  Serial.print("Connected to ");
  Serial.println(central_name);

  // Stop the SOLID ON (Phase 1) and start the quick flash (Phase 2)
  Bluefruit.Advertising.stop(); // Stop advertising while connected
  isBlueLEDFlashing = false;    // Stop any long status flashing
  isQuickFlashing = true;       // Start the quick confirmation flash
  lastQuickFlashTime = millis();
  currentQuickFlash = 0;
  blueLEDState = LOW;           // Ensure it starts ON
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
  (void) conn_handle;
  (void) reason;
  Serial.println();
  Serial.print("Disconnected, reason = 0x"); Serial.println(reason, HEX);
  
  // If disconnected before time was set, revert to SOLID ON (Phase 1) and restart advertising
  if (!isClockRunning) {
      enableBLE();
  }
}
