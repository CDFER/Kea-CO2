/**
 * @file main.cpp
 * @brief Kea Studios CO2 Sensor Firmware
 * This firmware is designed to work with an ESP32 Wroom module and a Sensirion SCD40 or SCD41
 * sensor to measure ambient CO2 levels and display them on a strip of WS2812B addressable RGB
 * LEDs (Light Bar). The brightness of the LEDs is adjusted based on the ambient light level.
 * The firmware also includes a web server which displays graphs of CO2, humidity, and temperature,
 * and provides a CSV download for that data.
 * @author Chris Dirks (@CDFER)
 * @created 14/02/2023
 * @url https://www.keastudios.co.nz
 * @license HIPPOCRATIC LICENSE Version 3.0
 */

 //    __ __           ______          ___
 //   / //_/__ ___ _  / __/ /___ _____/ (_)__  ___
 //  / ,< / -_) _ `/ _\ \/ __/ // / _  / / _ \(_-<
 // /_/|_|\__/\_,_/ /___/\__/\_,_/\_,_/_/\___/___/
 //                  __      _        _  __             ____           __             __
 //   __ _  ___ ____/ /__   (_)__    / |/ /__ _    __  /_  / ___ ___ _/ /__ ____  ___/ /
 //  /  ' \/ _ `/ _  / -_) / / _ \  /    / -_) |/|/ /   / /_/ -_) _ `/ / _ `/ _ \/ _  /
 // /_/_/_/\_,_/\_,_/\__/ /_/_//_/ /_/|_/\__/|__,__/   /___/\__/\_,_/_/\_,_/_//_/\_,_/

#include <Arduino.h>

// -----------------------------------------
//
//    External Libraries
// 		for Wifi, Webserver and DNS
//
// -----------------------------------------
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "AsyncJson.h"
#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"

// Onboard Flash Storage
#include <LittleFS.h>

// NTP and Timezone headers
#include "sntp.h"
#include "time.h"

#ifdef OTA
#include <ArduinoOTA.h>
#include <ESPTelnet.h>
const char* ssid = "ssid";
const char* netpassword = "password";
ESPTelnet telnet;
#endif

// -----------------------------------------
//
//    Hardware Config
//
// -----------------------------------------
#include "NeoPixelBus.h"  // Light Bar
#include "ltr303.h"		  // LTR303 (Light sensor)
#include "pcf8563.h"	  // pcf8563 (Backup Clock)
#include "scd4x.h"		  // SCD4x (CO2 sensor)

// Define the pins for the I2C communication buses
// #define WIRE_SDA_PIN 21
// #define WIRE_SCL_PIN 22
// #define WIRE1_SDA_PIN 33
// #define WIRE1_SCL_PIN 32

// Define the data pin for the WS2812B LED light bar, the number of pixels,
// and an offset to adjust the temperature reading
// #define PIXEL_DATA_PIN 16  // GPIO -> LEVEL SHIFT -> Pixel 1 Data In Pin
#define PIXEL_COUNT 11	   // Number of Addressable Pixels to write data to (starts at pixel 1)
#define TEMP_OFFSET 10.6	   // The enclosure runs a bit hot, so reduce this to get a more accurate ambient temperature

// -----------------------------------------
//
//    LightBar Config
//
// -----------------------------------------

// Define the maximum and minimum CO2 values for the light bar,
// the frame time (in milliseconds) between light bar updates,
// the brightness factor (lux/BRIGHTNESS_FACTOR = LED brightness),
// and the maximum brightness setting for the LEDs
#define CO2_MAX 2000		 // Top of the CO2 light bar (when it transitions to warning flash)
#define CO2_MIN 400			 // Bottom of the light bar (baseline CO2 level)
#define FRAME_TIME 30		 // Milliseconds between frames (30ms = ~33.3fps maximum)
#define BRIGHTNESS_FACTOR 6	 // Lux/BRIGHTNESS_FACTOR = LED brightness
#define MAX_BRIGHTNESS 200	 // Maximum brightness setting for the WS2812B LEDs
enum lightBarModes {
	idleFrame,
	lightBarScale,
	flashRed,
	purplePulse,
	greenPulse,
	rgbTest,
	errorRed,
	off
};

// -----------------------------------------
//
//    Webserver Settings
//
// -----------------------------------------

// Define the time zone, SSID, and password for the WiFi network,
// as well as the record intervals for the CSV and JSON files
const char* time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";	// Time zone (see https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)
const char* password = "";								// Password of the WiFi access point (leave blank for no password)
#define CSV_RECORD_INTERVAL_SECONDS 60					// Record interval (in seconds) for the CSV file
#define JSON_RECORD_INTERVAL_SECONDS 1					// Record interval (in seconds) for the JSON file

// Define the filename, maximum size, and location for the CSV log file,
// as well as the IP and URL for the web server
char CSVLogFilename[] = "/Kea-CO2-Data.csv";			// Location of the CSV file
#define MAX_CSV_SIZE_BYTES 2000000						// Maximum size of the CSV file (2 MB)
#define CSV_LINE_MAX_CHARS 64							// Maximum size of the csvLine character buffer
const IPAddress localIP(4, 3, 2, 1);					// IP address of the web server (Samsung requires the IP to be in public space)
const IPAddress gatewayIP(4, 3, 2, 1);					// IP address of the network (should be the same as the local IP in most cases)
const String localIPURL = "http://4.3.2.1/index.html";	// URL to the web server (same as the local IP, but as a string with http and the landing page subdomain)

// -----------------------------------------
//
//    Helpful Defines (Don't Touch)
//
// -----------------------------------------

#define LIGHTBAR_MAX_POSITION PIXEL_COUNT * 255
#define LIGHTBAR_MIN_POSITION 255
#define LAST_PIXEL PIXEL_COUNT - 1				 // last Addressable Pixel to write data to (starts at pixel 0)
#define low8Bits(w) ((uint8_t)((w)))			 // returns bits 1 - 8 of a 32bit variable
#define high16Bits(w) ((uint16_t)((w) >> 16))	 // returns bits 17 - 32 of a 32bit variable

