/*!
 * @file  main.cpp
 * @brief  keaStudios CO2 Sensor Firmware
 * @copyright Chris Dirks
 * @license  HIPPOCRATIC LICENSE Version 3.0
 * @author  @CDFER (Chris Dirks)
 * @created 14/02/2023
 * @url  http://www.keastudios.co.nz
 *
 *
 * ESP32 Wroom Module
 * Lights up a strip of WS2812B Addressable RGB LEDs to display a scale of the ambient CO2 level (Light Bar)
 * CO2 data is from a Sensirion SCD40 or SCD41
 * The LEDs are adjusted depending on the ambient Light
 * There is also a webserver which displays graphs of CO2, Humidity and Temperature while also providing a csv download for that data
 */

#include <Arduino.h>

// Wifi, Webserver and DNS
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "AsyncJson.h"
#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"

// Onboard Flash Storage
#include <LittleFS.h>

// Light Bar
#include <NeoPixelBus.h>

// LTR303 (Light sensor)
#include "ltr303.h"

// SCD4x (CO2 sensor)
#include "scd4x.h"

// pcf8563 (Backup Clock)
#include "pcf8563.h"
#include "sntp.h"
#include "time.h"

// -----------------------------------------
//
//    Hardware Config
//
// -----------------------------------------
#define WIRE_SDA_PIN 21
#define WIRE_SCL_PIN 22
#define WIRE1_SDA_PIN 33
#define WIRE1_SCL_PIN 32
#define PIXEL_DATA_PIN 16  // GPIO -> LEVEL SHIFT -> Pixel 1 Data In Pin
#define PIXEL_COUNT 11	   // Number of Addressable Pixels to write data to (starts at pixel 1)

#define TEMP_OFFSET 6.4	   // The Enclosure runs a bit hot reduce to get a more accurate ambient

// -----------------------------------------
//
//    LightBar Config
//
// -----------------------------------------
#define CO2_MAX 2000	   // top of the CO2 LightBar (also when it transitions to warning Flash)
#define CO2_MIN 450		   // bottom of the LightBar
#define FRAME_TIME 30	   // Milliseconds between frames 30ms = ~33.3fps maximum
#define BRIGHTNESS_FACTOR 3	   // lux/BRIGHTNESS_FACTOR = led brightness
#define MAX_BRIGHTNESS 200	   // max brighitness setting for ws2812b1's

// -----------------------------------------
//
//    Helpful Defines (Don't Touch)
//
// -----------------------------------------
#define LIGHTBAR_MAX_POSITION PIXEL_COUNT * 255
#define LAST_PIXEL PIXEL_COUNT - 1	// last Addressable Pixel to write data to (starts at pixel 0)

// -----------------------------------------
//
//    Webserver Settings
//
// -----------------------------------------
const char *time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";	// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
const char *ssid = "Kea-CO2";  // Name of the Wifi Access Point, FYI The SSID can't have a space in it.
const char *password = "";	   // Password of the Wifi Access Point, leave as "" for no Password

#define CSV_RECORD_INTERVAL_SECONDS 60
#define JSON_RECORD_INTERVAL_SECONDS 1

char CSVLogFilename[] = "/Kea-CO2-Data.csv";  // location of the csv file
#define MAX_CSV_SIZE_BYTES 2000000			  // set max size at 2mb

const IPAddress localIP(4, 3, 2, 1);					// the IP address the webserver, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1);					// IP address of the network (should be the same as the local IP in most cases)
const String localIPURL = "http://4.3.2.1/index.html";	// URL to the webserver (Same as the local IP but as a string with http and the landing page subdomain)

// -----------------------------------------
//
//    Global Variables
//
// -----------------------------------------
// Allocate the JSON document size in RAM https://arduinojson.org/v6/assistant to compute the size.
DynamicJsonDocument jsonDataDocument(23520); //18816 bytes for 128 points used in testing, 25% buffer
#define JSON_DATA_POINTS_MAX 128

