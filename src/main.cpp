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
#include <LittleFS.h>

// Addressable LEDs
#include <NeoPixelBusLg.h>	// instead of NeoPixelBus.h (has Luminace and Gamma as default)

// I2C (CO2 & LUX)
#include <Wire.h>

// VEML7700 (LUX)
// #include <DFRobot_VEML7700.h>

// SCD40/41 (CO2)
#include <scd4x.h>

// to store last time data was recorded
#include <Preferences.h>

// I2C Back Up Clock (RTC)
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
#define PIXEL_COUNT 11	   // Number of Addressable Pixels to write data to (starts at pixel 1)
#define PIXEL_DATA_PIN 16  // GPIO -> LEVEL SHIFT -> Pixel 1 Data In Pin


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
//double co2 = 0;						   // Global CO2 Level from SCD40 (updates every 5s)
double temperature = 0;				   // Global temperature
double humidity = 0;				   // Global humidity
#define TEMP_OFFSET 6.4				   // The Enclosure runs a bit hot reduce to get a more accurate ambient

hw_timer_t *timer = NULL;
volatile bool writeDataFlag = false;  // set by timer interrupt and used by addDataToFiles() task to know when to print datapoints to csv
volatile bool clearDataFlag = false;  // set by webserver and used by addDataToFiles() task to know when to clear the csv
volatile bool dataFullFlag = false;

uint8_t globalLedLux = 255;	  // Global 8bit Lux (Max 127lx) (not Locked)
uint16_t globalLedco2 = 450;  // Global CO2 Level (not locked)


QueueHandle_t predictedCO2Queue;
TaskHandle_t lightBar = NULL;

// sets global jsonDataDocument to have y axis names and graph colors but with one data point (x = 0, y = null) per graph data array
void 	createEmptyJson();
// round double variable to 1 decimal place
double roundTo1DP(double value);
// round double variable to 2 decimal places
double roundTo2DP(double value);
// scan all i2c devices and prints results to serial
void i2cScan(TwoWire &i2cBus);

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

#define FRAME_TIME 30	   // Milliseconds between frames 30ms = ~33.3fps maximum
#define SCD4x_TIME 5000	   // Milliseconds between updates

#define OFF RgbColor(0)											   // Black no leds on
#define WARNING_COLOR RgbColor(HsbColor(CO2_MAX_HUE, 1.0f, 1.0f))  // full red

#define PPM_PER_PIXEL ((CO2_MAX - CO2_MIN) / PIXEL_COUNT)  // how many parts per million of co2 each pixel represents

float mapCO2toHue(float inputCO2) {
	return (float)((inputCO2 - (float)CO2_MIN) * ((float)CO2_MAX_HUE - (float)CO2_MIN_HUE) / (float)(CO2_MAX - (float)CO2_MIN) + (float)CO2_MIN_HUE);
}

float mapCO2toPosition(float inputCO2) {
	return (float)((inputCO2 - (float)CO2_MIN) / (float)(CO2_MAX - (float)CO2_MIN));
}