// -----------------------------------------
//
//    Global Variables
//
// -----------------------------------------

// Allocate the JSON document size in RAM https://arduinojson.org/v6/assistant to compute the size.
DynamicJsonDocument jsonDataDocument(23520);  // The size of the JSON document in bytes (about 23KB).
// This is used to store the data that will be sent to the web server.

#define JSON_DATA_POINTS_MAX 128  // The maximum number of data points to store in the JSON document.

TaskHandle_t lightBar = NULL;		  // A handle to the task that controls the light bar.
TaskHandle_t csvFileManager = NULL;	  // A handle to the task that writes sensor data to a CSV file.
TaskHandle_t sensorManager = NULL;	  // A handle to the task that reads sensor data.
TaskHandle_t webserver = NULL;		  // A handle to the task that runs the web server.
TaskHandle_t jsonFileManager = NULL;  // A handle to the task that writes JSON data to a file.

QueueHandle_t charsForCSVFileQueue;	 // A queue of character arrays which contain human readable sensor data.
// This is used to communicate between the sensor manager and the CSV file manager tasks.

QueueHandle_t jsonDataQueue;  // A queue of data points in doubles.
// This is used to communicate between the sensor manager and the JSON file manager tasks.

SemaphoreHandle_t jsonDocMutex;	 // A semaphore used to ensure that only one task accesses the JSON document at a time.
// This is used to prevent race conditions where two tasks try to access the document at the same time.

/**
 * @brief Rounds a double to a float with x decimal places
 * @param decimalPlaces The number of decimal places to round to
 * - 0: Returns the integer part of the value
 * - 1: Returns the value rounded to 1 decimal place
 * - 2: Returns the value rounded to 2 decimal places
 * - and so on...
 * @param value The input double to be rounded
 * @return A float rounded to the specified number of decimal places
 */
float roundToXDP(uint8_t decimalPlaces, double value) {	 // rounds a number to x decimal place
	return (float)(((int)(value * (10 * decimalPlaces) + 0.5)) / (10 * decimalPlaces));
}

/**
 * @brief Initializes the jsonDataDocument with default state, which includes graph colors but no data points.
 * This function clears the jsonDataDocument and creates three nested objects representing CO2, humidity, and temperature graphs.
 * Each object includes a "name", "color", and "y_title" field. Additionally, each object has a nested array called "data"
 * which initially has no elements.
 * @note This function assumes that the jsonDataDocument has already been initialized elsewhere in the program.
 * @see jsonDataDocument
 */
void initializeJson() {
	xSemaphoreTake(jsonDocMutex, portMAX_DELAY);  // ask for control of json doc

	jsonDataDocument.clear();

	JsonObject co2_graph_object = jsonDataDocument.createNestedObject();
	co2_graph_object["name"] = "CO2";
	co2_graph_object["color"] = "#70AE6E";
	co2_graph_object["y_title"] = "CO2 Parts Per Million (PPM)";
	JsonArray co2_graph_data_array = co2_graph_object["data"].createNestedArray();

	JsonObject humidity_graph_object = jsonDataDocument.createNestedObject();
	humidity_graph_object["name"] = "Humidity";
	humidity_graph_object["color"] = "#333745";
	humidity_graph_object["y_title"] = "Relative humidity (%RH)";
	JsonArray humidity_graph_data_array = humidity_graph_object["data"].createNestedArray();

	JsonObject temperature_graph_object = jsonDataDocument.createNestedObject();
	temperature_graph_object["name"] = "Temperature";
	temperature_graph_object["color"] = "#FE5F55";
	temperature_graph_object["y_title"] = "Temperature (Deg C)";
	JsonArray temperature_graph_data_array = temperature_graph_object["data"].createNestedArray();

	xSemaphoreGive(jsonDocMutex);  // release control of json doc
}

/**
 * @brief Convert CO2 level in parts per million to a position integer for a light bar display.
 * This function maps the input CO2 level to a position integer between 0 and LIGHTBAR_MAX_POSITION (each pixel has a position range of 0-255).
 * The mapping is linear and is based on the CO2_MIN, CO2_MAX, and LIGHTBAR_MAX_POSITION constants.
 * @param inputCO2 CO2 level in parts per million.
 * @return uint16_t The position integer for the light bar display.
 */
uint16_t mapCO2toPosition(double inputCO2) {
	if (inputCO2 > CO2_MIN) {
		return (uint16_t)((inputCO2 - CO2_MIN) * (LIGHTBAR_MAX_POSITION) / (CO2_MAX - CO2_MIN));
	} else {
		return (uint16_t)0;
	}
}

// Initializes the LED strip to black.
void initializeLightBar(NeoPixelBus<NeoGrbFeature, NeoEsp32I2s0Ws2812xMethod>& lightBar) {
	lightBar.Begin();
	lightBar.Show();
}

// Reads the light sensor and adjusts the brightness of the LED strip based on the light level.
bool updateBrightness(LTR303& lightSensor, uint8_t& brightness, uint8_t& targetBrightness) {
	double lux;
	if (lightSensor.getApproximateLux(lux)) {
		if (lux < (BRIGHTNESS_FACTOR * MAX_BRIGHTNESS)) {
			targetBrightness = (uint8_t)(lux / BRIGHTNESS_FACTOR);
		} else {
			targetBrightness = MAX_BRIGHTNESS;
		}
		if (brightness > targetBrightness) {
			brightness--;
			return true;
		} else if (brightness < targetBrightness) {
			brightness++;
			return true;
		}
	}
	return false;
}

