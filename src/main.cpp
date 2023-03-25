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
 * There is also a webserver which displays graphs of CO2, Humidity, Temperature and Lux while also providing a csv download for that data
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

// I2C (Sensors + Clock)
#include <Wire.h>

// LTR303 (Light sensor)
// #include <DFRobot_VEML7700.h>

// SCD4x (CO2 sensor)
#include "scd4x.h"

// to store last time data was recorded
// #include <Preferences.h>

// pcf8563 (Backup Clock)
#include "pcf8563.h"
#include "sntp.h"
#include "time.h"

// -----------------------------------------
//
//    Hardware Settings
//
// -----------------------------------------
#define WIRE_SDA_PIN 21
#define WIRE_SCL_PIN 22
#define WIRE1_SDA_PIN 33
#define WIRE1_SCL_PIN 32

#define PIXEL_DATA_PIN 16  // GPIO -> LEVEL SHIFT -> Pixel 1 Data In Pin
#define PIXEL_COUNT 11	   // Number of Addressable Pixels to write data to (starts at pixel 1)
#define CO2_MAX 2000	   // top of the CO2 LightBar (also when it transitions to warning Flash)
#define CO2_MIN 450		   // bottom of the LightBar
#define FRAME_TIME 30	   // Milliseconds between frames 30ms = ~33.3fps maximum

#define TEMP_OFFSET 6.4	 // The Enclosure runs a bit hot reduce to get a more accurate ambient

// -----------------------------------------
//
//    Config Settings
//
// -----------------------------------------
const char *ssid = "Kea-CO2";  // Name of the Wifi Access Point, FYI The SSID can't have a space in it.
const char *password = "";	   // Password of the Wifi Access Point, leave as "" for no Password

#define DATA_RECORD_INTERVAL_MINS 1

char CSVLogFilename[] = "/Kea-CO2-Data.csv";  // location of the csv file
#define MAX_CSV_SIZE_BYTES 1000000			  // set max size at 1mb

char jsonLogPreview[] = "/data.json";  // name of the file which has the json used for the graphs
#define JSON_DATA_POINTS_MAX 32
#define MIN_JSON_SIZE_BYTES 350	  // tune to empty json size
#define MAX_JSON_SIZE_BYTES 3000  // tune to number of Data Points (do not forget to leave space for large timestamp)

const IPAddress localIP(4, 3, 2, 1);					// the IP address the webserver, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1);					// IP address of the network (should be the same as the local IP in most cases)
const String localIPURL = "http://4.3.2.1/index.html";	// URL to the webserver (Same as the local IP but as a string with http and the landing page subdomain)

// -----------------------------------------
//
//    Global Variables (used doubles over floats due to float errors with Arduino json)
//
// -----------------------------------------
// Allocate the JSON document spot in RAM Use https://arduinojson.org/v6/assistant to compute the size.
DynamicJsonDocument jsonDataDocument(8192);

hw_timer_t *timer = NULL;
TaskHandle_t lightBar = NULL;
TaskHandle_t writeDataToFile = NULL;

// sets global jsonDataDocument to have y axis names and graph colors but with one data point (x = 0, y = null) per graph data array
void createEmptyJson();
// round double variable to 1 decimal place
double roundTo1DP(double value);
// round double variable to 2 decimal places
double roundTo2DP(double value);
// scan all i2c devices and prints results to serial
void i2cScan(TwoWire &i2cBus);

uint16_t mapCO2toPosition(double inputCO2) {
	return (uint16_t)((inputCO2 - CO2_MIN) * (LIGHTBAR_MAX_POSITION) / (CO2_MAX - CO2_MIN));
}