void AddressableRGBLeds(void *parameter) {
	NeoPixelBusLg<NeoGrbFeature, NeoEsp32I2s1Ws2812xMethod> strip(PIXEL_COUNT, PIXEL_DATA_PIN);	 // uses i2s silicon remapped to any pin to drive led data

	uint8_t ledLux = globalLedLux;
	uint8_t currentBrightness = 127;
	uint8_t targetBrightness = 255;
	bool updateLeds = true;

	float predictedCO2 = 0, 	ledCO2 = 0, 	smoothPredictedCO2 = 0;
	uint32_t syncCO2;

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
		// if (currentBrightness != targetBrightness) {
		// 	if (currentBrightness < targetBrightness) {
		// 		currentBrightness++;
		// 		strip.SetLuminance(currentBrightness);	// Luminance is a gamma corrected Brightness
		// 	} else if (currentBrightness > targetBrightness) {
		// 		currentBrightness--;
		// 		strip.SetLuminance(currentBrightness);	// Luminance is a gamma corrected Brightness
		// 	}
		// 	updateLeds = true;	// SetLuminance does NOT affect current pixel data, therefore we must force Redraw of LEDs
		// } else if (ledLux != globalLedLux) {
		// 	ledLux = globalLedLux;

		// 	if (globalLedLux > LED_ON_LUX && targetBrightness < 255) {
		// 		targetBrightness = 255;
		// 	} else if (globalLedLux < LED_OFF_LUX && targetBrightness > 0) {
		// 		targetBrightness = 0;
		// 	}
		// }

		// Convert globalLedco2 to ledco2 and smooth
		// if (globalLedco2 != ledCO2) {
		// 	if (ledCO2 > globalLedco2 && ledCO2 > CO2_MIN) {
		// 		ledCO2--;
		// 	} else {
		// 		ledCO2++;
		// 	}
		// 	updateLeds = true;
		// }
		xTaskNotifyWait(0, 1, &syncCO2, 0);

		if (syncCO2 == true) {
			xQueueReceive(predictedCO2Queue, &predictedCO2, 0);
		}

		smoothPredictedCO2 = smoothPredictedCO2 + (predictedCO2 - smoothPredictedCO2) / (SCD4x_TIME/FRAME_TIME);
		ledCO2 = ledCO2 + (smoothPredictedCO2 - ledCO2) / (SCD4x_TIME / FRAME_TIME);

		//if (outputCO2 < CO2_MAX) {
		float position = mapCO2toPosition(ledCO2);
		uint8_t mixingPixel = (int)(position * PIXEL_COUNT);
		float mixingPixelBrightness = (position * PIXEL_COUNT) - mixingPixel;
		float hue = mapCO2toHue(ledCO2);

		strip.ClearTo(OFF);
		strip.ClearTo(HsbColor(hue, 1.0f, 1.0f), 0, mixingPixel);				 // apply base color to first few pixels
		strip.SetPixelColor(mixingPixel+1, HsbColor(hue, 1.0f, mixingPixelBrightness));  // apply the in-between color mix for the in-between pixel

		strip.Show();									  // push led data to buffer
		//vTaskDelay(FRAME_TIME * 3 / portTICK_PERIOD_MS);  // don't waste cpu time

		// } else {
		// 	strip.SetLuminance(255);

		// 	do {
		// 		strip.ClearTo(OFF);
		// 		strip.Show();
		// 		vTaskDelay(1000 / portTICK_PERIOD_MS);
		// 		strip.ClearTo(WARNING_COLOR);
		// 		strip.Show();
		// 		vTaskDelay(1000 / portTICK_PERIOD_MS);
		// 	} while (co2 > CO2_MAX);
		// 	ledCO2 = co2;  // ledCo2 will have not been updated during CO2 waring Flash
		// 	strip.ClearTo(OFF);
		// 	strip.Show();
		// 	vTaskDelay(1000 / portTICK_PERIOD_MS);
		// }
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

	server.serveStatic("/Kea-CO2-Data.csv", LittleFS, "/Kea-CO2-Data.csv").setCacheControl("no-store");	 // do not cache

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
	writeDataFlag = true;  // set flag for data write during interrupt from timer
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
	while (WiFi.status() != WL_CONNECTED && retryCount <20) {
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

	//const int16_t SCD_ADDRESS = 0x62;
	const char *time_zone = "NZST-12NZDT,M9.5.0,M4.1.0/3";	// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

	Wire.begin(WIRE_SDA_PIN, WIRE_SCL_PIN, 100000);
	rtc.begin(Wire);
	scd4x.begin(Wire);

	setenv("TZ", time_zone, 1);
	tzset();

	rtc.syncToSystem();

	// Wire.beginTransmission(SCD_ADDRESS);
	// Wire.write(0x21);
	// Wire.write(0xb1);
	// Wire.endTransmission();

	// const char *ssid = "Nest";
	// const char *password = "sL&WeEnJs33F";
	// syncWifiTimeToRTC(rtc, ssid, password);
	// setenv("TZ", time_zone, 1);
	// tzset();

	struct tm timeinfo;
	double CO2 = 0, temperature = 0, humidity = 0;
	double prevCO2 = 0, trendCO2 = 0;
	float predictedCO2 = 0;

	while (true) {
		scd4x.isDataReady();
		
		if (scd4x.readMeasurement(CO2, temperature, humidity) == 0) {
			trendCO2 = 0.5 * (CO2 - prevCO2) + (1 - 0.5) * trendCO2;
			predictedCO2 = (float)((CO2) + trendCO2);
			prevCO2 = CO2;

			xQueueOverwrite(predictedCO2Queue, (void *)&predictedCO2);
			xTaskNotify(lightBar, true, eSetValueWithOverwrite);

			getLocalTime(&timeinfo);
			char timeBuf[64];
			size_t written = strftime(timeBuf, 64, "%d/%m/%y,%H:%M:%S", &timeinfo);

			Serial.printf("%s,%4.0f,%2.1f,%1.0f\n", timeBuf, CO2, temperature, humidity);
		}

		// send read data command
		// Wire.beginTransmission(SCD_ADDRESS);
		// Wire.write(0xec);
		// Wire.write(0x05);
		// if (Wire.endTransmission(true) == 0) {
		// 	// read measurement data: 2 bytes co2, 1 byte CRC,
		// 	// 2 bytes T, 1 byte CRC, 2 bytes RH, 1 byte CRC,
		// 	// 2 bytes sensor status, 1 byte CRC
		// 	// stop reading after bytesRequested (12 bytes)

		// 	uint8_t bytesReceived = Wire.requestFrom(SCD_ADDRESS, bytesRequested);
		// 	if (bytesReceived == bytesRequested) {	// If received requested amount of bytes than zero bytes
		// 		uint8_t data[bytesReceived];
		// 		Wire.readBytes(data, bytesReceived);

		// 		// floating point conversion
		// 		CO2 = (double)((uint16_t)data[0] << 8 | data[1]);
		// 		// convert T in degC
		// 		rawTemperature = -45 + 175 * (double)((uint16_t)data[3] << 8 | data[4]) / 65536;
		// 		// convert RH in %
		// 		rawHumidity = 100 * (double)((uint16_t)data[6] << 8 | data[7]) / 65536;

		// 		getLocalTime(&timeinfo);

		// 		trendCO2 = 0.5 * (CO2 - prevCO2) + (1 - 0.5) * trendCO2;
		// 		predictedCO2 = (float)((CO2) + trendCO2);
		// 		prevCO2 = CO2;

		// 		//Serial.println(predictedCO2);
		// 		xQueueOverwrite(predictedCO2Queue, (void *)&predictedCO2);
		// 		xTaskNotify(lightBar, true, eSetValueWithOverwrite);

		// 		char timeBuf[64];
		// 		size_t written = strftime(timeBuf, 64, "%d/%m/%y,%H:%M:%S", &timeinfo);

		// 		//Serial.print(&timeinfo, "%d/%m/%y %H:%M:%S");
		// 		//Serial.printf("%s,%4.0f,%2.1f,%1.0f\n", timeBuf, rawCO2, rawTemperature, rawHumidity);
		// 	} else {
		// 		ESP_LOGE("SCD4x CO2", "I2C bytesReceived(%i) != bytesRequested(%i)", bytesReceived, bytesRequested);
		// 	}
		// } else {
		// 	ESP_LOGE("SCD4x CO2", "I2C Wire.endTransmission");
		// }

		// for (size_t i = 0; i < 50; i++)
		// {
		// 	uint32_t calcStart = millis();
		// 	smoothPredictedCO2 = smoothPredictedCO2 + (predictedCO2 - smoothPredictedCO2) / 25;
		// 	outputCO2 = outputCO2 + (smoothPredictedCO2 - outputCO2) / 25;
		// 	uint32_t calcEnd = millis();
		// 	//ESP_LOGV("", "%f calc in ~%ums", outputCO2, (calcStart - calcEnd));
		// 	// Serial.printf("%4.2f,%4.2f,%4.2f,%4.2f\n", CO2, predictedCO2, smoothPredictedCO2, outputCO2);
		// 	vTaskDelay(100 / portTICK_PERIOD_MS);
		// }
		vTaskDelay(5000 / portTICK_PERIOD_MS);
	}
}

void setup() {
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

	// Wire1.begin(WIRE1_SDA_PIN, WIRE1_SCL_PIN, 100000);
	// Wire.begin(WIRE_SDA_PIN, WIRE_SCL_PIN, 100000);
	//  i2cScan(Wire);
	//  i2cScan(Wire1);

	// Semaphore Mutexes lock resource access to a single task to prevent both cores crashing into each other.
	// veml7700DataSemaphore = xSemaphoreCreateMutex();
	// scd40DataSemaphore = xSemaphoreCreateMutex();
	// i2cBusSemaphore = xSemaphoreCreateMutex();
	predictedCO2Queue = xQueueCreate(1, sizeof(float));

	// 			Function, Name (for debugging), Stack size, Params, Priority, Handle
	// xTaskCreate(LightSensor, "LightSensor", 2000, NULL, 0, NULL);
	// xTaskCreate(CO2Sensor, "CO2Sensor", 3000, NULL, 0, NULL);
	xTaskCreate(sensorsAndData, "sensorsAndData", 5000, NULL, 0, NULL);
	xTaskCreate(AddressableRGBLeds, "AddressableRGBLeds", 2000, NULL, 0, &lightBar);
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