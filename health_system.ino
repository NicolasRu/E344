#include <Arduino.h>
#include <WS2812FX.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiAP.h>
#include <AsyncWebServer_ESP32_SC_W5500.h>

#define LED_COUNT 1
#define LED_PIN 8  // connected as such on ESP32 C3 board
// custom pin connections:
#define OLED_SDA_PIN 5
#define OLED_SCL_PIN 6
#define ONE_WIRE_BUS_PIN 9
#define STEP_PIN_X 2
#define STEP_PIN_Y 3
#define STEP_PIN_Z 4
#define HEARTBEAT_PIN 18
#define ANALOG_TEMP_PIN 0  // only use pin 0-4
#define WEIGHT_PIN 1  // only use pin 0-4

// ----------------------------------------------------------------------------
// CHANGE MAPPINGS BELOW
#define ANALOG_TEMP_MIN_IN 620
#define ANALOG_TEMP_MAX_IN 3723
#define ANALOG_TEMP_MIN_OUT 30
#define ANALOG_TEMP_MAX_OUT 40

#define WEIGHT_MIN_IN 0
#define WEIGHT_MAX_IN 4095
#define WEIGHT_MIN_OUT 0
#define WEIGHT_MAX_OUT 200

#define BPM_AVG 3  // number of beats to use to calculate BPM
#define INVALID_BEAT_GAP 3500  // time (ms) between two beats before it is invalid

float STEP_THRESHOLD 1.38 //threshold voltage corresponding to 1.46G
// CHANGE BELOW WIFI ACCESS POINT DETAILS
// The ESP32 creates its own wifi network that other devices can connect to
const char *ssid = "24771767";
const char *password = "12345678";
// ----------------------------------------------------------------------------

// initialise Wi-fi server
AsyncWebServer server(80);

// initialise WS2812FX instance (onboard RGB LED)
WS2812FX ws2812fx = WS2812FX(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);

// initialise u8g2 instance (OLED screen)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// initialise oneWire instance (temp sensor)
OneWire oneWire(ONE_WIRE_BUS_PIN);

// pass oneWire reference to DallasTemperature library
DallasTemperature sensors(&oneWire);

String newestStrings[5] = {"", "", "", "", ""}; // used for OLED
String newestString = ",,,,";  // used to send to website and serial
volatile unsigned long steps = 0;
volatile unsigned long lastStepTime = 0;
volatile unsigned long beatTimes[BPM_AVG] = {};