TaskHandle_t lightBar = NULL;
TaskHandle_t csvFileManager = NULL;
TaskHandle_t sensorManager = NULL;
TaskHandle_t webserver = NULL;
TaskHandle_t jsonFileManager = NULL;

QueueHandle_t charsForCSVFileQueue; // kinda like a letterbox that holds a char array of sensor data
QueueHandle_t jsonDataQueue; // a queue of data points in doubles

SemaphoreHandle_t jsonDocMutex; //kinda like a lock so only one task accesses the json doc at once

/// @brief takes double and returns a float with x decimal places
/// @param decimalPlaces 0 -> 1.000, 1 -> 1.100, 2-> 1.110
/// @param value input raw number
/// @return output float
float roundToXDP(uint8_t decimalPlaces, double value) {	 // rounds a number to x decimal place
	return (float)(((int)(value * (10 * decimalPlaces) + 0.5)) / (10 * decimalPlaces));
}

/// @brief set jsonDataDocument to default state (no data points but has graph colors)
void initJson() {
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

/// @brief converter ppm of CO2 to a light bar position integer
/// @param inputCO2 (double CO2 level in ppm)
/// @return uint16_t LIGHTBAR POSITION (each pixel has 255 positions)
uint16_t mapCO2toPosition(double inputCO2) {
	if (inputCO2 > CO2_MIN) {
		return (uint16_t)((inputCO2 - CO2_MIN) * (LIGHTBAR_MAX_POSITION) / (CO2_MAX - CO2_MIN));
	} else {
		return (uint16_t) 0;
	}
}

/// @brief set Light Bar to Black
void wipeLightBar(void *parameter){
	NeoPixelBus<NeoGrbFeature, NeoEsp32I2s1Ws2812xMethod> lightBar(PIXEL_COUNT, PIXEL_DATA_PIN);  // uses i2s silicon remapped to any pin to drive led data

	lightBar.Begin();
	lightBar.Show();  // init to black
	vTaskDelete(NULL);
}

/// @brief controls addressable led pixels and uses the i2c ambient light sensor
void lightBarTask(void *parameter) {
	NeoPixelBus<NeoGrbFeature, NeoEsp32I2s1Ws2812xMethod> lightBar(PIXEL_COUNT, PIXEL_DATA_PIN);  // uses i2s silicon remapped to any pin to drive led data

	lightBar.Begin();
	lightBar.Show();  // init to black

	LTR303 lightSensor;
	Wire1.begin(WIRE1_SDA_PIN, WIRE1_SCL_PIN, 500000); //tested to be 380khz irl (400khz per data sheet)
	lightSensor.begin(GAIN_48X, EXPOSURE_400ms, true, Wire1);
#ifdef PRODUCTION_TEST
	lightSensor.isConnected(Wire1, &Serial);
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
#endif

	double lux;
	uint8_t targetBrightness = 255;
	uint8_t brightness = 255;

	uint32_t rawPosition = 0;
	uint16_t targetPosition = LIGHTBAR_MAX_POSITION/3;
	uint16_t position = 0;

	RgbColor baseColor;
	uint8_t redGreenMix;
	uint16_t mixingPixel;
	uint8_t mixingPixelBrightness;

	bool overMaxPosition = false;
	bool updateBrightness = false;

	while (true) {
		if (lightSensor.getApproximateLux(lux)) {
			if (lux < BRIGHTNESS_FACTOR * MAX_BRIGHTNESS) {
				targetBrightness = (uint16_t)(lux / BRIGHTNESS_FACTOR);
			} else {
				targetBrightness = MAX_BRIGHTNESS;
			}
			if (brightness > targetBrightness) {
				brightness--;
				updateBrightness = true;
			} else if (brightness < targetBrightness) {
				brightness++;
				updateBrightness = true;
			}
		}

		for (uint8_t i = 0; i < 20; i++) { //get light reading every 20 frames (600ms at 30ms frames)
			xTaskNotifyWait(0, 65535, &rawPosition, 0);

			if (rawPosition > 0) {
				if (rawPosition < 65535) {
					targetPosition = (uint16_t)rawPosition;
				}
				//Serial.println(targetPosition);
			}

			if (targetPosition < LIGHTBAR_MAX_POSITION) {
				if (position != targetPosition || updateBrightness == true) {  // only update leds when something has changed
					if (position > targetPosition) {
						position--;
					} else if (position < targetPosition) {
						position += (targetPosition - position) / 32;
						position++;
					}

					redGreenMix = position / PIXEL_COUNT;
					mixingPixel = position / 255;
					mixingPixelBrightness = position % 255;

					mixingPixel = LAST_PIXEL - mixingPixel;

					baseColor = RgbColor(redGreenMix, 255 - redGreenMix, 0).Dim(brightness);

					lightBar.SetPixelColor(mixingPixel - 1, RgbColor(0));
					lightBar.SetPixelColor(mixingPixel, baseColor.Dim(mixingPixelBrightness));
					
					if (mixingPixel < LAST_PIXEL) {
						lightBar.ClearTo(baseColor, mixingPixel + 1, LAST_PIXEL);
					}

					lightBar.Show();
					updateBrightness = false;
				}
				vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // time between frames
			} else {
				// flash red to indicate CO2 level over CO2_MAX
				lightBar.ClearTo(RgbColor(0));
				lightBar.Show();
				vTaskDelay(500 / portTICK_PERIOD_MS);

				lightBar.ClearTo(RgbColor(255, 0, 0));
				lightBar.Show();
				vTaskDelay(500 / portTICK_PERIOD_MS);

				position = LIGHTBAR_MAX_POSITION;
				brightness = MAX_BRIGHTNESS;
			}
		}
		
	}
}

/// @brief runs webserver, all Wifi functions, sets up NTP servers
void webserverTask(void *parameter) {
#define DNS_INTERVAL 30	 // ms between processing dns requests: dnsServer.processNextRequest();

#define MAX_CLIENTS 4
#define WIFI_CHANNEL 6	// 2.4ghz channel 6

	bool lightBarOn = true;

	const char *ntpServer1 = "pool.ntp.org";
	const char *ntpServer2 = "time.nist.gov";
	const char *ntpServer3 = "time.google.com";

	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, (char *)ntpServer1);
	sntp_setservername(1, (char *)ntpServer2);
	sntp_setservername(2, (char *)ntpServer3);
	sntp_init();

	WiFi.mode(WIFI_MODE_APSTA);

	const IPAddress subnetMask(255, 255, 255, 0);

	DNSServer dnsServer;
	AsyncWebServer server(80);
	
	WiFi.softAPConfig(localIP, gatewayIP, subnetMask);	// Samsung requires the IP to be in public space
	WiFi.softAP(ssid, password, WIFI_CHANNEL, 0, MAX_CLIENTS);

	dnsServer.setTTL(300);				// set 5min client side cache for DNS
	dnsServer.start(53, "*", localIP);	// if DNSServer is started with "*" for domain name, it will reply with provided IP to all DNS request

	// ampdu_rx_disable android workaround see https://github.com/espressif/arduino-esp32/issues/4423
	esp_wifi_stop();
	esp_wifi_deinit();

	wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();	// We use the default config ...
	my_config.ampdu_rx_enable = false;							//... and modify only what we want.

	esp_wifi_init(&my_config);			   // set the new config
	esp_wifi_start();					   // Restart WiFi
	vTaskDelay(100 / portTICK_PERIOD_MS);  // this is necessary don't ask me why
	
	//======================== Webserver ========================
	// WARNING IOS (and maybe macos) WILL NOT POP UP IF IT CONTAINS THE WORD "Success" https://www.esp8266.com/viewtopic.php?f=34&t=4398
	// SAFARI (IOS) IS STUPID, G-ZIPPED FILES CAN'T END IN .GZ https://github.com/homieiot/homie-esp8266/issues/476 this is fixed by the webserver serve static function.
	// SAFARI (IOS) there is a 128KB limit to the size of the HTML. The HTML can reference external resources/images that bring the total over 128KB
	// SAFARI (IOS) popup browser has some severe limitations (javascript disabled, cookies disabled)

	//------------- Setup json -----------

	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->redirect(localIPURL);
	});

	server.on("/data.json", HTTP_GET, [](AsyncWebServerRequest *request) {	// when client asks for the json data preview file..
		AsyncResponseStream *response = request->beginResponseStream("application/json");
		response->addHeader("Cache-Control:", "max-age=5");
		//response->addHeader("Cache-Control:", "no-store");
		xSemaphoreTake(jsonDocMutex, 1);  // ask for control of json doc
		serializeJson(jsonDataDocument, *response);	 // turn the json document in ram into a normal json file (as a stream of data)
		xSemaphoreGive(jsonDocMutex);				 // release control of json doc
		request->send(response);
	});

	//server.serveStatic("/Kea-CO2-Data.csv", LittleFS, "/Kea-CO2-Data.csv").setCacheControl("no-store");	 // do not cache

	server.on("/Kea-CO2-Data.csv", HTTP_GET, [](AsyncWebServerRequest *request) {  // when client asks for the json data preview file..
		AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/Kea-CO2-Data.csv", String(), true);
		request->send(response);
	});

	server.on("/yesclear.html", HTTP_GET, [](AsyncWebServerRequest *request) {	// when client asks for the json data preview file..
		request->redirect(localIPURL);
		initJson();
		xTaskNotify(jsonFileManager, 1, eSetValueWithOverwrite);
		vTaskResume(jsonFileManager);
		xTaskNotify(csvFileManager, 1, eSetValueWithOverwrite);
		vTaskResume(csvFileManager);

		ESP_LOGI("", "data clear Requested");
	});

	// server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request) {	// when client asks for the json data preview file..
	// 	request->redirect(localIPURL);
	// 	xTaskCreate(lightBarTask, "lightBar", 4200, NULL, 2, &lightBar);
	// 	ESP_LOGI("", "led on Requested");
	// });

	server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request) {	 // when client asks for the json data preview file..
		request->redirect(localIPURL);
		vTaskDelete(lightBar);
		xTaskCreate(wipeLightBar, "wipeLightBar", 5000, NULL, 1, NULL);
		ESP_LOGI("", "led off Requested");
	});

	// Required
	server.on("/connecttest.txt", [](AsyncWebServerRequest *request) { request->redirect("http://logout.net"); });	// windows 11 captive portal workaround
	server.on("/wpad.dat", [](AsyncWebServerRequest *request) { request->send(404); });								// Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32 :)
	server.on("/favicon.ico", [](AsyncWebServerRequest *request) { request->send(404); });

	// Background responses: Probably not all are Required, but some are. Others might speed things up?
	// A Tier (commonly used by modern systems)
	server.on("/generate_204", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });		   // android captive portal redirect
	server.on("/redirect", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });			   // microsoft redirect
	server.on("/hotspot-detect.html", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });  // apple call home
	server.on("/canonical.html", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });	   // firefox captive portal call home
	server.on("/success.txt", [](AsyncWebServerRequest *request) { request->send(200); });					   // firefox captive portal call home
	server.on("/ncsi.txt", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });			   // windows call home

	server.serveStatic("/", LittleFS, "/").setCacheControl("max-age=86400");  // serve any file on the device when requested (24hr cache limit)

	// B Tier (uncommon)
	// server.on("/chrome-variations/seed", [](AsyncWebServerRequest *request) { request->send(200); });  // chrome captive portal call home
	// server.on("/service/update2/json", [](AsyncWebServerRequest *request) { request->send(200); });	   // firefox?
	// server.on("/chat", [](AsyncWebServerRequest *request) { request->send(404); });					   // No stop asking Whatsapp, there is no internet connection
	// server.on("/startpage", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });

	server.onNotFound([](AsyncWebServerRequest *request) {
		request->redirect(localIPURL);
		// Serial.print("onnotfound ");
		// Serial.print(request->host());	// This gives some insight into whatever was being requested on the serial monitor
		// Serial.print(" ");
		// Serial.print(request->url());
		// Serial.println(" sent redirect to " + localIPURL + "\n");
	});

	server.begin();

	int numberOfNetworks = WiFi.scanNetworks();
	if (numberOfNetworks == 0) {
		ESP_LOGI("", "no networks found");
	} else {
		for (int i = 0; i < numberOfNetworks; ++i) {
			// Print SSID and RSSI for each network found
			//  Serial.print(WiFi.SSID(i));
			//  Serial.printf(", %i",WiFi.RSSI(i));
			if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN && WiFi.RSSI(i) > -60) {
				ESP_LOGI("", "Found Open Network");
				WiFi.begin(WiFi.SSID(i).c_str());

			}  // else {
			// 	Serial.println("");
			// }
		}
	}

	WiFi.setTxPower(WIFI_POWER_2dBm);
	ESP_LOGV("WiFi Tx Power Set To:", "%i", (WiFi.getTxPower()));

	ESP_LOGV("", "Startup complete by %ims", (millis()));

	while (true) {
		dnsServer.processNextRequest();		
	}
}