// Updates the position of the lighting effect on the LED strip based on a target position.
void updatePosition(uint16_t& position, const uint16_t& targetPosition) {
	if (targetPosition < LIGHTBAR_MIN_POSITION) {
		position = LIGHTBAR_MIN_POSITION;
	} else if (targetPosition < LIGHTBAR_MAX_POSITION) {  // if position is in valid range
		if (position > targetPosition) {
			position--;
		} else if (position < targetPosition) {
			position += (targetPosition - position) / 32;
			position++;
		}
	}
}

// Updates the lighting effect on the LED strip.
void updateLightBar(NeoPixelBus<NeoGrbFeature, NeoEsp32I2s0Ws2812xMethod>& lightBar, const uint16_t& position, const uint8_t& brightness) {
	uint8_t redGreenMix = position / PIXEL_COUNT;	 // 0 - 255 version of position
	uint16_t mixingPixel = position / 255;			 // which pixel is the position at
	uint8_t mixingPixelBrightness = position % 255;	 // what is the local position of the pixel

	mixingPixel = LAST_PIXEL - mixingPixel;	 // reverse direction of lightbar (0th pixel is the top)

	RgbColor baseColor = RgbColor(redGreenMix, 255 - redGreenMix, 0).Dim(brightness);  // brew base color -> Green at bottom, Red at top

	lightBar.SetPixelColor(mixingPixel - 1, RgbColor(0));						// set one pixel above the mixing pixel to black
	lightBar.SetPixelColor(mixingPixel, baseColor.Dim(mixingPixelBrightness));	// set mixing pixel

	if (mixingPixel < LAST_PIXEL) {
		lightBar.ClearTo(baseColor, mixingPixel + 1, LAST_PIXEL);  // fill solid color to the bottom of the bar
	}

	lightBar.Show();
}

// Handles the target position notification and sets the local target position.
void handleTargetPositionNotification(uint16_t& targetPosition, uint32_t rawNotification, lightBarModes& lightBarMode) {
	uint16_t rawPosition = high16Bits(rawNotification);
	if (rawPosition != 0) {
		targetPosition = rawPosition;
	}

	lightBarModes rawMode = static_cast<lightBarModes>(low8Bits(rawNotification));

	if (rawMode != 0) {
		lightBarMode = rawMode;
	} else if (targetPosition > LIGHTBAR_MAX_POSITION) {
		lightBarMode = flashRed;
	}
	// Serial.printf("%i, %i, %i\n\r", lightBarMode, rawMode, rawPosition);
	return;
}

/**
 * @brief Controls addressable LED pixels and uses the I2C ambient light sensor.
 * This task controls an LED strip based on readings from an ambient light sensor, adjusting the brightness and position of a color gradient on the strip.
 * The gradient blends from green to red, and the position is set using xTaskNotify(). If the target position is above the maximum position, the strip flashes
 * red to indicate high CO2 levels. The code initializes the light strip and enters a loop that reads the light sensor, updates the position and brightness,
 * and shows the effect. There are also modes for testing and flashing a purple pulse.
 *
 * @param[in] parameter The task parameter (unused).
 */
void lightBarTask(void* parameter) {
	// Create the the lightbar object
	NeoPixelBus<NeoGrbFeature, NeoEsp32I2s0Ws2812xMethod> lightBar(PIXEL_COUNT + 1, PIXEL_DATA_PIN);  // uses i2s silicon remapped to any pin to drive led data

	//double lux;	 // The measured illumination level in lux.

	uint8_t targetBrightness = MAX_BRIGHTNESS;	// The target brightness value for the light bar, ranging from 0 to 255.
	uint8_t brightness = 255;					// The current brightness value for the light bar, ranging from 0 to 255.

	uint32_t rawNotification;							  // The raw position value for the light bar, filled from task notification
	uint16_t targetPosition = LIGHTBAR_MAX_POSITION / 3;  // The target position value for the light bar, ranging from 0 to LIGHTBAR_MAX_POSITION.
	uint16_t position = 0;								  // The current position value for the light bar, ranging from 0 to LIGHTBAR_MAX_POSITION.
	lightBarModes lightBarMode = lightBarScale;

	initializeLightBar(lightBar);

	LTR303 lightSensor;
	Wire1.begin(WIRE1_SDA_PIN, WIRE1_SCL_PIN, 500000);	// tested to be 380khz irl (400khz per data sheet)

#ifdef PRODUCTION_TEST
	if (!lightSensor.isConnected(Wire1, &Serial)) {
		lightBarMode = errorRed;
	} else {
		lightBarMode = rgbTest;
	}
#endif

	lightSensor.begin(GAIN_48X, EXPOSURE_100ms, true, Wire1);

	while (true) {
		if (updateBrightness(lightSensor, brightness, targetBrightness) == true) {
			if (lightBarMode == idleFrame) {
				lightBarMode = lightBarScale;
			}
		}
		//Serial.println(brightness);

		for (uint8_t i = 0; i < 4; i++) {	// get light reading every 4 frames
			if (xTaskNotifyWait(0, 0xFFFF, &rawNotification, 0) == pdTRUE) {
				handleTargetPositionNotification(targetPosition, rawNotification, lightBarMode);
			}

			switch (lightBarMode) {
			case idleFrame:
				if (position != targetPosition) {
					lightBarMode = lightBarScale;
				}
				vTaskDelay(pdMS_TO_TICKS(FRAME_TIME));  // time between frames
				break;

			case lightBarScale:
				updatePosition(position, targetPosition);
				updateLightBar(lightBar, position, brightness);
				if (position == targetPosition) {
					lightBarMode = idleFrame;
				}
				vTaskDelay(pdMS_TO_TICKS(FRAME_TIME));  // time between frames
				break;

			case flashRed:
				lightBar.ClearTo(RgbColor(0));
				lightBar.Show();
				vTaskDelay(500 / portTICK_PERIOD_MS);

				lightBar.ClearTo(RgbColor(255, 0, 0));
				lightBar.Show();
				vTaskDelay(500 / portTICK_PERIOD_MS);

				if (targetPosition < LIGHTBAR_MAX_POSITION) {
					lightBarMode = lightBarScale;
					position = LIGHTBAR_MAX_POSITION;
					brightness = MAX_BRIGHTNESS;
				}
				break;

			case purplePulse:
				for (size_t i = 0; i < MAX_BRIGHTNESS; i += 2) {
					lightBar.ClearTo(RgbColor(i, 0, i));
					lightBar.Show();
					vTaskDelay(pdMS_TO_TICKS(FRAME_TIME));  // time between frames
				}
				lightBar.ClearTo(RgbColor(0));
				lightBar.Show();
				lightBarMode = lightBarScale;
				break;

			case greenPulse:
				for (size_t i = 0; i < MAX_BRIGHTNESS; i += 2) {
					lightBar.ClearTo(RgbColor(0, i, 0));
					lightBar.Show();
					vTaskDelay(pdMS_TO_TICKS(FRAME_TIME));  // time between frames
				}
				lightBar.ClearTo(RgbColor(0));
				lightBar.Show();
				lightBarMode = lightBarScale;
				break;

			case rgbTest:  // Flash all pixels red, green, blue to test wiring and config is correct
				lightBar.ClearTo(RgbColor(255, 0, 0));
				lightBar.Show();
				vTaskDelay(1000 / portTICK_PERIOD_MS);
				lightBar.ClearTo(RgbColor(0, 255, 0));
				lightBar.Show();
				vTaskDelay(1000 / portTICK_PERIOD_MS);
				lightBar.ClearTo(RgbColor(0, 0, 255));
				lightBar.Show();
				vTaskDelay(1000 / portTICK_PERIOD_MS);
				lightBar.ClearTo(RgbColor(0, 0, 0));
				lightBar.Show();
				lightBarMode = lightBarScale;
				break;

			case errorRed:
				lightBar.ClearTo(RgbColor(MAX_BRIGHTNESS, 0, 0));
				lightBar.Show();
				vTaskSuspend(NULL);
				break;

			case off:
				lightBar.ClearTo(RgbColor(0));
				lightBar.Show();
				vTaskSuspend(NULL);
				break;

			default:
				break;
			}
		}
	}
}