float mapfloat(float x, float in_min, float in_max, float out_min, float out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

void updateSingleString() {
  newestString = "";
  for (int i = 0; i < 5; i++) {
    newestString += newestStrings[i] + ",";
  }
  newestString.remove(newestString.length() - 1, 1);
}

// This function is called every loop.
// The values it updates are then used to update the website asynchronously, update the OLED and sent via serial to PC.
void updateValues() {
  // --> Digital temperature
  sensors.requestTemperatures();
  String temp_value = String(sensors.getTempCByIndex(0));
  newestStrings[0] = temp_value;

  // --> Analogue RTD temperature
  newestStrings[1] = mapfloat(analogRead(ANALOG_TEMP_PIN), ANALOG_TEMP_MIN_IN, ANALOG_TEMP_MAX_IN, ANALOG_TEMP_MIN_OUT, ANALOG_TEMP_MAX_OUT);
  
  // --> Weight
  newestStrings[2] = mapfloat(analogRead(WEIGHT_PIN), WEIGHT_MIN_IN, WEIGHT_MAX_IN, WEIGHT_MIN_OUT, WEIGHT_MAX_OUT);

  // --> Pedometer
  newestStrings[3] = steps;

  // --> Heartrate
  bool valid = true;
  for (int i = 0; i < (BPM_AVG - 1); i++) {
    if (beatTimes[i + 1] - beatTimes[i] > INVALID_BEAT_GAP) {
      valid = false;
    }
  }
  if (millis() - beatTimes[BPM_AVG - 1] > INVALID_BEAT_GAP || !valid) {  // if no beat in last 3.5 seconds...
    newestStrings[4] = "--";
  } else {
    newestStrings[4] = String(1.0 / ((float)(beatTimes[BPM_AVG - 1] - beatTimes[0]) / (float)(BPM_AVG - 1)) * 1000.0 * 60.0);
  }

  updateSingleString();
}

void IRAM_ATTR newStep() {
  unsigned long currentTime = millis();
  if (currentTime - lastStepTime > 50) {
    steps += 1;
  }
  lastStepTime = currentTime;
}

void IRAM_ATTR newBeat() {
  unsigned long currentTime = millis();
  if (currentTime - beatTimes[BPM_AVG - 1] > 200) {
    for (int i = 0; i < (BPM_AVG - 1); i++) {
      beatTimes[i] = beatTimes[i + 1];
    }
    beatTimes[BPM_AVG - 1] = currentTime;
  }
}

void setup(void) {
  // USB serial setup
  Serial.begin(115200);

  // wi-fi setup
  if (!WiFi.softAP(ssid, password)) {
    Serial.println("Could not create wi-fi network. Check SSID and password for validity.");
  }
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  // server setup
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    String htmlContent = R"=====(
      <!DOCTYPE html>
      <html>
      <head>
        <title>Health Monitoring System</title>
        <style>
          body {
            font-family: 'Arial', sans-serif;
            background-color: #f9f9f9;
            margin: 0;
            padding: 0;
          }
      
          h1 {
            text-align: center;
            margin: 30px 0;
            color: #4CAF50;
            font-size: 70px;
          }
      
          .values-container {
            display: flex;
            flex-wrap: wrap;
            justify-content: center;
            margin: 20px;
          }
      
          .value-box {
            width: 350px;
            height: 300px;
            padding: 25px;
            background-color: #fff;
            border-radius: 8px;
            box-shadow: 0 10px 10px rgba(0, 0, 0, 0.1);
            margin: 10px;
            text-align: center;
          }
      
          .value-title {
            font-size: 40px;
            font-weight: bold;
            color: #333;
          }
      
          .value {
            font-size: 60px;
            color: #4CAF50;
          }
      
          /* Media Query for Smartphones */
          @media (min-width: 1200px) {
            h1 {
              font-size: 40px;
            }
      
            .value-box {
              width: 150px;
              height: 100px;
              padding: 20px;
            }
      
            .value-title {
              font-size: 16px;
            }
      
            .value {
              font-size: 20px;
            }
          }
        </style>
      </head>
      <body>
        <h1>Health Monitoring System</h1>
        <div class="values-container">
          <div class="value-box">
            <div class="value-title">Digital temperature (1-wire)</div>
            <div class="value" id="value1"></div>
          </div>
          <div class="value-box">
            <div class="value-title">RTD analogue temperature</div>
            <div class="value" id="value2"></div>
          </div>
          <div class="value-box">
            <div class="value-title">Weight</div>
            <div class="value" id="value3"></div>
          </div>
          <div class="value-box">
            <div class="value-title">Pedometer count</div>
            <div class="value" id="value4"></div>
          </div>
          <div class="value-box">
            <div class="value-title">Heartrate</div>
            <div class="value" id="value5"></div>
          </div>
        </div>
      
        <script>
          function updateValues() {
            var xhr = new XMLHttpRequest();
            xhr.onreadystatechange = function() {
              if (xhr.readyState === 4) {
                if (xhr.status === 200) {
                  var response = xhr.responseText;
                  var values = response.split(',');
                  document.getElementById('value1').textContent = values[0].trim() + " \u2103";
                  document.getElementById('value2').textContent = values[1].trim() + " \u2103";
                  document.getElementById('value3').textContent = values[2].trim() + " kg";
                  document.getElementById('value4').textContent = values[3].trim() + " steps";
                  document.getElementById('value5').textContent = values[4].trim() + " BPM";
                } else {
                  document.getElementById('value1').textContent = "No data!"
                  document.getElementById('value2').textContent = "No data!"
                  document.getElementById('value3').textContent = "No data!"
                  document.getElementById('value4').textContent = "No data!"
                  document.getElementById('value5').textContent = "No data!"
                }
              }
            };
            xhr.onerror = function() {
              document.getElementById('value1').textContent = "No data!"
              document.getElementById('value2').textContent = "No data!"
              document.getElementById('value3').textContent = "No data!"
              document.getElementById('value4').textContent = "No data!"
              document.getElementById('value5').textContent = "No data!"
            };
            xhr.open('GET', '/values', true);
            xhr.send();
          }
      
          setInterval(updateValues, 500);
        </script>
      </body>
      </html>
    )=====";
    request->send(200, "text/html", htmlContent);
  });
  server.on("/values", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", newestString);
  });
  server.begin();
  Serial.println("Server started successfully.");

  // onboard LED setup
  ws2812fx.init();
  ws2812fx.setBrightness(255);
  ws2812fx.setSpeed(200);
  ws2812fx.setMode(FX_MODE_RAINBOW_CYCLE);
  ws2812fx.start();

  // OLED screen setup
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  u8g2.begin();
  u8g2.setFont(u8g2_font_helvB08_tf);

  // Digital temperature sensor setup
  sensors.begin();
  pinMode(ONE_WIRE_BUS_PIN, INPUT_PULLUP);

  // interrupts setup
  pinMode(STEP_PIN, INPUT_PULLUP);
  pinMode(HEARTBEAT_PIN, INPUT_PULLUP);
  attachInterrupt(STEP_PIN, newStep, RISING);
  attachInterrupt(HEARTBEAT_PIN, newBeat, RISING);
}

void loop(void) {
  updateValues();
  Serial.println(newestString);

  // update onboard LED
  ws2812fx.service();

  // update OLED screen
  u8g2.clearBuffer();
  u8g2.setCursor(0, 12);
  u8g2.print("Digital temp: ");
  u8g2.print(newestStrings[0]);
  u8g2.print(" ");
  u8g2.print((char)176);
  u8g2.print("C");
  u8g2.setCursor(0, 24);
  u8g2.print("Analog temp: ");
  u8g2.print(newestStrings[1]);
  u8g2.print(" ");
  u8g2.print((char)176);
  u8g2.print("C");
  u8g2.setCursor(0, 36);
  u8g2.print("Weight: ");
  u8g2.print(newestStrings[2]);
  u8g2.print(" kg");
  u8g2.setCursor(0, 48);
  u8g2.print("Pedometer: ");
  u8g2.print(newestStrings[3]);
  u8g2.print(" steps");
  u8g2.setCursor(0, 60);
  u8g2.print("Heartrate: ");
  u8g2.print(newestStrings[4]);
  u8g2.print(" BPM");
  u8g2.sendBuffer();
}