/// @brief takes csv queue and add it to the csv in flash storage
void csvFileManagerTask(void *parameter) {

	//------------- Setup CSV File ----------------
	bool dataFullFlag = false;
	uint8_t retryCount = 0;
	bool err;
	File csvDataFile = LittleFS.open(F(CSVLogFilename), FILE_APPEND, true);	 // open if exists CSVLogFilename, create if it doesn't
	do {
		err = true;
		if (csvDataFile) {					// Can I open the file?
			if (csvDataFile.size() > 25) {	// does it have stuff in it ?
				err = false;
			} else {
				ESP_LOGE("", "%s too small", (CSVLogFilename));
				if (csvDataFile.print("TimeStamp(Mins),CO2(PPM),Humidity(%RH),Temperature(DegC)\r\n")) {	 // Can I add a header?
					ESP_LOGI("", "Setup %s with Header", (CSVLogFilename));
					err = false;
				} else {
					ESP_LOGE("", "Error Printing to %s", (CSVLogFilename));
				}
				csvDataFile.flush();
			}
		} else {
			ESP_LOGE("", "Error Opening %s", (CSVLogFilename));
		}
		retryCount++;
	} while (retryCount < 5 && err == true);

	uint32_t bufferSizeNow = 0;
	csvDataFile.setBufferSize(512);
	uint32_t csvDataFilesize = csvDataFile.size();

	while (true) {

		vTaskSuspend(NULL);
		uint32_t notification = 0;
		xTaskNotifyWait(0, 65535, &notification, 0);
		if (notification > 0) {
			csvDataFile.close();
			LittleFS.remove(F(CSVLogFilename));
			csvDataFile = LittleFS.open(F(CSVLogFilename), FILE_APPEND, true);
			csvDataFile.print("TimeStamp(Mins),CO2(PPM),Humidity(%RH),Temperature(DegC)\r\n");
			csvDataFile.flush();

			dataFullFlag = false;
			bufferSizeNow = 0;
			csvDataFile.setBufferSize(512);
			csvDataFilesize = 0;
			ESP_LOGI("", "%s data cleared", (CSVLogFilename));
		}

		char csvLine[32];
		if (xQueueReceive(charsForCSVFileQueue, &csvLine, 0)) {
			//Serial.println(csvLine);
		
			//----- Append line to CSV File (flush if buffer nearly fully) -----------------
			if (dataFullFlag == false) {

				if (csvDataFilesize < MAX_CSV_SIZE_BYTES) {		// last line of defense for memory leaks

					uint8_t bytesAdded = csvDataFile.println(csvLine);
					if (bytesAdded > 0) {
						bufferSizeNow += bytesAdded;
						//ESP_LOGV("", "added to buffer");

						if (bufferSizeNow > 450) {
							uint32_t writeStart = millis();
							csvDataFile.flush();
							uint32_t writeEnd = millis();
							csvDataFilesize += bufferSizeNow;
							bufferSizeNow = 0;

							ESP_LOGV("", "Wrote data in ~%ums, %s is %ikb", (writeEnd - writeStart), CSVLogFilename, (csvDataFilesize / 1000));
							//Serial.printf("%u,%i\n\r", (writeEnd - writeStart), csvDataFilesize);
						}
					}
					else {
						ESP_LOGE("", "Error Printing to %s", (CSVLogFilename));
					}
				} else {
					ESP_LOGE("", "%s is too large", (CSVLogFilename));
					dataFullFlag = true;
				}
			}
			//-----------------------------------------------
		}
	}
}