// Callback function (get's called when time adjusts via NTP)
void onTimeAvailable(struct timeval* t) {
	vTaskResume(lightBar);
	xTaskNotify(lightBar, greenPulse, eSetValueWithOverwrite);
#ifndef OTA
	WiFi.disconnect();
#endif

	time_t epoch;
	struct tm gmt;
	time(&epoch);
	gmtime_r(&epoch, &gmt);
	Serial.println(&gmt, "\n\rGMT Time Set: %A, %B %d %Y %H:%M:%S\n\r");
}

void initializeNTPClient() {
	// Define the NTP servers to be used for time synchronization
	const char* ntpServer1 = "pool.ntp.org";
	const char* ntpServer2 = "time.nist.gov";
	const char* ntpServer3 = "time.google.com";

	sntp_set_time_sync_notification_cb(onTimeAvailable);

	configTzTime(time_zone, ntpServer1, ntpServer2, ntpServer3);
}

void setUpDNSServer(DNSServer& dnsServer, const IPAddress& localIP) {
	// Define the DNS interval in milliseconds between processing DNS requests
#define DNS_INTERVAL 10

	// Set the TTL for DNS response and start the DNS server
	dnsServer.setTTL(3600);
	dnsServer.start(53, "*", localIP);
}

void onClientConnected(WiFiEvent_t event) {
	uint32_t notification = static_cast<uint8_t>(purplePulse);
	xTaskNotify(lightBar, static_cast<uint8_t>(purplePulse), eSetValueWithOverwrite);
	return;
}

void startSoftAccessPoint(const char* password, const IPAddress& localIP, const IPAddress& gatewayIP) {
	// Define the maximum number of clients that can connect to the server
	const uint8_t MAX_CLIENTS = 4;

	// Define the WiFi channel to be used (channel 6 in this case)
	const uint8_t WIFI_CHANNEL = 6;

	// Set the WiFi mode to access point and station
	WiFi.mode(WIFI_MODE_APSTA);

	// Define the subnet mask for the WiFi network
	const IPAddress subnetMask(255, 255, 255, 0);

	// Configure the soft access point with a specific IP and subnet mask
	WiFi.softAPConfig(localIP, gatewayIP, subnetMask);

	// Generate a unique SSID based on the ESP32's MAC address
	char uniqueSSID[18] = { 0 };
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	snprintf(uniqueSSID, sizeof(uniqueSSID), "Kea-CO2-%02X", mac[5]);

	// Start the soft access point with the generated SSID, password, channel, and max number of clients
	WiFi.softAP(uniqueSSID, password, WIFI_CHANNEL, 0, MAX_CLIENTS);

	// Disable AMPDU RX on the ESP32 WiFi to fix a bug on Android
	esp_wifi_stop();
	esp_wifi_deinit();
	wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();
	my_config.ampdu_rx_enable = false;
	esp_wifi_init(&my_config);
	esp_wifi_start();
	vTaskDelay(pdMS_TO_TICKS(100));  // Add a small delay

	// Register an event handler for when a station connects to the soft AP
	WiFi.onEvent(onClientConnected, WiFiEvent_t::ARDUINO_EVENT_WIFI_AP_STACONNECTED);
}

