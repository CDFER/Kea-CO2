/*!
 * @file  main.cpp
 * @brief  Kea Studios Open Source Air Quality (CO2) Sensor Firmware
 * @copyright Chris Dirks
 * @license  HIPPOCRATIC LICENSE Version 3.0
 * @author  @CD_FER (Chris Dirks)
 * @created 14/02/2023
 * @url  http://www.keastudios.co.nz
 *
 *
 * ESP32 Wroom Module
 * Lights up a strip of WS2812B Addressable RGB LEDs to display a scale of the ambient CO2 level
 * CO2 data is from a Sensirion SCD40
 * The LEDs are adjusted depending on the ambient Light data from a VEML7700
 * There is also a webserver which displays graphs of CO2, Humidity, Temperature and Lux while also providing a csv download for that data
 *
 * i2c (IO21 -> SDA, IO22 ->SCL) -> SCD40 & VEML7700 (3.3V Power and Data)
 * IO2 (3.3V) -> SN74LVC2T45 Level Shifter (5v) -> WS2812B (5v Power and Data)
 * USB C power (5v Rail) -> XC6220B331MR-G -> 3.3V Rail
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
#include <SPIFFS.h>

// Addressable LEDs
#include <NeoPixelBusLg.h>	// instead of NeoPixelBus.h (has Luminace and Gamma as default)

// I2C (CO2 & LUX)
#include <Wire.h>

// VEML7700 (LUX)
#include <DFRobot_VEML7700.h>

// SCD40/41 (CO2)
#include <SensirionI2CScd4x.h>

// to store last time data was recorded
#include <Preferences.h>

#define DATA_RECORD_INTERVAL_MINS 1

char CSVLogFilename[] = "/Kea-CO2-Data.csv";  // location of the csv file
#define MAX_CSV_SIZE_BYTES 1000000			  // set max size at 1mb

char jsonLogPreview[] = "/data.json";  // name of the file which has the json used for the graphs
#define JSON_DATA_POINTS_MAX 32
#define MIN_JSON_SIZE_BYTES 350	  // tune to empty json size
#define MAX_JSON_SIZE_BYTES 3000  // tune to number of Data Points (do not forget to leave space for large timestamp)
// Allocate the JSON document spot in RAM Use https://arduinojson.org/v6/assistant to compute the size.
DynamicJsonDocument jsonDataDocument(8192);

const char *ssid = "Kea-CO2";  // Name of the Wifi Access Point, FYI The SSID can't have a space in it.
const char *password = "";	   // Password of the Wifi Access Point, leave as "" for no Password

const IPAddress localIP(4, 3, 2, 1);					// the IP address the webserver, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1);					// IP address of the network (should be the same as the local IP in most cases)
const String localIPURL = "http://4.3.2.1/index.html";	// URL to the webserver (Same as the local IP but as a string with http and the landing page subdomain)

// -----------------------------------------
//
//    Global Variables (used doubles over floats due to float errors with Arduino json)
//
// -----------------------------------------
SemaphoreHandle_t i2cBusSemaphore;	// control which task has access to the i2c port

SemaphoreHandle_t veml7700DataSemaphore;  // control which task (core) has access to the global veml7700Data Variables
bool veml7700DataValid = false;			  // Global flag set to true if the VEML7700 variables hold valid data
double Lux;								  // Global Lux

SemaphoreHandle_t scd40DataSemaphore;  // control which task (core) has access to the global scd40 Variables
bool scd40DataValid = false;		   // Global flag true if the SCD40 variables hold valid data
uint16_t co2 = 0;					   // Global CO2 Level from SCD40 (updates every 5s)
double temperature = 0;				   // Global temperature
double humidity = 0;				   // Global humidity
#define TEMP_OFFSET 6.4				   // The Enclosure runs a bit hot reduce to get a more accurate ambient

hw_timer_t *timer = NULL;
volatile bool writeDataFlag = false;  // set by timer interrupt and used by addDataToFiles() task to know when to print datapoints to csv
volatile bool clearDataFlag = false;  // set by webserver and used by addDataToFiles() task to know when to clear the csv

uint8_t globalLedLux = 255;	  // Global 8bit Lux (Max 127lx) (not Locked)
uint16_t globalLedco2 = 450;  // Global CO2 Level (not locked)

// sets global jsonDataDocument to have y axis names and graph colors but with one data point (x = 0, y = null) per graph data array
void createEmptyJson();
// round double variable to 1 decimal place
double roundTo1DP(double value);
// round double variable to 2 decimal places
double roundTo2DP(double value);
// scan all i2c devices and prints results to serial
void i2cScan();
// takes 16bit co2 level and returns the hue float (0.0 - 0.3) using the definitions in Addressable LED Config
float mapCO2ToHue(uint16_t ledCO2);

// -----------------------------------------
//
//    Addressable LED Config
//
// -----------------------------------------
#define CO2_MAX 2000	 // top of the CO2 Scale (also when it transitions to warning Flash)
#define CO2_MAX_HUE 0.0	 // top of the Scale (Red Hue)
#define CO2_MIN 450		 // bottom of the Scale
#define CO2_MIN_HUE 0.3	 // bottom of the Scale (Green Hue)

#define LED_OFF_LUX 250
#define LED_ON_LUX 5

#define PIXEL_COUNT 9	  // Number of Addressable Pixels to write data to (starts at pixel 1)
#define PIXEL_DATA_PIN 2  // GPIO 2 -> LEVEL SHIFT -> Pixel 1 Data In Pin
#define FRAME_TIME 30	  // Milliseconds between frames 30ms = ~33.3fps maximum

#define OFF RgbColor(0)											   // Black no leds on
#define WARNING_COLOR RgbColor(HsbColor(CO2_MAX_HUE, 1.0f, 1.0f))  // full red

#define PPM_PER_PIXEL ((CO2_MAX - CO2_MIN) / PIXEL_COUNT)  // how many parts per million of co2 each pixel represents

void AddressableRGBLeds(void *parameter) {
	NeoPixelBusLg<NeoGrbFeature, NeoEsp32I2s1Ws2812xMethod> strip(PIXEL_COUNT, PIXEL_DATA_PIN);	 // uses i2s silicon remapped to any pin to drive led data

	uint16_t ledCO2 = CO2_MIN;	// internal co2 which has been smoothed
	uint8_t ledLux = globalLedLux;
	uint8_t currentBrightness = 127;
	uint8_t targetBrightness = 255;
	bool updateLeds = true;

	strip.Begin();
	strip.Show();  // init to black

	// Startup Fade IN OUT Green
	uint8_t i = 0;
	while (i < 250) {
		strip.ClearTo(RgbColor(0, i, 0));
		strip.Show();
		i += 5;
		vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // vTaskDelay wants ticks, not milliseconds
	}

	while (i > 5) {
		strip.ClearTo(RgbColor(0, i, 0));
		strip.Show();
		i -= 5;
		vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // vTaskDelay wants ticks, not milliseconds
	}

	while (true) {
		// Convert Lux to Luminance and smooth
		if (currentBrightness != targetBrightness) {
			if (currentBrightness < targetBrightness) {
				currentBrightness++;
				strip.SetLuminance(currentBrightness);	// Luminance is a gamma corrected Brightness
			} else if (currentBrightness > targetBrightness) {
				currentBrightness--;
				strip.SetLuminance(currentBrightness);	// Luminance is a gamma corrected Brightness
			}
			updateLeds = true;	// SetLuminance does NOT affect current pixel data, therefore we must force Redraw of LEDs
		} else if (ledLux != globalLedLux) {
			ledLux = globalLedLux;

			if (globalLedLux > LED_ON_LUX && targetBrightness < 255) {
				targetBrightness = 255;
			} else if (globalLedLux < LED_OFF_LUX && targetBrightness > 0) {
				targetBrightness = 0;
			}
		}

		// Convert globalLedco2 to ledco2 and smooth
		if (globalLedco2 != ledCO2) {
			if (ledCO2 > globalLedco2 && ledCO2 > CO2_MIN) {
				ledCO2--;
			} else {
				ledCO2++;
			}
			updateLeds = true;
		}

		if (ledCO2 < CO2_MAX) {
			if (updateLeds) {  // only update leds if co2 has changed
				updateLeds = false;

				float hue = mapCO2ToHue(ledCO2);

				// Find Which Pixels are filled with base color, in-between and not lit
				uint16_t ppmDrawn = CO2_MIN;  // ppmDrawn is a counter for how many ppm have been displayed by the previous pixels
				uint8_t currentPixel = 1;	  // starts form first pixel )index = 1
				while (ppmDrawn <= (ledCO2 - PPM_PER_PIXEL)) {
					ppmDrawn += PPM_PER_PIXEL;
					currentPixel++;
				}

				strip.ClearTo(HsbColor(hue, 1.0f, 1.0f), 0, currentPixel - 1);												// apply base color to first few pixels
				strip.SetPixelColor(currentPixel, HsbColor(hue, 1.0f, (float(ledCO2 - ppmDrawn) / float(PPM_PER_PIXEL))));	// apply the in-between color mix for the in-between pixel
				strip.ClearTo(OFF, currentPixel + 1, PIXEL_COUNT);															// apply black to the last few leds

				strip.Show();  // push led data to buffer
			}
		} else {
			strip.SetLuminance(255);

			do {
				strip.ClearTo(OFF);
				strip.Show();
				vTaskDelay(1000 / portTICK_PERIOD_MS);
				strip.ClearTo(WARNING_COLOR);
				strip.Show();
				vTaskDelay(1000 / portTICK_PERIOD_MS);
			} while (co2 > CO2_MAX);
			ledCO2 = co2;  // ledCo2 will have not been updated during CO2 waring Flash
			strip.ClearTo(OFF);
			strip.Show();
			vTaskDelay(1000 / portTICK_PERIOD_MS);
		}
		vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // time between frames
	}
}

void apWebserver(void *parameter) {
#define DNS_INTERVAL 30	 // ms between processing dns requests: dnsServer.processNextRequest();

#define MAX_CLIENTS 4
#define WIFI_CHANNEL 6	// 2.4ghz channel 6

	const IPAddress subnetMask(255, 255, 255, 0);

	DNSServer dnsServer;
	AsyncWebServer server(80);

	WiFi.mode(WIFI_AP);
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

	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
		request->redirect(localIPURL);
	});

	server.on("/data.json", HTTP_GET, [](AsyncWebServerRequest *request) {	// when client asks for the json data preview file..
		AsyncResponseStream *response = request->beginResponseStream("application/json");
		response->addHeader("Cache-Control:", "max-age=30");
		serializeJson(jsonDataDocument, *response);	 // turn the json document in ram into a normal json file (as a stream of data)
		request->send(response);
	});

	server.serveStatic("/Kea-CO2-Data.csv", SPIFFS, "/Kea-CO2-Data.csv").setCacheControl("no-store");  // do not cache

	server.serveStatic("/", SPIFFS, "/").setCacheControl("max-age=86400");	// serve any file on the device when requested (24hr cache limit)

	server.on("/yesclear.html", HTTP_GET, [](AsyncWebServerRequest *request) {	// when client asks for the json data preview file..
		request->redirect(localIPURL);
		clearDataFlag = true;
		createEmptyJson();
		ESP_LOGI("", "data clear Requested");
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

	// B Tier (uncommon)
	server.on("/chrome-variations/seed", [](AsyncWebServerRequest *request) { request->send(200); });  // chrome captive portal call home
	server.on("/service/update2/json", [](AsyncWebServerRequest *request) { request->send(200); });	   // firefox?
	server.on("/chat", [](AsyncWebServerRequest *request) { request->send(404); });					   // No stop asking Whatsapp, there is no internet connection
	server.on("/startpage", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });

	server.onNotFound([](AsyncWebServerRequest *request) {
		request->redirect(localIPURL);
		Serial.print("onnotfound ");
		Serial.print(request->host());	// This gives some insight into whatever was being requested on the serial monitor
		Serial.print(" ");
		Serial.print(request->url());
		Serial.println(" sent redirect to " + localIPURL + "\n");
	});

	server.begin();

	WiFi.setTxPower(WIFI_POWER_2dBm);
	ESP_LOGV("WiFi Tx Power Set To:", "%i", (WiFi.getTxPower()));

	ESP_LOGV("", "Startup complete by %ims", (millis()));

	while (true) {
		dnsServer.processNextRequest();
		vTaskDelay(DNS_INTERVAL / portTICK_PERIOD_MS);
	}
}

void LightSensor(void *parameter) {
	DFRobot_VEML7700 VEML7700;

	vTaskDelay(5000 / portTICK_PERIOD_MS);	// make sure co2 goes first

	while (xSemaphoreTake(i2cBusSemaphore, 1) != pdTRUE) {
		vTaskDelay(240 / portTICK_PERIOD_MS);
	};	// infinite loop, check every 1s for access to i2c bus

	VEML7700.begin();
	VEML7700.setGain(VEML7700.ALS_GAIN_x2);
	VEML7700.setIntegrationTime(VEML7700.ALS_INTEGRATION_800ms);
	VEML7700.setPowerSaving(false);
	xSemaphoreGive(i2cBusSemaphore);  // release access to i2c bus

	vTaskDelay(1100 / portTICK_PERIOD_MS);	// give time to settle before reading

	while (true) {
		while (xSemaphoreTake(i2cBusSemaphore, 1) != pdTRUE) {
			vTaskDelay(230 / portTICK_PERIOD_MS);
		};
		float rawLux;
		uint8_t error = VEML7700.getALSLux(rawLux);	 // Get the measured ambient light value
		xSemaphoreGive(i2cBusSemaphore);

		while (xSemaphoreTake(veml7700DataSemaphore, 1) != pdTRUE) {
			vTaskDelay(60 / portTICK_PERIOD_MS);
		};
		veml7700DataValid = false;
		if (!error) {
			if (rawLux < 120000.00f && rawLux >= 0.00f) {
				veml7700DataValid = true;

				Lux = rawLux;
				if (rawLux > 255.0f) {	// overflow protection for 8bit int
					globalLedLux = 255;
				} else {
					globalLedLux = int(rawLux);
				}
			} else {
				ESP_LOGW("VEML7700", "Out of Range");
			}

		} else {
			ESP_LOGW("VEML7700", "getAutoWhiteLux(): ERROR");
		}
		xSemaphoreGive(veml7700DataSemaphore);
		vTaskDelay(10000 / portTICK_PERIOD_MS);	 // only update every 10s
	}
}

void CO2Sensor(void *parameter) {
	SensirionI2CScd4x scd4x;

	uint16_t error;
	uint16_t co2Raw;
	float temperatureRaw;
	float humidityRaw;
	char errorMessage[256];

	while (xSemaphoreTake(i2cBusSemaphore, 1) != pdTRUE) {
		vTaskDelay(100 / portTICK_PERIOD_MS);
	};									   // infinite loop, check every 1s for access to i2c bus
	scd4x.begin(Wire);					   // access i2c bus
	vTaskDelay(100 / portTICK_PERIOD_MS);  // allow time for boot

	error = scd4x.startPeriodicMeasurement();
	if (error) {
		// errorToString(error, errorMessage, 256);
		// ESP_LOGW("SCD4x", "startPeriodicMeasurement(): %s", errorMessage);

		error = scd4x.stopPeriodicMeasurement();
		if (error) {
			errorToString(error, errorMessage, 256);
			ESP_LOGE("SCD4x", "stopPeriodicMeasurement(): %s", errorMessage);
			i2cScan();
		}
		error = scd4x.startPeriodicMeasurement();
		if (error) {
			errorToString(error, errorMessage, 256);
			ESP_LOGE("SCD4x", "startPeriodicMeasurement(): %s", errorMessage);
		}
	}
	xSemaphoreGive(i2cBusSemaphore);

	while (true) {
		error = 0;
		bool isDataReady = false;
		while (xSemaphoreTake(i2cBusSemaphore, 1) != pdTRUE) {
			vTaskDelay(100 / portTICK_PERIOD_MS);
		};	// infinite loop, check every 1s for access to i2c bus
		do {
			error = scd4x.getDataReadyFlag(isDataReady);
			vTaskDelay(30 / portTICK_PERIOD_MS);
		} while (isDataReady == false);

		while (xSemaphoreTake(scd40DataSemaphore, 1) != pdTRUE) {
			vTaskDelay(1000 / portTICK_PERIOD_MS);
		};
		scd40DataValid = false;
		if (error) {
			errorToString(error, errorMessage, 256);
			ESP_LOGE("SCD4x", "getDataReadyFlag(): %s", errorMessage);

		} else {
			error = scd4x.readMeasurement(co2Raw, temperatureRaw, humidityRaw);
			xSemaphoreGive(i2cBusSemaphore);
			if (error) {
				errorToString(error, errorMessage, 256);
				ESP_LOGE("SCD4x", "readMeasurement(): %s", errorMessage);

			} else {
				if ((40000 > co2Raw && co2Raw > 280) &&	 // Baseline pre industrial co2 level (as per paris)
					(100.0f > temperatureRaw && temperatureRaw > (-10.0f)) &&
					(100.0f > humidityRaw && humidityRaw > 0.0f)) {	 // data is sane

					co2 = co2Raw;
					globalLedco2 = co2Raw;
					temperature = (double)temperatureRaw - TEMP_OFFSET;
					humidity = (double)humidityRaw;
					scd40DataValid = true;

				} else {
					ESP_LOGW("SCD4x", "Out of Range co2:%i temp:%f humidity:%f", co2Raw, temperatureRaw, humidityRaw);
				}
			}
		}
		xSemaphoreGive(scd40DataSemaphore);
		vTaskDelay(4750 / portTICK_PERIOD_MS);	// about 5s between readings, don't waste cpu time
	}
}

void IRAM_ATTR onTimerInterrupt() {
	writeDataFlag = true;  // set flag for data write during interrupt from timer
}

void addDataToFiles(void *parameter) {
	timer = timerBegin(0, 80, true);										   // timer_id = 0; divider=80 (80 MHz APB_CLK clock => 1MHz clock); countUp = true;
	timerAttachInterrupt(timer, &onTimerInterrupt, false);					   // edge = false
	timerAlarmWrite(timer, (1000000 * 60 * DATA_RECORD_INTERVAL_MINS), true);  // set time in microseconds
	timerAlarmEnable(timer);
	ESP_LOGV("", "timer init by %ims", millis());

	//----- Import TimeStamp  -----
	Preferences preferences;
	preferences.begin("time", false);							  // open time namespace on flash storage
	uint32_t timeStamp = preferences.getInt("lastTimeStamp", 0);  // uint32_t timeStamp in minutes maxes at ~8000years
	preferences.end();

	//------------- Setup json -----------
	createEmptyJson();
	uint8_t jsonIndex = 0;

	//------------- Setup CSV File ----------------
	uint8_t retryCount = 0;
	bool err;
	File csvDataFile = SPIFFS.open(F(CSVLogFilename), FILE_APPEND, true);  // open if exists CSVLogFilename, create if it doesn't
	do {
		err = true;
		if (csvDataFile) {					// Can I open the file?
			if (csvDataFile.size() > 25) {	// does it have stuff in it ?
				err = false;
			} else {
				ESP_LOGE("", "%s too small", (CSVLogFilename));
				if (csvDataFile.print("TimeStamp(Mins),CO2(PPM),Humidity(%RH),Temperature(DegC),Luminance(Lux)\r\n")) {	 // Can I add a header?
					ESP_LOGI("", "Setup %s with Header", (CSVLogFilename));
					timeStamp = 0;
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

	vTaskDelay(10000 / portTICK_PERIOD_MS);

	while (true) {
		//don't waste cpu time while also accounting for ~1s to add data to files and ~0.5s RTOS inaccuracy
		vTaskDelay(((1000 * 60 * DATA_RECORD_INTERVAL_MINS) - 1500) / portTICK_PERIOD_MS);

		while (writeDataFlag == false) {
			vTaskDelay(50 / portTICK_PERIOD_MS);
		}


		writeDataFlag = false;	// reset timer interrupt flag

		if (clearDataFlag == true) {
			clearDataFlag = false;
			csvDataFile.close();
			SPIFFS.remove(F(CSVLogFilename));
			csvDataFile = SPIFFS.open(F(CSVLogFilename), FILE_APPEND, true);
			csvDataFile.print("TimeStamp(Mins),CO2(PPM),Humidity(%RH),Temperature(DegC),Luminance(Lux)\r\n");
			csvDataFile.flush();
			jsonIndex = 0;
			timeStamp = 0;
			ESP_LOGI("", "%s data cleared", (CSVLogFilename));
		}

		//----- TimeStamp(Mins) -----------------------
		timeStamp += DATA_RECORD_INTERVAL_MINS;
		preferences.begin("time", false);				 // open time namespace on flash storage
		preferences.putInt("lastTimeStamp", timeStamp);	 // save new time stamp to storage
		preferences.end();

		for (size_t i = 0; i < 4; i++) {
			jsonDataDocument[i]["data"][jsonIndex][0] = timeStamp;	// fill x values in json array
		}
		//------------------------------------------------------

		//----- SCD40 Data -------------------------------
		while (xSemaphoreTake(scd40DataSemaphore, 1) != pdTRUE) {
			vTaskDelay(50 / portTICK_PERIOD_MS);
		};
		char scd4xf[18];  // temp char array for CSV 40000,99.9,99.9
		if (scd40DataValid == true) {
			sprintf(scd4xf, "%i,%1.0f,%3.1f", co2, humidity, temperature);	// for temp print atleast 3 characters with 1 decimal place

			jsonDataDocument[0]["data"][jsonIndex][1] = co2;
			jsonDataDocument[1]["data"][jsonIndex][1] = roundTo1DP(humidity);
			jsonDataDocument[2]["data"][jsonIndex][1] = roundTo1DP(temperature);
		} else {
			strcpy(scd4xf, ",,,");	// blank cells in csv
			for (size_t i = 0; i < 3; i++) {
				jsonDataDocument[i]["data"][jsonIndex][1] = (char *)NULL;  // apexCharts treats NULL as missing data
			}
			ESP_LOGW("scd40", "Data Not Valid");
		}
		xSemaphoreGive(scd40DataSemaphore);
		//-----------------------------------------------

		//----- VEML7700 (Lux) Data  --------------------
		while (xSemaphoreTake(veml7700DataSemaphore, 1) != pdTRUE) {
			vTaskDelay(50 / portTICK_PERIOD_MS);
		};
		char luxf[12];	// 119999.99
		if (veml7700DataValid == true) {
			sprintf(luxf, "%3.2f", Lux);  // print atleast 3 characters with 2 decimal place
			jsonDataDocument[3]["data"][jsonIndex][1] = roundTo2DP(Lux);
		} else {
			strcpy(luxf, "");
			jsonDataDocument[3]["data"][jsonIndex][1] = (char *)NULL;
			ESP_LOGW("veml7700", "Data Not Valid");
		}
		xSemaphoreGive(veml7700DataSemaphore);
		//-----------------------------------------------

		//----- Append line to CSV File -----------------
		char csvLine[32];
		sprintf(csvLine, "%u,%s,%s\r\n", timeStamp, scd4xf, luxf);	// print atleast 3 characters with 2 decimal place

		uint32_t csvDataFilesize = csvDataFile.size();	// must be 32bit (16 bit tops out at 65kb)
		if (csvDataFilesize < MAX_CSV_SIZE_BYTES) {  // last line of defense for memory leaks

			vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);  // increase priority so we get out of the way of the webserver Quickly
			uint32_t writeStart = millis();
			if (csvDataFile.print(csvLine)) {
				csvDataFile.flush();
				uint32_t writeEnd = millis();
				vTaskPrioritySet(NULL, 0);	// reset task priority

				if ((writeEnd - writeStart)>30) {
					ESP_LOGW("", "Wrote data in ~%ums, %s is %ikb", (writeEnd - writeStart), CSVLogFilename, (csvDataFilesize / 1000));
				}else
				{
					ESP_LOGV("", "Wrote data in ~%ums, %s is %ikb", (writeEnd - writeStart), CSVLogFilename, (csvDataFilesize / 1000));
				}
				
			} else {
				vTaskPrioritySet(NULL, 0);	// reset task priority
				ESP_LOGE("", "Error Printing to %s", (CSVLogFilename));
			}
		}
		else {
			ESP_LOGE("", "%s is too large", (CSVLogFilename));
		}
		//-----------------------------------------------

		jsonIndex = (jsonIndex < JSON_DATA_POINTS_MAX) ? (jsonIndex + 1) : (0);	 // increment from 0 -> JSON_DATA_POINTS_MAX -> 0 -> etc...
	}
}

void setup() {
	Serial.setTxBufferSize(1024);
	Serial.begin(115200);
	while (!Serial)
		;
	Serial.printf("\r\n Kea CO2 \r\n %s compiled on " __DATE__ " at " __TIME__ " \r\n %s%s in the %s environment \r\n\r\n", USER, VERSION, TAG, ENV);

	if (SPIFFS.begin(true)) {  // Initialize SPIFFS (ESP32 SPI Flash Storage) and format on fail
		ESP_LOGV("", "FS init by %ims", millis());
	} else {
		ESP_LOGE("", "Error mounting SPIFFS (Even with Format on Fail)");
	}

	// Pin to core 0 so there are no collisions when trying to access files on SPIFFS
	xTaskCreatePinnedToCore(apWebserver, "apWebserver", 5000, NULL, 1, NULL, 0);
	xTaskCreatePinnedToCore(addDataToFiles, "addDataToFiles", 3500, NULL, 1, NULL, 0);

	if (Wire.begin(21, 22, 10000)) {  // Initialize I2C Bus (CO2 and Light Sensor)
		ESP_LOGV("I2C", "Initialized Correctly by %ims", millis());
	} else {
		ESP_LOGE("I2C", "Can't begin I2C Bus");
	}

	// Semaphore Mutexes lock resource access to a single task to prevent both cores crashing into each other.
	veml7700DataSemaphore = xSemaphoreCreateMutex();
	scd40DataSemaphore = xSemaphoreCreateMutex();
	i2cBusSemaphore = xSemaphoreCreateMutex();

	// 			Function, Name (for debugging), Stack size, Params, Priority, Handle
	xTaskCreatePinnedToCore(LightSensor, "LightSensor", 2000, NULL, 0, NULL, 1);
	xTaskCreatePinnedToCore(CO2Sensor, "CO2Sensor", 3000, NULL, 0, NULL, 1);
	xTaskCreatePinnedToCore(AddressableRGBLeds, "AddressableRGBLeds", 2000, NULL, 0, NULL, 1);
}

void loop() {
	vTaskSuspend(NULL);	 // Loop task Not Needed
}

void createEmptyJson() {
	StaticJsonDocument<768> doc;

	JsonObject doc_0 = doc.createNestedObject();
	doc_0["name"] = "CO2";

	JsonArray doc_0_data_0 = doc_0["data"].createNestedArray();
	doc_0_data_0.add(0);
	doc_0_data_0.add(nullptr);
	doc_0["color"] = "#70AE6E";
	doc_0["y_title"] = "CO2 Parts Per Million (PPM)";

	JsonObject doc_1 = doc.createNestedObject();
	doc_1["name"] = "Humidity";

	JsonArray doc_1_data_0 = doc_1["data"].createNestedArray();
	doc_1_data_0.add(0);
	doc_1_data_0.add(nullptr);
	doc_1["color"] = "#333745";
	doc_1["y_title"] = "Relative humidity (%RH)";

	JsonObject doc_2 = doc.createNestedObject();
	doc_2["name"] = "Temperature";

	JsonArray doc_2_data_0 = doc_2["data"].createNestedArray();
	doc_2_data_0.add(0);
	doc_2_data_0.add(nullptr);
	doc_2["color"] = "#FE5F55";
	doc_2["y_title"] = "Temperature (Deg C)";

	JsonObject doc_3 = doc.createNestedObject();
	doc_3["name"] = "Light Level";

	JsonArray doc_3_data_0 = doc_3["data"].createNestedArray();
	doc_3_data_0.add(0);
	doc_3_data_0.add(nullptr);
	doc_3["color"] = "#8B6220";
	doc_3["y_title"] = "Lux (lm/m^2)";

	jsonDataDocument = doc;
}

double roundTo2DP(double value) {  // rounds a number to 2 decimal places
	return (int)(value * 100 + 0.5) / 100.0;
}

double roundTo1DP(double value) {  // rounds a number to 1 decimal place
	return (int)(value * 10 + 0.5) / 10.0;
}

void i2cScan() {
	byte error, address;
	int nDevices;
	nDevices = 0;
	for (address = 1; address < 127; address++) {
		Wire.beginTransmission(address);
		error = Wire.endTransmission();
		if (error == 0) {
			Serial.print("I2C device found at address 0x");
			if (address < 16) {
				Serial.print("0");
			}
			Serial.println(address, HEX);
			nDevices++;
		} else if (error == 4) {
			Serial.print("Error at address 0x");
			if (address < 16) {
				Serial.print("0");
			}
			Serial.println(address, HEX);
		}
	}
	if (nDevices == 0) {
		ESP_LOGE("", "No I2C devices found on Scan");
	}
}

float mapCO2ToHue(uint16_t ledCO2) {
	return (float)(ledCO2 - (uint16_t)CO2_MIN) * ((float)CO2_MAX_HUE - (float)CO2_MIN_HUE) / (float)((uint16_t)CO2_MAX - (uint16_t)CO2_MIN) + (float)CO2_MIN_HUE;
}
