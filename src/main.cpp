#include <Arduino.h>

// Wifi, Webserver and DNS
#include <DNSServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"

// Onboard Flash Storage
#include <SPIFFS.h>

// Adressable LEDs
#include <NeoPixelBusLg.h>	// instead of NeoPixelBus.h (has Luminace and Gamma as default)

//I2C (CO2 & LUX)
#include <Wire.h>

//VEML7700 (LUX)
#include <DFRobot_VEML7700.h>

//SCD40/41 (CO2)
#include <SensirionI2CScd4x.h>


// -----------------------------------------
//
//    Access Point Settings
//
// -----------------------------------------
const char *ssid = "captive";  // FYI The SSID can't have a space in it.
const char *password = "12345678";
char LogFilename[] = "/Air_Quality_Data.csv";

const IPAddress localIP(4, 3, 2, 1);					// the IP address the webserver, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1);					// IP address of the network
const String localIPURL = "http://4.3.2.1/index.html";	// URL to the webserver


// -----------------------------------------
//
//    Global Variables
//
// -----------------------------------------
uint16_t lux = 0;
uint16_t co2 = 450;




float mapCO2toHue(uint16_t x, uint16_t in_min, uint16_t in_max, float out_min, float out_max) {
	return (float)(x - in_min) * (out_max - out_min) / (float)(in_max - in_min) + out_min;
}

void ARGBLEDs(void *parameter) {
	const uint8_t pixelCount = 9;
	const uint8_t pixelPin = 2;
	const uint8_t FrameTime = 30;  // Milliseconds between frames 30ms = ~33.3fps max
	NeoPixelBusLg<NeoGrbFeature, NeoWs2812xMethod> strip(pixelCount, pixelPin); //uses i2s silicon remaped to any pin to drive led data 

	const uint16_t co2Max = 2000;	// ppm
	const float co2MaxHue = 0.0;	 // red
	const uint16_t co2min = 450;  // ppm
	const float co2MinHue = 0.3;  // green

\
	const RgbColor blackColor = RgbColor(0);
	const RgbColor flashColor = RgbColor(255, 0, 0);
	const uint8_t ppmPerPixel = (co2Max - co2min) / pixelCount;

	uint16_t ppmDrawn = 0;
	float hue = co2MinHue;
	float lastL = 1.0; //Luminance of last led in scale
	uint8_t brightness = 255; //
	uint16_t ledCO2 = co2min;  // internal co2 which has been smoothed

	strip.Begin();
	strip.SetLuminance(255);  // (0-255) - initially at full brightness
	strip.Show();
	ESP_LOGV("LED Strip", "STARTED");

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

		//Smooth CO2
		if (co2 != 0) {
			ledCO2 = ((ledCO2 * 49) + co2) / 50;
		}

		//Convert Lux to Brightness and smooth
		if (lux < (255 * 2)) {
			brightness = ((brightness * 49) + (lux / 2)) / 50;
		} else {
			brightness = ((brightness * 49) + (255)) / 50;
		}

		if (ledCO2 < co2Max) {
			strip.SetLuminance(brightness); //Luminance is a gamma corrected Brightness
			hue = mapCO2toHue(ledCO2, co2min, co2Max, co2MinHue, co2MaxHue);//map hue from co2 level
			
			//Find Which Pixels are filled with base color, inbetween and not lit
			ppmDrawn = co2min;//ppmDrawn is a counter for how many ppm have been displayed by the previous pixels
			uint8_t currentPixel = 0;
			while (ppmDrawn <= (ledCO2 - ppmPerPixel)) {
				ppmDrawn += ppmPerPixel;
				currentPixel++;
			}

			strip.ClearTo(HsbColor(hue, 1.0f, 1.0f), 0, currentPixel - 1);	// apply base color to fully on pixels

			lastL = float((ledCO2 - ppmDrawn) / ppmPerPixel);
			strip.SetPixelColor(currentPixel, HsbColor(hue, 1.0f, lastL));//apply the inbetween color mix for the inbetween pixel

			strip.ClearTo(blackColor, currentPixel + 1, pixelCount);  // apply black to the last few leds

			strip.Show();// push led data to buffer
			vTaskDelay(FrameTime / portTICK_PERIOD_MS);// time between frames

		} else {  // co2 too high flash on off

			brightness = 255;
			strip.SetLuminance(brightness);

			while (co2 > co2Max) {
				strip.ClearTo(flashColor);
				strip.Show();
				vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
				strip.ClearTo(blackColor);
				strip.Show();
				vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
			}
			ledCO2 = co2;//ledCo2 will have not been updated
		}
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
	WiFi.setSleep(false);

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

	server.serveStatic("/Water_Quality_Data.csv", SPIFFS, "/Water_Quality_Data.csv").setCacheControl("no-store");  // fetch data file every page reload
	server.serveStatic("/index.html", SPIFFS, "/index.html").setCacheControl("max-age=120");					   // serve html file

	// Required
	server.on("/connecttest.txt", [](AsyncWebServerRequest *request) { request->redirect("http://logout.net"); });	// windows 11 captive portal workaround
	server.on("/wpad.dat", [](AsyncWebServerRequest *request) { request->send(404); });								// Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32 :)

	// Background responses: Probably not all are Required, but some are. Others might speed things up?
	// A Tier (commonly used by modern systems)
	server.on("/generate_204", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });		   // android captive portal redirect
	server.on("/redirect", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });			   // microsoft redirect
	server.on("/hotspot-detect.html", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });  // apple call home
	server.on("/canonical.html", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });	   // firefox captive portal call home
	server.on("/success.txt", [](AsyncWebServerRequest *request) { request->send(200); });					   // firefox captive portal call home
	server.on("/ncsi.txt", [](AsyncWebServerRequest *request) { request->redirect(localIPURL); });			   // windows call home

	// B Tier (uncommon)
	//  server.on("/chrome-variations/seed",[](AsyncWebServerRequest *request){request->send(200);}); //chrome captive portal call home
	//  server.on("/service/update2/json",[](AsyncWebServerRequest *request){request->send(200);}); //firefox?
	//  server.on("/chat",[](AsyncWebServerRequest *request){request->send(404);}); //No stop asking Whatsapp, there is no internet connection
	//  server.on("/startpage",[](AsyncWebServerRequest *request){request->redirect(localIPURL);});

	server.serveStatic("/", SPIFFS, "/").setCacheControl("max-age=120").setDefaultFile("index.html");  // serve any file on the device when requested

	server.onNotFound([](AsyncWebServerRequest *request) {
		request->redirect(localIPURL);
		ESP_LOGW("WebServer", "Page not found sent redirect to localIPURL");
		// DEBUG_SERIAL.print("onnotfound ");
		// DEBUG_SERIAL.print(request->host());       //This gives some insight into whatever was being requested on the serial monitor
		// DEBUG_SERIAL.print(" ");
		// DEBUG_SERIAL.print(request->url());
		// DEBUG_SERIAL.print(" sent redirect to " + localIPURL +"\n");
	});

	server.begin();

	ESP_LOGV("WebServer", "Startup complete by %ims",(millis()));

	while (true) {
		dnsServer.processNextRequest();
		vTaskDelay(DNS_INTERVAL / portTICK_PERIOD_MS);
	}
}