void setUpWebserver(AsyncWebServer& server, const IPAddress& localIP) {
	//======================== Webserver ========================
	// WARNING IOS (and maybe macos) WILL NOT POP UP IF IT CONTAINS THE WORD "Success" https://www.esp8266.com/viewtopic.php?f=34&t=4398
	// SAFARI (IOS) IS STUPID, G-ZIPPED FILES CAN'T END IN .GZ https://github.com/homieiot/homie-esp8266/issues/476 this is fixed by the webserver serve static function.
	// SAFARI (IOS) there is a 128KB limit to the size of the HTML. The HTML can reference external resources/images that bring the total over 128KB
	// SAFARI (IOS) popup browser has some severe limitations (javascript disabled, cookies disabled)

	server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
		request->redirect(localIPURL);
		});

	server.on("/data.json", HTTP_GET, [](AsyncWebServerRequest* request) {	// when client asks for the json data preview file..
		AsyncResponseStream* response = request->beginResponseStream("application/json");
		response->addHeader("Cache-Control:", "max-age=5");
		xSemaphoreTake(jsonDocMutex, 1);			 // ask for control of json doc
		serializeJson(jsonDataDocument, *response);	 // turn the json document in ram into a normal json file (as a stream of data)
		xSemaphoreGive(jsonDocMutex);				 // release control of json doc
		request->send(response);
		});

	server.on("/Kea-CO2-Data.csv", HTTP_GET, [](AsyncWebServerRequest* request) {
		AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/Kea-CO2-Data.csv", String(), true);
		request->send(response);
		});

	server.on("/yesclear.html", HTTP_GET, [](AsyncWebServerRequest* request) {
		request->redirect(localIPURL);	// return the user back to the home page

		initializeJson();  // clears the data file

		xTaskNotify(jsonFileManager, 1, eSetValueWithOverwrite);  // notification value of 1 instructs jsonFileManager to clear data
		vTaskResume(jsonFileManager);							  // jsonFileManager is usually left in paused state until needed, resuming it here.

		xTaskNotify(csvFileManager, 1, eSetValueWithOverwrite);	 // notification value of 1 instructs csvFileManager to clear data
		vTaskResume(csvFileManager);							 // csvFileManager is usually left in paused state until needed, resuming it here.

		ESP_LOGI("", "data clear Requested");
		});

	server.on("/off", HTTP_GET, [](AsyncWebServerRequest* request) {
		request->redirect(localIPURL);
		uint32_t notification = static_cast<uint32_t>(off);
		xTaskNotify(lightBar, notification, eSetValueWithOverwrite);
		ESP_LOGI("", "led off Requested");
		});

	server.on("/favicon.ico", [](AsyncWebServerRequest* request) { request->send(404); });

	// Required for captive portal redirects
	server.on("/connecttest.txt", [](AsyncWebServerRequest* request) { request->redirect("http://logout.net"); });	// windows 11 captive portal workaround
	server.on("/wpad.dat", [](AsyncWebServerRequest* request) { request->send(404); });								// Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32 :)

	// Background responses: Probably not all are Required, but some are. Others might speed things up?
	// A Tier (commonly used by modern systems)
	server.on("/generate_204", [](AsyncWebServerRequest* request) { request->redirect(localIPURL); });		   // android captive portal redirect
	server.on("/redirect", [](AsyncWebServerRequest* request) { request->redirect(localIPURL); });			   // microsoft redirect
	server.on("/hotspot-detect.html", [](AsyncWebServerRequest* request) { request->redirect(localIPURL); });  // apple call home
	server.on("/canonical.html", [](AsyncWebServerRequest* request) { request->redirect(localIPURL); });	   // firefox captive portal call home
	server.on("/success.txt", [](AsyncWebServerRequest* request) { request->send(200); });					   // firefox captive portal call home
	server.on("/ncsi.txt", [](AsyncWebServerRequest* request) { request->redirect(localIPURL); });			   // windows call home

	// B Tier (uncommon)
	// server.on("/chrome-variations/seed", [](AsyncWebServerRequest *request) { request->send(200); }); // chrome captive portal call home
	// server.on("/service/update2/json",   [](AsyncWebServerRequest *request) { request->send(200); }); // firefox?
	// server.on("/chat",                   [](AsyncWebServerRequest *request) { request->send(404); }); // No stop asking Whatsapp, there is no internet connection
	// server.on("/startpage",              [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });

	server.serveStatic("/", LittleFS, "/").setCacheControl("max-age=86400");  // serve any file on the device when requested (24hr cache limit)

	server.onNotFound([](AsyncWebServerRequest* request) {
		request->redirect(localIPURL);

#ifdef TEST_WEBSERVER
		Serial.print("onnotfound ");
		Serial.print(request->host());	// This gives some insight into whatever was being requested on the serial monitor
		Serial.print(" ");
		Serial.print(request->url());
		Serial.println(" sent redirect to " + localIPURL + "\n");
#endif
		});
}

/**
 * @brief Runs the webserver, all WiFi functions, and sets up NTP servers.
 *
 * This task
 *  - initializes and runs the webserver,
 *  - handles all WiFi functionality,
 *  - sets up SNTP client for NTP time synchronization
 *
 * It configures the WiFi mode as an access point and sets the IP address, gateway and subnet mask.
 * It also sets up a DNSServer to handle DNS requests and initializes the SNTP client to use the
 * specified NTP servers.
 *
 * The webserver serves the following routes:
 * - "/" redirects to the local IP address.
 * - "/data.json" returns a JSON representation of the sensor data.
 * - "/Kea-CO2-Data.csv" returns the Kea-CO2 data file.
 * - "/yesclear.html" clears the sensor data.
 * - "/off" turns off the light bar.
 *
 * @param[in] parameter The task parameter (unused).
 */