void lightBarTask(void *parameter) {
#define LIGHTBAR_MAX_POSITION PIXEL_COUNT * 255
#define OFF RgbColor(0)

	NeoPixelBus<NeoGrbFeature, NeoEsp32I2s1Ws2812xMethod> strip(PIXEL_COUNT, PIXEL_DATA_PIN);  // uses i2s silicon remapped to any pin to drive led data

	strip.Begin();
	strip.Show();  // init to black

	uint32_t rawPosition = 0;
	uint16_t targetPosition = 0;
	uint16_t outputPosition = 0;

	RgbColor baseColor;
	uint8_t redGreenMix;
	uint16_t mixingPixel;
	uint8_t mixingPixelBrightness;

	bool overMaxPosition = false;

	// Startup Fade IN OUT Green
	// uint8_t i = 0;
	// while (i < 250) {
	// 	strip.ClearTo(RgbColor(0, i, 0));
	// 	strip.Show();
	// 	i += 5;
	// 	vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // vTaskDelay wants ticks, not milliseconds
	// }

	// while (i > 5) {
	// 	strip.ClearTo(RgbColor(0, i, 0));
	// 	strip.Show();
	// 	i -= 5;
	// 	vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // vTaskDelay wants ticks, not milliseconds
	// }

	while (true) {
		xTaskNotifyWait(0, 65535, &rawPosition, 0);

		if (rawPosition > 0) {
			if (rawPosition < LIGHTBAR_MAX_POSITION) {
				targetPosition = (uint16_t)rawPosition;
				overMaxPosition = false;
			} else {
				overMaxPosition = true;
			}
		}

		if (overMaxPosition == false) {
			if (outputPosition != targetPosition) {
				if (outputPosition > targetPosition) {
					outputPosition--;
				} else if (outputPosition < targetPosition) {
					outputPosition += (targetPosition - outputPosition) / 32;
					outputPosition++;
				}

				redGreenMix = outputPosition / PIXEL_COUNT;
				mixingPixel = outputPosition / 255;
				mixingPixelBrightness = outputPosition % 255;

				baseColor = RgbColor(redGreenMix, 255 - redGreenMix, 0);

				strip.ClearTo(OFF);

				strip.SetPixelColor(mixingPixel, baseColor.Dim(mixingPixelBrightness));

				if (mixingPixel > 0) {
					strip.ClearTo(baseColor, 0, mixingPixel - 1);
				}

				strip.Show();
			}
			vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // time between frames
		} else {
			strip.ClearTo(OFF);
			strip.Show();
			vTaskDelay(500 / portTICK_PERIOD_MS);

			strip.ClearTo(RgbColor(255, 0, 0));
			strip.Show();
			vTaskDelay(500 / portTICK_PERIOD_MS);

			outputPosition = LIGHTBAR_MAX_POSITION;
		}
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

	server.serveStatic("/Kea-CO2-Data.csv", LittleFS, "/Kea-CO2-Data.csv").setCacheControl("no-store");	 // do not cache

	server.on("/yesclear.html", HTTP_GET, [](AsyncWebServerRequest *request) {	// when client asks for the json data preview file..
		request->redirect(localIPURL);
		// clearDataFlag = true;
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

	server.serveStatic("/", LittleFS, "/").setCacheControl("max-age=86400");  // serve any file on the device when requested (24hr cache limit)

	// B Tier (uncommon)
	// server.on("/chrome-variations/seed", [](AsyncWebServerRequest *request) { request->send(200); });  // chrome captive portal call home
	// server.on("/service/update2/json", [](AsyncWebServerRequest *request) { request->send(200); });	   // firefox?
	// server.on("/chat", [](AsyncWebServerRequest *request) { request->send(404); });					   // No stop asking Whatsapp, there is no internet connection
	// server.on("/startpage", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });

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

void IRAM_ATTR onTimerInterrupt() {
	xTaskNotify(writeDataToFile, true, eSetValueWithOverwrite);
}
/*
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
	File csvDataFile = LittleFS.open(F(CSVLogFilename), FILE_APPEND, true);	 // open if exists CSVLogFilename, create if it doesn't
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

	while (scd40DataValid == false || veml7700DataValid == false) {	 // wait until valid data is available
		vTaskDelay(100 / portTICK_PERIOD_MS);
	}

	while (true) {
		if (clearDataFlag == true) {
			clearDataFlag = false;
			csvDataFile.close();
			LittleFS.remove(F(CSVLogFilename));
			csvDataFile = LittleFS.open(F(CSVLogFilename), FILE_APPEND, true);
			csvDataFile.print("TimeStamp(Mins),CO2(PPM),Humidity(%RH),Temperature(DegC),Luminance(Lux)\r\n");
			csvDataFile.flush();
			dataFullFlag = false;
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
		if (dataFullFlag == false) {
			char csvLine[32];
			sprintf(csvLine, "%u,%s,%s\r\n", timeStamp, scd4xf, luxf);	// print atleast 3 characters with 2 decimal place

			uint32_t csvDataFilesize = csvDataFile.size();	// must be 32bit (16 bit tops out at 65kb)
			if (csvDataFilesize < MAX_CSV_SIZE_BYTES) {		// last line of defense for memory leaks

				vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);  // increase priority so we get out of the way of the webserver Quickly
				uint32_t writeStart = millis();
				if (csvDataFile.print(csvLine)) {
					csvDataFile.flush();
					uint32_t writeEnd = millis();
					vTaskPrioritySet(NULL, 0);	// reset task priority

					// if ((writeEnd - writeStart)>100) {
					// 	ESP_LOGW("", "Wrote data in ~%ums, %s is %ikb", (writeEnd - writeStart), CSVLogFilename, (csvDataFilesize / 1000));
					// }else
					// {
					// 	ESP_LOGV("", "Wrote data in ~%ums, %s is %ikb", (writeEnd - writeStart), CSVLogFilename, (csvDataFilesize / 1000));
					// }
					Serial.println((writeEnd - writeStart));

				} else {
					vTaskPrioritySet(NULL, 0);	// reset task priority
					ESP_LOGE("", "Error Printing to %s", (CSVLogFilename));
				}
			} else {
				ESP_LOGE("", "%s is too large", (CSVLogFilename));
				dataFullFlag = true;
			}
		}
		//-----------------------------------------------

		jsonIndex = (jsonIndex < JSON_DATA_POINTS_MAX) ? (jsonIndex + 1) : (0);	 // increment from 0 -> JSON_DATA_POINTS_MAX -> 0 -> etc...

		// don't waste cpu time while also accounting for ~1s to add data to files and ~0.5s RTOS inaccuracy
		// vTaskDelay(((1000 * 60 * DATA_RECORD_INTERVAL_MINS) - 1500) / portTICK_PERIOD_MS);

		// while (writeDataFlag == false) {
		// 	vTaskDelay(50 / portTICK_PERIOD_MS);
		// }
		// writeDataFlag = false;	// reset timer interrupt flag

		// vTaskDelay(10 / portTICK_PERIOD_MS);
	}
}
 */
void syncWifiTimeToRTC(PCF8563_Class &rtc, const char *ssid, const char *password) {
	const char *ntpServer1 = "pool.ntp.org";
	const char *ntpServer2 = "time.nist.gov";
	const char *ntpServer3 = "time.google.com";

	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, (char *)ntpServer1);
	sntp_setservername(1, (char *)ntpServer2);
	sntp_setservername(2, (char *)ntpServer3);
	sntp_init();

	// connect to WiFi
	Serial.printf("Connecting to %s ", ssid);
	WiFi.begin(ssid, password);
	uint8_t retryCount = 0;
	while (WiFi.status() != WL_CONNECTED && retryCount < 20) {
		vTaskDelay(100 / portTICK_PERIOD_MS);
		retryCount++;
	}

	if (WiFi.status() == WL_CONNECTED) {
		Serial.println(" CONNECTED");

		struct tm timeinfo;
		time_t now;
		while (getLocalTime(&timeinfo) == false) {
			Serial.println("Waiting for NTP");
			vTaskDelay(950 / portTICK_PERIOD_MS);
		}
		rtc.syncToRtc();
	} else {
		Serial.println("Wifi Connect Failed");
	}
}

void sensorsAndData(void *parameter) {
	PCF8563_Class rtc;
	scd4x scd4x;

	const char *time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";	// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

	Wire.begin(WIRE_SDA_PIN, WIRE_SCL_PIN, 100000);
	scd4x.begin(Wire);
	scd4x.startPeriodicMeasurement();

	rtc.begin(Wire);
	rtc.syncToSystem();
	setenv("TZ", time_zone, 1);
	tzset();

	// const char *ssid = "Nest";
	// const char *password = "sL&WeEnJs33F";
	// syncWifiTimeToRTC(rtc, ssid, password);
	// setenv("TZ", time_zone, 1);
	// tzset();

	struct tm timeinfo;
	double CO2, temperature, humidity;
	double prevCO2 = 1000, trendCO2 = 0;
	uint16_t position;

	vTaskDelay(4700 / portTICK_PERIOD_MS);	// chill while scd40 wakes up and gets a data point

	while (true) {
		while (scd4x.isDataReady() == false) {
			vTaskDelay(10 / portTICK_PERIOD_MS);
		}

		if (scd4x.readMeasurement(CO2, temperature, humidity) == 0) {
			trendCO2 = 0.5 * (CO2 - prevCO2) + (1 - 0.5) * trendCO2;
			position = mapCO2toPosition(CO2 + trendCO2);
			xTaskNotify(lightBar, position, eSetValueWithOverwrite);

			prevCO2 = CO2;
			// Serial.printf("%4.0f,%2.1f,%1.0f\n", CO2, temperature, humidity);
			vTaskDelay(4700 / portTICK_PERIOD_MS);	// chill while scd40 gets new data
		}
	}
}

void setup() {
	xTaskCreate(lightBarTask, "lightBar", 2000, NULL, 0, &lightBar);

	Serial.setTxBufferSize(1024);
	Serial.begin(115200);
	while (!Serial)
		;
	Serial.printf("\r\n Kea CO2 \r\n %s compiled on " __DATE__ " at " __TIME__ " \r\n %s%s in the %s environment \r\n\r\n", USER, VERSION, TAG, ENV);

	if (LittleFS.begin(true)) {	 // Initialize LittleFS (ESP32 Storage) and format on fail
		ESP_LOGV("", "FS init by %ims", millis());
	} else {
		ESP_LOGE("", "Error mounting LittleFS (Even with Format on Fail)");
	}

	// Pin to core 0 so there are no collisions when trying to access files on LittleFS
	// xTaskCreate(apWebserver, "apWebserver", 5000, NULL, 1, NULL);
	// xTaskCreate(addDataToFiles, "addDataToFiles", 3500, NULL, 0, NULL);

	// 			Function, Name (for debugging), Stack size, Params, Priority, Handle
	// xTaskCreate(LightSensor, "LightSensor", 2000, NULL, 0, NULL);
	xTaskCreate(sensorsAndData, "sensorsAndData", 5000, NULL, 0, NULL);
}

void loop() {
	vTaskDelay(100 / portTICK_PERIOD_MS);
	// vTaskSuspend(NULL);	 // Loop task Not Needed
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

void i2cScan(TwoWire &i2cBus) {
	TwoWire *ptr;
	ptr = &i2cBus;

	byte error, address;
	int nDevices = 0;

	for (address = 1; address < 127; address++) {
		// The i2c scanner uses the return value of
		// the Write.endTransmisstion to see if
		// a device did acknowledge to the address.
		ptr->beginTransmission(address);
		error = ptr->endTransmission();
		if (error == 0) {
			Serial.printf("I2C device found at address 0x%02X\n", address);
			nDevices++;
		} else if (error != 2) {
			Serial.printf("Error %d at address 0x%02X\n", error, address);
		}
	}

	if (nDevices == 0) {
		Serial.print("No I2C devices found\n\n");
	}
}