/// @brief takes json queue and add it to the json data document in ram
void jsonFileManagerTask(void *parameter) {
	const char *time_format = "%H:%M:%S";
	time_t currentEpoch, prevEpoch = 0;
	uint8_t CO2Index = 0, tempIndex = 0, humidityIndex = 0;
	uint32_t notification;
	double CO2, temperature, humidity;
	double prevCO2 = 0, prevTemperature = 0, prevHumidity = 0;

	initJson();

	while (true) {
		vTaskSuspend(NULL);

		xTaskNotifyWait(0, 65535, &notification, 0);
		if (notification > 0) {
			CO2Index = 0, tempIndex = 0, humidityIndex = 0;
			prevCO2 = 0, prevTemperature = 0, prevHumidity = 0;
		}

		if (uxQueueMessagesWaiting(jsonDataQueue) == 3 ) {
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

/// @brief runs co2 sensor and RTC, puts data in csv and json queues
void sensorManagerTask(void *parameter) {
	PCF8563_Class rtc;
	SCD4X co2;

	const char *time_format = "%y/%m/%d,%H:%M";

	Wire.begin(WIRE_SDA_PIN, WIRE_SCL_PIN, 100000);
#ifdef PRODUCTION_TEST
	co2.isConnected(Wire, &Serial);
#endif
	co2.begin(Wire);
	co2.startPeriodicMeasurement();


	rtc.begin(Wire);
	rtc.syncToSystem();
	setenv("TZ", time_zone, 1);
	tzset();

	double CO2, rawTemperature, temperature = 20.0, rawHumidity, humidity = 0.0;
	double prevCO2 = CO2_MIN, trendCO2 = 0;
	uint16_t position;
	uint32_t dataRequested;

	time_t currentEpoch;
	time_t prevEpoch;
	time(&prevEpoch);
	uint32_t notification;

	bool timeSet = false;

	while (true) {
		vTaskDelay(4700 / portTICK_PERIOD_MS);	// chill while scd40 gets new data
		while (co2.isDataReady() == false) {
			vTaskDelay(10 / portTICK_PERIOD_MS);
		}
		
		if (co2.readMeasurement(CO2, rawTemperature, rawHumidity) == 0) {
			if (prevCO2 == 0.0) {
				prevCO2 = CO2;
			}

			trendCO2 = 0.5 * (CO2 - prevCO2) + (1 - 0.5) * trendCO2;
			position = mapCO2toPosition(CO2 + trendCO2);
			xTaskNotify(lightBar, position, eSetValueWithOverwrite);

			rawTemperature -= TEMP_OFFSET;

			temperature = temperature + (rawTemperature - temperature) * 0.5;
			humidity = humidity + (rawHumidity - humidity) * 0.5;

			xQueueReset(jsonDataQueue);
			xQueueSend(jsonDataQueue, &CO2, 0);
			xQueueSend(jsonDataQueue, &humidity, 0);
			xQueueSend(jsonDataQueue, &temperature, 0);
			vTaskResume(jsonFileManager);

			prevCO2 = CO2;
			//Serial.printf("%4.0f,%2.1f,%1.0f\n", CO2, temperature, humidity);
		}

		time(&currentEpoch);
		if (prevEpoch + CSV_RECORD_INTERVAL_SECONDS <= currentEpoch) {
			prevEpoch += CSV_RECORD_INTERVAL_SECONDS;

			struct tm timeinfo;
			localtime_r(&currentEpoch, &timeinfo);
			char timeStamp[16];
			strftime(timeStamp, 16, time_format, &timeinfo);

			char buf[32]; // temp char array for CSV 40000,99,99
			//CO2(PPM),Humidity(%RH),Temperature(DegC)"
			sprintf(buf, "%s,%4.0f,%2.1f,%2.0f", timeStamp, CO2, humidity, temperature);
			xQueueSend(charsForCSVFileQueue, &buf, 1000 / portTICK_PERIOD_MS);

			vTaskResume(csvFileManager);
		}

		if (timeSet == false && sntp_getreachability(0) + sntp_getreachability(1) + sntp_getreachability(2) > 0) {
			timeSet = true; 
			rtc.syncToRtc();
			WiFi.disconnect();
			time_t epoch;
			struct tm gmt;
			time(&epoch);
			gmtime_r(&epoch, &gmt);
			Serial.println(&gmt, "\n\rGMT Time Set: %A, %B %d %Y %H:%M:%S\n\r");
		}
	}
}

void setup() {
	xTaskCreate(lightBarTask, "lightBar", 4200, NULL, 2, &lightBar);

	Serial.setTxBufferSize(1024);
	Serial.begin(115200);
	while (!Serial)
		;
	Serial.printf("\r\n Kea CO2 \r\n %s compiled on " __DATE__ " at " __TIME__ " \r\n %s%s in the %s environment \r\n\r\n", USER, VERSION, TAG, ENV);

#ifdef PRODUCTION_TEST
	Serial.printf("%s-%d\n\r", ESP.getChipModel(), ESP.getChipRevision());
#endif

	if (LittleFS.begin(true) == false) {	 // Initialize LittleFS (ESP32 Storage) and format on fail
		ESP_LOGE("", "Error mounting LittleFS (Even with Format on Fail)");
	}

	char buf[32];
	charsForCSVFileQueue = xQueueCreate(3, sizeof(buf));
	jsonDataQueue = xQueueCreate(3, sizeof(double));

	jsonDocMutex = xSemaphoreCreateMutex();

	// 		Function, Name (for debugging), Stack size, Params, Priority, Handle
	xTaskCreate(webserverTask, "webserverTask", 17060, NULL, 1, &webserver);
	xTaskCreate(sensorManagerTask, "sensorManagerTask", 3800, NULL, 1, &sensorManager);
	xTaskCreate(csvFileManagerTask, "csvFileManagerTask", 3060, NULL, 0, &csvFileManager);
	xTaskCreate(jsonFileManagerTask, "jsonFileManagerTask", 21000, NULL, 0, &jsonFileManager);
}

void loop() {
	//vTaskDelay(60000 / portTICK_PERIOD_MS);
	vTaskSuspend(NULL);	 // Loop task Not Needed
	// Serial.print(uxTaskGetStackHighWaterMark(lightBar));
	// Serial.print(", ");
	// Serial.print(uxTaskGetStackHighWaterMark(webserver));
	// Serial.print(", ");
	// Serial.print(uxTaskGetStackHighWaterMark(sensorManager));
	// Serial.print(", ");
	// Serial.print(uxTaskGetStackHighWaterMark(csvFileManager));
	// Serial.print(", ");
	// Serial.print(uxTaskGetStackHighWaterMark(jsonFileManager));
	// Serial.print(", ");
	// xSemaphoreTake(jsonDocMutex, 1000 / portTICK_PERIOD_MS);
	// Serial.println(jsonDataDocument.memoryUsage());
	// xSemaphoreGive(jsonDocMutex);
}