void webserverTask(void* parameter) {
	// Create a DNS server instance
	DNSServer dnsServer;
	// Create an AsyncWebServer instance listening on port 80
	AsyncWebServer server(80);

	initializeNTPClient();

	startSoftAccessPoint(password, localIP, gatewayIP);

	setUpDNSServer(dnsServer, localIP);

	setUpWebserver(server, localIP);
	server.begin();

#ifdef OTA
	WiFi.begin(ssid, netpassword);

	for (size_t i = 0; i < 50 && WiFi.waitForConnectResult() != WL_CONNECTED; i++) {
		vTaskDelay(100 / portTICK_PERIOD_MS);
	}

	if (WiFi.waitForConnectResult() != WL_CONNECTED) {
		Serial.println("Connection Failed! Rebooting...");
		ESP.restart();
	}

	telnet.begin(23);
	ArduinoOTA.setPort(3232);

	ArduinoOTA
		.onStart([]() {

		String type;
		if (ArduinoOTA.getCommand() == U_FLASH)
			type = "sketch";
		else  // U_SPIFFS
			type = "filesystem";

		// NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
		Serial.println("Start updating " + type);
			})
		.onEnd([]() {
		Serial.println("\nEnd");
			})
		.onProgress([](unsigned int progress, unsigned int total) {
		Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
			})
		.onError([](ota_error_t error) {
		Serial.printf("Error[%u]: ", error);
		if (error == OTA_AUTH_ERROR)
			Serial.println("Auth Failed");
		else if (error == OTA_BEGIN_ERROR)
			Serial.println("Begin Failed");
		else if (error == OTA_CONNECT_ERROR)
			Serial.println("Connect Failed. Firewall Issue ?");
		else if (error == OTA_RECEIVE_ERROR)
			Serial.println("Receive Failed");
		else if (error == OTA_END_ERROR)
			Serial.println("End Failed");
			});

	ArduinoOTA.setTimeout(30000);
	ArduinoOTA.begin();
#else
	WiFi.begin("time", "12345678");
	WiFi.setAutoReconnect(false); //critically needed
	WiFi.setTxPower(WIFI_POWER_2dBm);
#endif

	ESP_LOGV("WiFi Tx Power Set To:", "%i", (WiFi.getTxPower()));

	ESP_LOGV("", "Startup completed by %ims", (millis()));

	while (true) {
		dnsServer.processNextRequest();

#ifdef OTA
		ArduinoOTA.handle();
		telnet.loop();
#endif

		vTaskDelay(DNS_INTERVAL / portTICK_PERIOD_MS);
	}
}

/**
 * @brief Initializes a CSV file and adds a header if needed.
 *
 * This function creates a new CSV file with the given filename and adds a header to it if it doesn't exist.
 * If a file with the same filename already exists and has some data in it, it is left alone.
 *
 * @param[in] filename The name of the CSV file.
 * @return True if the CSV was successfully initialized, false otherwise.
 */
bool initializeCsvFile(const char* filename, char* csvHeader) {
	// Attempt to open the CSV file (create if it doesn't exist)
	File file = LittleFS.open(filename, FILE_APPEND, true);
	if (!file) {
		ESP_LOGE("", "Unable to open %s. Aborting task.", filename);
		return false;
	}

	// Check if the CSV file already has data
	bool fileHasData = file.size() > 0;

	// If the file is empty, add the CSV header
	if (!fileHasData) {
		if (!file.print(csvHeader)) {
			ESP_LOGE("", "Error writing CSV header to file %s", filename);
			file.close();
			return false;
		}
	}

	file.close();
	return true;
}

/**
 * @brief Takes a queue of char arrays and adds it to the CSV file in flash storage.
 * This function opens a CSV file named CSVLogFilename, either by appending to an existing file or creating a new one.
 * If the file is empty, a CSV header is added. The function then sets up the CSV file buffer and waits for
 * notifications.
 *
 * When a delete file notification is received, the CSV file is closed, removed, and re-opened with a new header.
 *
 * If the buffer is nearly full, the data is written to the file and the buffer is flushed. If the buffer is full, incoming data is ignored until the file is
 * cleared. If the CSV file exceeds MAX_CSV_SIZE_BYTES, incoming data is also ignored until the file is cleared.
 * @param[in] parameter The task parameter (unused).
 */
void csvFileManagerTask(void* parameter) {
	// Generate a unique header based on the ESP32's MAC address
	char csvHeader[128];
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	snprintf(csvHeader, sizeof(csvHeader), "Kea-CO2-%02X (D/M/Y), Time(H:M), CO2(PPM), Humidity(\%RH), Temperature(DegC)\r\n", mac[5]);
	const uint32_t BUFFER_SIZE = 256;
	const uint32_t FLUSH_THRESHOLD = 128;
	const uint32_t FLUSH_EVERY_THRESHOLD = 512;

	if (!initializeCsvFile(CSVLogFilename, csvHeader)) {
		ESP_LOGE("", "Unable to initialize %s. Aborting task.", CSVLogFilename);
		vTaskDelete(NULL);
	}

	File csvDataFile = LittleFS.open(CSVLogFilename, FILE_APPEND);
	if (!csvDataFile) {
		ESP_LOGE("", "Unable to open %s. Aborting task.", CSVLogFilename);
		vTaskDelete(NULL);
	}

	uint32_t bufferSizeNow = 0;
	csvDataFile.setBufferSize(BUFFER_SIZE);
	uint32_t csvDataFilesize = csvDataFile.size();

	while (true) {
		// Wait for notifications
		uint32_t notification = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		// Handle delete file notification
		if (notification > 0) {
			ESP_LOGI("", "Received delete file notification for %s", CSVLogFilename);
			csvDataFile.close();
			LittleFS.remove(CSVLogFilename);

			if (!initializeCsvFile(CSVLogFilename, csvHeader)) {
				ESP_LOGE("", "Unable to initialize %s. Aborting task.", CSVLogFilename);
				vTaskDelete(NULL);
			}

			csvDataFile = LittleFS.open(CSVLogFilename, FILE_APPEND);
			if (!csvDataFile) {
				ESP_LOGE("", "Unable to open %s. Aborting task.", CSVLogFilename);
				vTaskDelete(NULL);
			}

			bufferSizeNow = 0;
			csvDataFile.setBufferSize(BUFFER_SIZE);
			csvDataFilesize = csvDataFile.size();
		}

		char csvLine[CSV_LINE_MAX_CHARS];
		if (xQueueReceive(charsForCSVFileQueue, csvLine, 0) == pdTRUE) {
			// Write line to CSV file
			if (csvDataFilesize < MAX_CSV_SIZE_BYTES) { // Prevent memory leaks
				uint8_t bytesAdded = csvDataFile.println(csvLine);

				if (bytesAdded > 0) {
					bufferSizeNow += bytesAdded;

					if (bufferSizeNow > FLUSH_THRESHOLD || csvDataFilesize < FLUSH_EVERY_THRESHOLD) {
						csvDataFile.flush();
						csvDataFilesize += bufferSizeNow;
						bufferSizeNow = 0;
						ESP_LOGI("", "%s Flushed to Flash Storage", CSVLogFilename);
					}
				} else {
					ESP_LOGE("", "Error writing to %s", CSVLogFilename);
				}
			}
		}
	}
}

