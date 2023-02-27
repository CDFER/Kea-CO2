/*!
 * @file  main.cpp
 * @brief  Kea Studios Open Source Air Quality (CO2) Sensor Firmware
 * @copyright Chris Dirks (http://www.keastudios.co.nz)
 * @license  HIPPOCRATIC LICENSE Version 3.0
 * @author  @CD_FER (Chris Dirks)
 * @created 14/02/2023
 * @url  https://github.com/DFRobot/DFRobot_VEML7700
 *
 * ESP32 Wroom Module
 * Lights up a strip of WS2812B Addressable RGB LEDs to display a scale of the ambient CO2 level
 * CO2 data is from a Sensirion SCD40
 * The LEDs are adjusted depending on the ambient Light data from a VEML7700=
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

#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"

// Onboard Flash Storage
#include <SPIFFS.h>

// Addressable LEDs
#include <NeoPixelBusLg.h>	// instead of NeoPixelBus.h (has Luminace and Gamma as default)

//I2C (CO2 & LUX)
#include <Wire.h>

//VEML7700 (LUX)
#include <DFRobot_VEML7700.h>

//SCD40/41 (CO2)
#include <SensirionI2CScd4x.h>

//to store last time data was recorded
#include <Preferences.h>

#define VERSION "V0.1.0-DEV"
#define USER "CD_FER" //username to add to the startup serial output

#define DATA_RECORD_INTERVAL_MINS 1

char CSVLogFilename[] = "/Kea-CO2-Data.csv";  // location of the csv file
#define MAX_CSV_SIZE_BYTES 1000000 //set max size at 1mb

char jsonLogPreview[] = "/data.json";  // name of the file which has the json used for the graphs
#define JSON_DATA_POINTS_MAX 32
#define MIN_JSON_SIZE_BYTES 359 //tune to empty json size
#define MAX_JSON_SIZE_BYTES 3000 //tune to number of Data Points

const char *ssid = "Kea-CO2";	// Name of the Wifi Access Point, FYI The SSID can't have a space in it.
const char *password = "";		// Password of the Wifi Access Point, leave blank for no Password

const IPAddress localIP(4, 3, 2, 1);					// the IP address the webserver, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1);					// IP address of the network (should be the same as the local IP in most cases)
const String localIPURL = "http://4.3.2.1/index.html";	// URL to the webserver (Same as the local IP but as a string with http and the landing page subdomain)


// -----------------------------------------
//
//    Global Variables
//
// -----------------------------------------
SemaphoreHandle_t globalVariablesSemaphore;	 // control which task (core) has access to the global variables
uint8_t lux = 255;			// Global 8bit Lux (Max 255lx) Light Value from VEML7700 (updates every 1s)
float rawLux = -1.0f;		// Global floating Lux Light Value from VEML7700 (updates every 1s)
uint16_t co2 = 0;	 		// Global CO2 Level from SCD40 (updates every 5s)
float temperature = -100.0f;  	// Global temperature from SCD40 (updates every 5s)
float humidity = -1.0f;	   	// Global humidity Level from SCD40 (updates every 5s)

bool writeDataFlag = false;

// Allocate the JSON document spot in RAM
// Use https://arduinojson.org/v6/assistant to compute the capacity.
DynamicJsonDocument jsonDataDocument(10000);

SemaphoreHandle_t i2cBusSemaphore; //control which task (core) has access to the i2c port

// -----------------------------------------
//
//    Addressable LED Config
//
// -----------------------------------------
#define CO2_MAX 2000 	//top of the CO2 Scale (also when it transitions to warning Flash)
#define CO2_MAX_HUE 0.0	//top of the Scale (Red Hue) 
#define CO2_MIN 450		//bottom of the Scale
#define CO2_MIN_HUE 0.3	//bottom of the Scale (Green Hue)
#define CO2_SMOOTHING_FACTOR 100  // helps to make it look like we have continuous data not a update every 5s

#define LUX_SMOOTHING_FACTOR 10	 // helps to make it look like we have continuous data not a update every 5s
#define MIN_USABLE_LUMINANCE 48  // min Luminance when led's actually turn on

#define PIXEL_COUNT 9
#define PIXEL_DATA_PIN 2
#define FRAME_TIME 30  // Milliseconds between frames 30ms = ~33.3fps max

#define OFF RgbColor(0)
#define WARNING_COLOR RgbColor(HsbColor(CO2_MAX_HUE, 1.0f, 1.0f))

#define PPM_PER_PIXEL ((CO2_MAX - CO2_MIN) / PIXEL_COUNT)

float mapCO2ToHue(uint16_t ledCO2){
	return (float)(ledCO2 - (uint16_t)CO2_MIN) * ((float)CO2_MAX_HUE - (float)CO2_MIN_HUE) / (float)((uint16_t)CO2_MAX - (uint16_t)CO2_MIN) + (float) CO2_MIN_HUE;
}

void AddressableRGBLeds(void *parameter) {
	NeoPixelBusLg<NeoGrbFeature, NeoEsp32I2s1Ws2812xMethod> strip(PIXEL_COUNT, PIXEL_DATA_PIN);	 // uses i2s silicon remapped to any pin to drive led data

	uint16_t ppmDrawn = 0;
	float hue = CO2_MIN_HUE;
	uint8_t brightness = 255;
	uint16_t ledCO2 = CO2_MIN;  // internal co2 which has been smoothed
	uint8_t ledLux = 255;
	bool updateLeds = true;

	strip.Begin();
	strip.Show();
	ESP_LOGV("LED Strip", "STARTED");

	//Startup Fade IN OUT Green
	for (uint8_t i = 0; i < 255; i++) {
		strip.ClearTo(RgbColor(0,i,0));
		strip.Show();
		vTaskDelay((4500 / 255 / 2) / portTICK_PERIOD_MS);	 // vTaskDelay wants ticks, not milliseconds
	}
	for (uint8_t i = 255; i > 0; i--) {
		strip.ClearTo(RgbColor(0, i, 0));
		strip.Show();
		vTaskDelay((4500 / 255 / 2) / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
	}


	while (true) {
		// Convert Lux to Luminance and smooth
		if (ledLux != lux){
			if (ledLux >lux){
				ledLux--;
			}else{
				ledLux++;
			}
		
			if (ledLux < MIN_USABLE_LUMINANCE && ledLux > 0) {
				strip.SetLuminance(MIN_USABLE_LUMINANCE);
			} else {
				strip.SetLuminance(ledLux);	 // Luminance is a gamma corrected Brightness
			}
			updateLeds = true;	// SetLuminance does NOT affect current pixel data, therefore we must force Redraw of LEDs
		}


		if (co2 > CO2_MIN && co2 < CO2_MAX ) {
			if (ledCO2 != co2 || updateLeds) {	// only update leds if co2 has changed
				// ledCO2 = ((ledCO2 * (CO2_SMOOTHING_FACTOR - 1)) + co2) / CO2_SMOOTHING_FACTOR; //smooth

				updateLeds = false;

				if (ledCO2 > co2) {
					ledCO2--;
				} else {
					ledCO2++;
				}

				hue = mapCO2ToHue(ledCO2);

				// Find Which Pixels are filled with base color, in-between and not lit
				ppmDrawn = CO2_MIN;	 // ppmDrawn is a counter for how many ppm have been displayed by the previous pixels
				uint8_t currentPixel = 1; //starts form first pixel )index = 1
				while (ppmDrawn <= (ledCO2 - PPM_PER_PIXEL)) {
					ppmDrawn += PPM_PER_PIXEL;
					currentPixel++;
				}

				strip.ClearTo(HsbColor(hue, 1.0f, 1.0f), 0, currentPixel - 1);												// apply base color to first few pixels
				strip.SetPixelColor(currentPixel, HsbColor(hue, 1.0f, (float(ledCO2 - ppmDrawn) / float(PPM_PER_PIXEL))));	// apply the in-between color mix for the in-between pixel
				strip.ClearTo(OFF, currentPixel + 1, PIXEL_COUNT);															// apply black to the last few leds

				strip.Show();  // push led data to buffer
			}
		}else{
			if (co2 > CO2_MAX){
				brightness = 255;
				strip.SetLuminance(255);

				while (co2 > CO2_MAX) {
					strip.ClearTo(OFF);
					strip.Show();
					vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
					strip.ClearTo(WARNING_COLOR);
					strip.Show();
					vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
				}
				ledCO2 = co2;  // ledCo2 will have not been updated during CO2 waring Flash
				strip.ClearTo(OFF);
				strip.Show();
				vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
				
			}
		}
		vTaskDelay(FRAME_TIME / portTICK_PERIOD_MS);  // time between frames
	}
}

void accessPoint(void *parameter) {
#define DNS_INTERVAL 10	 // ms between processing dns requests: dnsServer.processNextRequest();

#define MAX_CLIENTS 4
#define WIFI_CHANNEL 6	// 2.4ghz channel 6

	const IPAddress subnetMask(255, 255, 255, 0);

	DNSServer dnsServer;
	AsyncWebServer server(80);

	WiFi.mode(WIFI_AP);
	WiFi.softAPConfig(localIP, gatewayIP, subnetMask);	// Samsung requires the IP to be in public space
	WiFi.softAP(ssid, password, WIFI_CHANNEL, 0, MAX_CLIENTS);
	//WiFi.setSleep(false);

	dnsServer.setTTL(300);				// set 5min client side cache for DNS
	dnsServer.start(53, "*", localIP);	// if DNSServer is started with "*" for domain name, it will reply with provided IP to all DNS request

	// ampdu_rx_disable android workaround see https://github.com/espressif/arduino-esp32/issues/4423
	esp_wifi_stop();
	esp_wifi_deinit();

	wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();	// We use the default config ...
	my_config.ampdu_rx_enable = false;							//... and modify only what we want.

	esp_wifi_init(&my_config);	// set the new config
	esp_wifi_start();			// Restart WiFi
	vTaskDelay(100 / portTICK_PERIOD_MS);  // this is necessary don't ask me why

	ESP_LOGV("AccessPoint", "Startup complete by %ims", (millis()));

	//======================== Webserver ========================
	// WARNING IOS (and maybe macos) WILL NOT POP UP IF IT CONTAINS THE WORD "Success" https://www.esp8266.com/viewtopic.php?f=34&t=4398
	// SAFARI (IOS) IS STUPID, G-ZIPPED FILES CAN'T END IN .GZ https://github.com/homieiot/homie-esp8266/issues/476 this is fixed by the webserver serve static function.
	// SAFARI (IOS) there is a 128KB limit to the size of the HTML. The HTML can reference external resources/images that bring the total over 128KB
	// SAFARI (IOS) popup browser has some severe limitations (javascript disabled, cookies disabled)

	server.serveStatic("/data.json", SPIFFS, "/data.json").setCacheControl("no-store");						   // fetch data file every request
	server.serveStatic("/index.html", SPIFFS, "/index.html").setCacheControl("max-age=120");					   // serve html file
	server.serveStatic("/", SPIFFS, "/").setCacheControl("max-age=120");			   // serve any file on the device when requested

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
	server.on("/chrome-variations/seed",[](AsyncWebServerRequest *request){request->send(200);}); //chrome captive portal call home
	server.on("/service/update2/json",[](AsyncWebServerRequest *request){request->send(200);}); //firefox?
	server.on("/chat",[](AsyncWebServerRequest *request){request->send(404);}); //No stop asking Whatsapp, there is no internet connection
	server.on("/startpage",[](AsyncWebServerRequest *request){request->redirect(localIPURL);});

	server.onNotFound([](AsyncWebServerRequest *request) {
		request->redirect(localIPURL);
		ESP_LOGW("WebServer", "Page not found sent redirect to localIPURL");
		Serial.print("onnotfound ");
		Serial.print(request->host());	// This gives some insight into whatever was being requested on the serial monitor
		Serial.print(" ");
		Serial.print(request->url());
		Serial.println(" sent redirect to " + localIPURL + "\n");
	});

	server.begin();

	ESP_LOGV("WebServer", "Startup complete by %ims",(millis()));

	while (true) {
		dnsServer.processNextRequest();
		vTaskDelay(DNS_INTERVAL / portTICK_PERIOD_MS);
	}
}

void LightSensor(void *parameter) {
	DFRobot_VEML7700 VEML7700;
	uint8_t error = 0;
	vTaskDelay(5000 / portTICK_PERIOD_MS);	// allow time for boot

	xSemaphoreTake(i2cBusSemaphore, 1000 / portTICK_PERIOD_MS);
	VEML7700.begin(); //comment out wire.begin() in this function
	xSemaphoreGive(i2cBusSemaphore);

	while (true) {
		xSemaphoreTake(i2cBusSemaphore, 250 / portTICK_PERIOD_MS);
		error = VEML7700.getWhiteLux(rawLux);  // Get the measured ambient light value
		xSemaphoreGive(i2cBusSemaphore);

		if (!error) {
			if (rawLux > 255.0f) {  // overflow protection for 8bit int
				lux = 255;
			} else {
				lux = int(rawLux);
			}
		}else{
			ESP_LOGW("VEML7700", "getAutoWhiteLux(): STATUS_ERROR");
		}

		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}

void CO2Sensor(void *parameter) {
	SensirionI2CScd4x scd4x;

	uint16_t error;
	uint16_t co2Raw = 0;
	float temperatureRaw = 0.0f;
	float humidityRaw = 0.0f;
	char errorMessage[256];
	bool isDataReady = false;

	xSemaphoreTake(i2cBusSemaphore, 1000 / portTICK_PERIOD_MS);

	scd4x.begin(Wire);
	error = scd4x.startPeriodicMeasurement();
	if (error) {
		errorToString(error, errorMessage, 256);
		ESP_LOGD("SCD4x", "startPeriodicMeasurement(): %s", errorMessage);

		error = scd4x.stopPeriodicMeasurement();
		if (error) {
			Serial.print("Error trying to execute stopPeriodicMeasurement(): ");
			errorToString(error, errorMessage, 256);
			Serial.println(errorMessage);
		}

		uint16_t serial0;
		uint16_t serial1;
		uint16_t serial2;
		error = scd4x.getSerialNumber(serial0, serial1, serial2);
		if (error) {
			Serial.print("Error trying to execute getSerialNumber(): ");
			errorToString(error, errorMessage, 256);
			Serial.println(errorMessage);
		} else {
			printf("%i %i %i\n",serial0, serial1, serial2);
		}

		// uint16_t sensorStatus;
		// error = scd4x.performSelfTest(sensorStatus);
		// if (error) {
		// 	Serial.print("Error trying to execute performSelfTest(): ");
		// 	errorToString(error, errorMessage, 256);
		// 	Serial.println(errorMessage);
		// } else {
		// 	errorToString(sensorStatus, errorMessage, 256);
		// 	Serial.println(errorMessage);
		// 	//printf("sensorStatus = %i\n", sensorStatus);
		// }

		byte error, address;
		int nDevices;
		Serial.println("Scanning...");
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
				Serial.print("Unknow error at address 0x");
				if (address < 16) {
					Serial.print("0");
				}
				Serial.println(address, HEX);
			}
		}
		if (nDevices == 0) {
			Serial.println("No I2C devices found\n");
		} else {
			Serial.println("done\n");
		}

		error = scd4x.reinit();
		if (error) {
			Serial.print("Error trying to execute performFactoryReset(): ");
			errorToString(error, errorMessage, 256);
			Serial.println(errorMessage);
		}

		// Start Measurement
		error = scd4x.startPeriodicMeasurement();
		if (error) {
			Serial.print("Error trying to execute startPeriodicMeasurement(): ");
			errorToString(error, errorMessage, 256);
			Serial.println(errorMessage);
		}

		Serial.println("");
	}

	xSemaphoreGive(i2cBusSemaphore);

	//Serial.print("\nCO2 (ppm),Temp (degC),Humidity (%RH)\n");

	while (true) {

		error = 0;
		isDataReady = false;
		xSemaphoreTake(i2cBusSemaphore, 1000 / portTICK_PERIOD_MS);
		do{
			error = scd4x.getDataReadyFlag(isDataReady);
			vTaskDelay(30 / portTICK_PERIOD_MS);	// about 5s between readings
		} while (isDataReady == false);


		if (error) {
			errorToString(error, errorMessage, 256);
			ESP_LOGW("SCD4x", "getDataReadyFlag(): %s", errorMessage);
			
		}else{
			error = scd4x.readMeasurement(co2Raw, temperatureRaw, humidityRaw);
			if (error) {
				errorToString(error, errorMessage, 256);
				ESP_LOGW("SCD4x", "readMeasurement(): %s", errorMessage);

			} else {
				//Serial.printf("%i,%.1f,%.1f\n", co2Raw, temperatureRaw, humidityRaw);

				// pass back to global values
				co2 = co2Raw;
				temperature = temperatureRaw;
				humidity = humidityRaw;
			}
		}
		xSemaphoreGive(i2cBusSemaphore);
		vTaskDelay(4750 / portTICK_PERIOD_MS);  //about 5s between readings, don't waste cpu time
	}
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

	File file = SPIFFS.open(F(jsonLogPreview), FILE_WRITE, true);
	if (!file) {
		ESP_LOGE("Files", "Error Creating %s", (jsonLogPreview));
	} else {
		serializeJson(doc, file);
		file.close();
		ESP_LOGW("", "Setup %s", (jsonLogPreview));
	}
}

void ARDUINO_ISR_ATTR onTimer() {
	writeDataFlag = true;  // set flag for data write
}

void addDataToFiles(void *parameter) {
	hw_timer_t *timer = NULL;

	// Use 1st timer of 4 (counted from zero).
	// Set 80 divider for prescaler (see ESP32 Technical Reference Manual for more info).
	timer = timerBegin(0, 80, true);

	// Attach onTimer function to our timer.
	timerAttachInterrupt(timer, &onTimer, true);

	// Set alarm to call onTimer function every second (value in microseconds).
	// Repeat the alarm (third parameter)
	timerAlarmWrite(timer, 1000000 * 60 *DATA_RECORD_INTERVAL_MINS, true);

	// Start an alarm
	timerAlarmEnable(timer);


	//----- Import TimeStamp and Json Index -----
	Preferences preferences;
	preferences.begin("time", false);							  // open time namespace on flash storage
	uint32_t timeStamp = preferences.getInt("lastTimeStamp", 0);  // uint32_t timeStamp in minutes maxes at ~8000years
	//ESP_LOGV("TimeStamp:", "%i mins", timeStamp);
	uint8_t jsonIndex = preferences.getInt("jsonIndex", 0);
	//ESP_LOGV("jsonIndex:", "%i", jsonIndex);
	preferences.end();

	//------------- Setup CSV File ----------------
	uint8_t i = 0;
	bool err;
	//File csvDataFile; //
	do {
		err = true;
		File csvDataFile = SPIFFS.open(F(CSVLogFilename), FILE_APPEND, true);  // open if exists CSVLogFilename, create if it doesn't
		if (csvDataFile) { //Can I open the file?
			if (csvDataFile.size() > 25) {  // does it have stuff in it ?
				err = false;
				ESP_LOGI("", "%s is %ikb", (CSVLogFilename), (csvDataFile.size() / 1000));
			} else {
				if (csvDataFile.print("TimeStamp(Mins),CO2(PPM),Humidity(%RH),Temperature(DegC),Luminance(Lux)\r\n")) {//Can I add a header?
					ESP_LOGW("", "Setup %s", (CSVLogFilename));
				} else {
					ESP_LOGE("", "Error Printing to %s", (CSVLogFilename));
				}
			}
		}else{
			ESP_LOGE("", "Error Opening %s", (CSVLogFilename));
		}
		i++;
		csvDataFile.close();
	} while (i < 5 && err == true);

	//------------- Setup json File ----------------
	i = 0;
	//File jsonDataFile;
	do {
		err = true;
		File jsonDataFile = SPIFFS.open(F(jsonLogPreview), FILE_READ, true);	 // open if exists CSVLogFilename, create if it doesn't
		if (jsonDataFile) {						 // Can I open the file?
			if (jsonDataFile.size() > MIN_JSON_SIZE_BYTES && jsonDataFile.size() < MAX_JSON_SIZE_BYTES) {  // does it have stuff in it ?
				DeserializationError error = deserializeJson(jsonDataDocument, jsonDataFile);
				if (!error) {					 // Can I successfully deserialize it?
					err = false;  
					ESP_LOGI("", "%s is %ib", (jsonLogPreview), (jsonDataFile.size()));
					jsonDataFile.close();
				} else {
					ESP_LOGE("", "Error deserializing %s", (jsonLogPreview));
					jsonDataFile.close();
				}
			} else {
				ESP_LOGE("", "%s is %ibytes", (jsonLogPreview), (jsonDataFile.size()));
				jsonDataFile.close();
				jsonIndex = 0;
				createEmptyJson();
			}
		} else {
			ESP_LOGE("", "Error Opening %s", (jsonLogPreview));
		}
		i++;
	} while (i < 5 && err == true);

	while (true) {
		while (writeDataFlag == false) {
			vTaskDelay(50 / portTICK_PERIOD_MS);
		}

		writeDataFlag = false;

		ESP_LOGV("", "WritingData");

		if (jsonIndex < JSON_DATA_POINTS_MAX) {
			jsonIndex++;
		} else {
			jsonIndex = 0;
		}

		timeStamp += DATA_RECORD_INTERVAL_MINS;

		preferences.begin("time", false);  // open time namespace on flash storage
		preferences.putInt("jsonIndex", jsonIndex);
		preferences.putInt("lastTimeStamp", timeStamp);	 // save new time stamp to storage
		preferences.end();

		//----- TimeStamp (Mins) -----------------------
		char timef[11];
		sprintf(timef, "%i", timeStamp);

		for (size_t i = 0; i < 4; i++) {
			jsonDataDocument[i]["data"][jsonIndex][0] = timeStamp;	// fill x values in json array
		}
		//------------------------------------------------------

		xSemaphoreTake(globalVariablesSemaphore, 1000 / portTICK_PERIOD_MS);

		//----- CO2 (PPM) -------------------------------
		char co2f[6];
		if (co2 < 400) {
			strcpy(co2f, "");										   // blank cell in csv
			jsonDataDocument[0]["data"][jsonIndex][1] = (char *)NULL;  // apexCharts treats NULL as missing data
			ESP_LOGW("co2 Value", "co2 < 400 ");
		} else {
			sprintf(co2f, "%i", co2);
			jsonDataDocument[0]["data"][jsonIndex][1] = co2;
		}
		//-----------------------------------------------

		//----- Humidity (%RH) --------------------------
		char humidityf[3];
		if (humidity < 0.01f || humidity > 100.0f) {
			strcpy(humidityf, "");
			jsonDataDocument[1]["data"][jsonIndex][1] = (char *)NULL;
			ESP_LOGW("humidity Value", "humidity < 0 or humidity > 100");
		} else {
			dtostrf(humidity, -2, 0, humidityf);  // 2 characters with 0 decimal place and left aligned (negative width)
			jsonDataDocument[1]["data"][jsonIndex][1] = humidity;
		}
		//-----------------------------------------------

		//----- Temperature (degC) ----------------------
		char temperaturef[5];
		if (temperature < -10.0f || temperature > 60.0f) {
			strcpy(temperaturef, "");
			jsonDataDocument[2]["data"][jsonIndex][1] = (char *)NULL;
			ESP_LOGW("temperature Value", "temperature < -10 or temperature > 60");
		} else {
			dtostrf(temperature, -4, 1, temperaturef);	// 4 characters with 1 decimal place and left aligned (negative width)
			jsonDataDocument[2]["data"][jsonIndex][1] = temperature;
		}
		//-----------------------------------------------

		//----- Lux  ------------------------------------
		char luxf[8];
		if (rawLux < 0.1f || rawLux > 99999.0f) {
			strcpy(luxf, "");
			jsonDataDocument[3]["data"][jsonIndex][1] = (char *)NULL;
			ESP_LOGW("light level Value", "lux < 0 or lux > 99,999");
		} else {
			dtostrf(rawLux, -7, 1, luxf);  // 7 characters with 1 decimal place and left aligned (negative width)
			jsonDataDocument[3]["data"][jsonIndex][1] = rawLux;
		}
		//-----------------------------------------------

		xSemaphoreGive(globalVariablesSemaphore);

		File csvDataFile = SPIFFS.open(F(CSVLogFilename), FILE_APPEND);
		if (csvDataFile.size() < MAX_CSV_SIZE_BYTES) {
			csvDataFile.printf("%s,%s,%s,%s,%s\r\n", timef, co2f, humidityf, temperaturef, luxf);
		} else {
			ESP_LOGE("", "%s is too large", (CSVLogFilename));
		}
		ESP_LOGI("", "%s is %ib", (CSVLogFilename), (csvDataFile.size()));
		csvDataFile.close();

		File jsonDataFile = SPIFFS.open(F(jsonLogPreview), FILE_WRITE);
		// serializeJsonPretty(jsonDataDocument, Serial);
		if (jsonDataFile.size() < MAX_JSON_SIZE_BYTES) {
			ESP_LOGI("", "%s is %ib", (jsonLogPreview), (jsonDataFile.size()));
			serializeJson(jsonDataDocument, jsonDataFile);
		} else {
			ESP_LOGE("", "%s is too large", (jsonLogPreview));	
		}
		jsonDataFile.close();		

		vTaskDelay((1000 * 60 * DATA_RECORD_INTERVAL_MINS)-1000 / portTICK_PERIOD_MS);	 // about don't waste cpu time while also accouting for compute time and drift
	}
}

void setup() {
	Serial.setTxBufferSize(1024);
	Serial.begin(115200);
	while (!Serial);
	ESP_LOGI("Kea Studios OSAQS (Kea CO2)", "%s Compiled on " __DATE__ " at " __TIME__ " by %s", VERSION, USER);

	if (SPIFFS.begin(true)) {  // Initialize SPIFFS (ESP32 SPI Flash Storage) and format on fail
		ESP_LOGV("", "FS init by %ims", millis());
	} else {
		ESP_LOGE("", "Error mounting SPIFFS (Even with Format on Fail)");
	}


	if (Wire.begin(21, 22,10000)) {	 // Initialize I2C Bus (CO2 and Light Sensor)
		// ESP_LOGV("I2C", "Initialized Correctly by %ims", millis()); //esp32-hal-i2c.c logs i2c init already
	} else {
		ESP_LOGE("I2C", "Can't begin I2C Bus");
	}

	globalVariablesSemaphore = xSemaphoreCreateBinary();
	i2cBusSemaphore = xSemaphoreCreateBinary();

	// 			Function, Name (for debugging), Stack size, Params, Priority, Handle
	xTaskCreate(accessPoint, "accessPoint", 5000, NULL, 3, NULL);
	xTaskCreate(AddressableRGBLeds, "AddressableRGBLeds", 5000, NULL, 2, NULL);
	xTaskCreate(LightSensor, "LightSensor", 5000, NULL, 1, NULL);
	xTaskCreate(CO2Sensor, "CO2Sensor", 5000, NULL, 1, NULL);
	xTaskCreate(addDataToFiles, "addDataToFiles", 5000, NULL, 1, NULL);
}

void loop() {
	vTaskSuspend(NULL);
}