void LightSensor(void *parameter) {
	DFRobot_VEML7700 als;
	float rawLux;
	uint8_t error = 0;
	vTaskDelay(1000 / portTICK_PERIOD_MS);	// allow time for boot and wire begin

	als.begin(); //comment out wire.begin() in this function

	while (true){
		error = als.getAutoWhiteLux(rawLux);  // Get the measured ambient light value

		if (error) {
			ESP_LOGW("VEML7700", "getAutoWhiteLux(): STATUS_ERROR");
		}else if (rawLux <65000){//overflow protection for 16bit int
			lux = int(rawLux);
		}else{
			lux = 65000;
		}

		vTaskDelay(500 / portTICK_PERIOD_MS);
	}
}

void CO2Sensor(void *parameter) {
	SensirionI2CScd4x scd4x;

	Wire.begin();

	uint16_t error;
	uint16_t co2Raw = 0;
	float temperature = 0.0f;
	float humidity = 0.0f;
	char errorMessage[256];
	bool isDataReady = false;

	scd4x.begin(Wire);

	error = scd4x.startPeriodicMeasurement();
	if (error) {
		errorToString(error, errorMessage, 256);
		ESP_LOGD("SCD4x", "startPeriodicMeasurement(): %s", errorMessage);
	}

	Serial.print("\nCO2 (ppm),Temp (degC),Humidity (%RH)\n");

	while (true) {

		error = 0;
		isDataReady = false;
		do{
			error = scd4x.getDataReadyFlag(isDataReady);
			vTaskDelay(30 / portTICK_PERIOD_MS);	// about 5s between readings
		} while (isDataReady == false);


		if (error) {
			errorToString(error, errorMessage, 256);
			ESP_LOGW("SCD4x", "getDataReadyFlag(): %s", errorMessage);
			
		}else{
			error = scd4x.readMeasurement(co2Raw, temperature, humidity);
			if (error) {
				errorToString(error, errorMessage, 256);
				ESP_LOGW("SCD4x", "readMeasurement(): %s", errorMessage);

			} else if (co2Raw == 0) {
				ESP_LOGW("SCD4x", "CO2 = 0ppm, skipping");
			} else {
				Serial.printf("%i,%.1f,%.1f\n", co2Raw, temperature, humidity);
				co2 = co2Raw;
			}
		}
		vTaskDelay(4750 / portTICK_PERIOD_MS);  //about 5s between readings, don't waste cpu time
	}
}

void setup() {
	Serial.setTxBufferSize(1024);
	Serial.begin(115200);
	while (!Serial);
	ESP_LOGI("OSAQS", "Compiled " __DATE__ " " __TIME__ " by CD_FER");

	if (SPIFFS.begin()) {  // Initialize SPIFFS (ESP32 SPI Flash Storage)
		if (SPIFFS.exists(LogFilename)) {
			ESP_LOGV("File System", "Initialized Correctly by %ims", millis());
		} else {
			ESP_LOGE("File System", "Can't find %s", (LogFilename));
		}
	} else {
		ESP_LOGE("File System", "Can't mount SPIFFS");
	}

	// 			Function, Name (for debugging), Stack size, Params, Priority, Handle
	// xTaskCreate(accessPoint, "accessPoint", 5000, NULL, 1, NULL);
	xTaskCreatePinnedToCore(ARGBLEDs, "ARGBLEDs", 5000, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(LightSensor, "LightSensor", 5000, NULL, 1, NULL, 1);
	xTaskCreatePinnedToCore(CO2Sensor, "CO2Sensor", 5000, NULL, 1, NULL, 1);
}

void loop() {
	vTaskSuspend(NULL);
	//vTaskDelay(1);	// Keep RTOS Happy with a 1 tick delay when there is nothing to do
}