/**
 * @brief Takes a queue of data points and adds it to the JSON data document in RAM
 * This function initializes the JSON document and continuously waits for a
 * notification. When a notification is received and the JSON data queue contains
 * three elements, it retrieves the elements and adds them to the JSON document.
 * The function also updates the index for each data type (CO2, humidity, and temperature)
 * to ensure that the data is stored in a circular buffer. The function also acquires and
 * releases a mutex to ensure that the JSON document is accessed safely in a multithreaded environment.
 * @param[in] parameter The task parameter (unused).
 */
void jsonFileManagerTask(void* parameter) {
	time_t currentEpoch, prevEpoch = 0;
	uint8_t CO2Index = 0, tempIndex = 0, humidityIndex = 0;
	uint32_t notification;
	double CO2, temperature, humidity;
	double prevCO2 = 0, prevTemperature = 0, prevHumidity = 0;

	initializeJson();

	while (true) {
		vTaskSuspend(NULL);

		xTaskNotifyWait(0, 65535, &notification, 0);
		if (notification > 0) {
			CO2Index = 0, tempIndex = 0, humidityIndex = 0;
			prevCO2 = 0, prevTemperature = 0, prevHumidity = 0;
		}

		if (uxQueueMessagesWaiting(jsonDataQueue) == 3) {
			time(&currentEpoch);
			if (currentEpoch > prevEpoch) {
				//----- SCD40 Data -------------------------------
				xQueueReceive(jsonDataQueue, &CO2, 10);
				xQueueReceive(jsonDataQueue, &humidity, 10);
				xQueueReceive(jsonDataQueue, &temperature, 10);

				// Serial.printf("%i,%4.0f,%2.1f,%2.0f\n\r", currentEpoch, CO2, temperature, humidity);

				xSemaphoreTake(jsonDocMutex, 1000 / portTICK_PERIOD_MS);  // ask for control of json doc
				//------------------------------------------------------
				if ((int)CO2 != (int)prevCO2) {
					jsonDataDocument[0]["data"][CO2Index][1] = (int)CO2;
					jsonDataDocument[0]["data"][CO2Index][0] = currentEpoch;
					CO2Index = (CO2Index < JSON_DATA_POINTS_MAX) ? (CO2Index + 1) : (0);  // increment from 0 -> JSON_DATA_POINTS_MAX -> 0 -> etc...
				}

				if ((int)humidity != (int)prevHumidity) {
					jsonDataDocument[1]["data"][humidityIndex][1] = (int)humidity;
					jsonDataDocument[1]["data"][humidityIndex][0] = currentEpoch;
					humidityIndex = (humidityIndex < JSON_DATA_POINTS_MAX) ? (humidityIndex + 1) : (0);	 // increment from 0 -> JSON_DATA_POINTS_MAX -> 0 -> etc...
				}

				if (roundToXDP(1, temperature) != roundToXDP(1, prevTemperature)) {
					jsonDataDocument[2]["data"][tempIndex][1] = roundToXDP(1, temperature);
					jsonDataDocument[2]["data"][tempIndex][0] = currentEpoch;
					tempIndex = (tempIndex < JSON_DATA_POINTS_MAX) ? (tempIndex + 1) : (0);	 // increment from 0 -> JSON_DATA_POINTS_MAX -> 0 -> etc...
				}
				//------------------------------------------------------
				xSemaphoreGive(jsonDocMutex);  // release control of json doc

				prevCO2 = CO2;
				prevHumidity = humidity;
				prevTemperature = temperature;
				prevEpoch = currentEpoch;
			}
		}
	}
}

/**
 * @brief Runs the CO2 sensor and RTC, puts data in CSV and JSON queues.
 *
 * This function initializes the PCF8563 RTC and SCD4X CO2 sensor, reads CO2, temperature,
 * and humidity data periodically, and puts the data into the CSV and JSON queues for file
 * management tasks to process. It also sends a notification to the `lightBar` task to
 * update the LED light bar based on the CO2 measurement. If the current time is more
 * than `CSV_RECORD_INTERVAL_SECONDS` seconds from the previous record time, a new record
 * is added to the CSV file. If the time has not been set yet and at least one NTP server
 * is reachable, the RTC is synchronized with the current time and a message is printed
 * to the serial monitor.
 *
 * @param[in] parameter The task parameter (unused).
 */
void sensorManagerTask(void* parameter) {
	PCF8563_Class rtc;
	SCD4X co2;

	const char* time_format = "%d/%m/%Y,%H:%M";

	Wire.begin(WIRE_SDA_PIN, WIRE_SCL_PIN, 100000);

	rtc.begin(Wire);

	rtc.disableAlarm();
	rtc.resetAlarm();

	if (rtc.syncToSystem() == true) {
		setenv("TZ", time_zone, 1);
		tzset();
	} else {
		xTaskNotify(lightBar, errorRed, eSetValueWithOverwrite);
	}

	co2.begin(Wire);

#ifdef PRODUCTION_TEST
	if (co2.isConnected(Wire, &Serial) == false) {
		xTaskNotify(lightBar, errorRed, eSetValueWithOverwrite);
	}
	co2.resetEEPROM();
	co2.setCalibrationMode(false);
	co2.saveSettings();
#endif

	double CO2, rawTemperature, temperature = 20.0, rawHumidity, humidity = 0.0;
	double prevCO2 = 0, trendCO2 = 0;
	uint16_t lightbarPosition;

	time_t currentEpoch;
	time(&currentEpoch);
	time_t prevEpoch = currentEpoch;
	uint32_t notification;

	bool timeSet = false;

	co2.startPeriodicMeasurement();

	while (true) {
		vTaskDelay(4700 / portTICK_PERIOD_MS);	// chill while scd40 gets new data
		while (co2.isDataReady() == false) {
			vTaskDelay(30 / portTICK_PERIOD_MS);
		}

		if (co2.readMeasurement(CO2, rawTemperature, rawHumidity) == 0) {
			if (prevCO2 == 0) {
				prevCO2 = CO2;
			}

			trendCO2 = 0.5 * (CO2 - prevCO2) + (1 - 0.5) * trendCO2;
			lightbarPosition = mapCO2toPosition(CO2 + trendCO2);
			uint32_t notification = lightbarPosition << 16; //put the lightbar Position in the top 16bits (bottom 8bits is the mode)
			xTaskNotify(lightBar, notification, eSetValueWithoutOverwrite);

			rawTemperature -= TEMP_OFFSET;

			temperature = temperature + (rawTemperature - temperature) * 0.5;
			humidity = humidity + (rawHumidity - humidity) * 0.5;

			xQueueReset(jsonDataQueue);
			xQueueSend(jsonDataQueue, &CO2, 0);
			xQueueSend(jsonDataQueue, &humidity, 0);
			xQueueSend(jsonDataQueue, &temperature, 0);
			vTaskResume(jsonFileManager);

			prevCO2 = CO2;
			// Serial.printf("%4.0f,%2.1f,%1.0f\n", CO2, temperature, humidity);
		}

		time(&currentEpoch);
		if (prevEpoch + CSV_RECORD_INTERVAL_SECONDS <= currentEpoch) {
			prevEpoch += CSV_RECORD_INTERVAL_SECONDS;

			struct tm timeInfo;
			localtime_r(&currentEpoch, &timeInfo);
			char timeStamp[24];
			strftime(timeStamp, sizeof(timeStamp), time_format, &timeInfo);

			char buf[CSV_LINE_MAX_CHARS];  // temp char array for CSV 40000,99,99
			// CO2(PPM),Humidity(%RH),Temperature(DegC)"
			sprintf(buf, "%s,%3.0f,%2.0f,%2.1f", timeStamp, CO2, humidity, temperature);
			xQueueSend(charsForCSVFileQueue, &buf, 1000 / portTICK_PERIOD_MS);
			Serial.println(buf);

			vTaskResume(csvFileManager);
		}

		if (timeSet == false && (sntp_getreachability(0) + sntp_getreachability(1) + sntp_getreachability(2) > 0)) {
			rtc.syncToRtc();
			timeSet = true;
		}
	}
}

void setup() {
	// Create a task for controlling the light bar.
	// Parameters are: task function, name for debugging, stack size, parameters to pass to task function, priority, pointer to task handle.
	xTaskCreate(lightBarTask, "lightBar", 4200, NULL, 2, &lightBar);

	// Set the transmit buffer size for the Serial object and start it with a baud rate of 115200.
	Serial.setTxBufferSize(1024);
	Serial.begin(115200);

	// Wait for the Serial object to become available.
	while (!Serial)
		;
	Serial.printf("\r\n Kea CO2 \r\n %s compiled on " __DATE__ " at " __TIME__ " \r\n %s%s in the %s environment \r\n\r\n", USER, VERSION, TAG, ENV);

	// Print chip model and revision if production test mode is enabled.
#ifdef PRODUCTION_TEST
	Serial.printf("%s-%d\n\r", ESP.getChipModel(), ESP.getChipRevision());
#endif

	// Create a queue for storing characters for the CSV file.
	// Parameters are: maximum number of items in the queue, size of each item in bytes.
	char csvLine[CSV_LINE_MAX_CHARS];
	charsForCSVFileQueue = xQueueCreate(3, sizeof(csvLine));

	// Create a queue for storing CO2, humidity, and temperature data
	// Parameters are: maximum number of items in the queue, size of each item in bytes.
	jsonDataQueue = xQueueCreate(3, sizeof(double));

	// Create a mutex for controlling access to the JSON document used for storing data for the webserver.
	jsonDocMutex = xSemaphoreCreateMutex();

	// Parameters are: task function, name for debugging, stack size, parameters to pass to task function, priority, pointer to task handle.
	xTaskCreate(sensorManagerTask, "sensorManagerTask", 3800, NULL, 1, &sensorManager);
	xTaskCreate(jsonFileManagerTask, "jsonFileManagerTask", 21000, NULL, 0, &jsonFileManager);

	// Initialize LittleFS (ESP32 Storage) and format it if it fails to mount.
	if (LittleFS.begin(true) == false) {
		xTaskNotify(lightBar, errorRed, eSetValueWithOverwrite);
		ESP_LOGE("", "Error mounting LittleFS (Even with Format on Fail)");
	}

#ifdef PRODUCTION_TEST
	LittleFS.remove(F(CSVLogFilename));
#endif
	ESP_LOGI("LittleFS", "unused storage = %ikib", (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024);
	if (LittleFS.exists("/index.html.gz") == false) {
		ESP_LOGE("LittleFS", "index.html.gz doesn't exist");
		xTaskNotify(lightBar, errorRed, eSetValueWithOverwrite);
	}

	// Parameters are: task function, name for debugging, stack size, parameters to pass to task function, priority, pointer to task handle.
	xTaskCreate(webserverTask, "webserverTask", 17060, NULL, 1, &webserver);
	xTaskCreate(csvFileManagerTask, "csvFileManagerTask", 4000, NULL, 0, &csvFileManager);
}

void loop() {
	vTaskSuspend(NULL);	 // Loop task Not Needed
}