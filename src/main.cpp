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
#include <NeoPixelAnimator.h>
#include <NeoPixelBus.h>

//I2C (CO2 & LUX)
#include <Wire.h>

//VEML7700 (LUX)
#include <DFRobot_VEML7700.h>

//SCD40/41 (CO2)
#include <SensirionI2CScd4x.h>

// -----------------------------------------
//
//    Main Settings
//
// -----------------------------------------
const char *ssid = "captive";  // FYI The SSID can't have a space in it.
const char *password = "12345678";
char LogFilename[] = "/Air_Quality_Data.csv";
#define RECORDING_TIME 10000  // time to record in milliseconds


// -----------------------------------------
//
//    Access Point Settings
//
// -----------------------------------------
const IPAddress localIP(4, 3, 2, 1);					// the IP address the webserver, Samsung requires the IP to be in public space
const IPAddress gatewayIP(4, 3, 2, 1);					// IP address of the network
const String localIPURL = "http://4.3.2.1/index.html";	// URL to the webserver


// -----------------------------------------
//
//    State Machines
//
// -----------------------------------------
enum deviceStates { STARTUP_DEVICE,
					FIND_GPS,
					PRE_IDLE,
					IDLE,
					PRE_RECORDING,
					RECORDING,
					POST_RECORDING,
					CHARGING };
deviceStates state = STARTUP_DEVICE;

enum LEDStates { STARTUP_LEDS,
				 FADE_TO_OFF,
				 LED_ANIMATION_UPDATE,
				 STARTUP_FADE_IN,
				 FADE_IN_OUT,
				 LED_OFF,
				 LED_IDLE,
				 CO2_SCALE };
LEDStates stripState = STARTUP_LEDS;

// -----------------------------------------
//
//    Global Variables
//
// -----------------------------------------
SensirionI2CScd4x scd4x;
float lux;
uint16_t co2 = 0;

// -----------------------------------------
//
//    ARGB Global Variables
//
// -----------------------------------------
const uint16_t PixelCount = 9;	  // make sure to set this to the number of pixels in your strip
const uint8_t PixelPin = 2;		  // make sure to set this to the correct pin
const uint8_t AnimationChannels = 1;  // we only need one as all the pixels are animated at once

RgbColor targetColor = RgbColor(0);
RgbColor prevtargetColor = RgbColor(0);

NeoGamma<NeoGammaTableMethod> colorGamma;  // for any fade animations it is best to correct gamma (this method uses a table of values to reduce cpu cycles)

NeoPixelBus<NeoGrbFeature, NeoWs2812xMethod> strip(PixelCount, PixelPin);

NeoPixelAnimator animations(AnimationChannels, NEO_MILLISECONDS);
// NEO_MILLISECONDS        1    // ~65 seconds max duration, ms updates
// NEO_CENTISECONDS       10    // ~10.9 minutes max duration, 10ms updates
// NEO_DECISECONDS       100    // ~1.8 hours max duration, 100ms updates

// what is stored for state is specific to the need, in this case, the colors.
// basically what ever you need inside the animation update function
struct MyAnimationState {
	RgbColor StartingColor;
	RgbColor EndingColor;
};

MyAnimationState animationState[AnimationChannels];


void ARGBLEDs(void *parameter) {
	const uint8_t FrameTime = 30;  // Milliseconds between frames 30ms = ~33.3fps

	const RgbColor fadeInColour = HslColor(random(360) / 360.0f, 1.0f, 0.5f);

	uint8_t brightness;

	LEDStates nextState = STARTUP_FADE_IN;
	stripState = STARTUP_LEDS;

	HslColor startColor;
	HslColor stopColor;

	srand(esp_random());

	void DrawPixels(bool corrected, HslColor startColor, HslColor stopColor);


	while (true) {
		switch (stripState) {
			case STARTUP_LEDS:
				strip.Begin();
				strip.Show();
				ESP_LOGV("LED Strip", "STARTED");
				stripState = CO2_SCALE;
				break;

			case STARTUP_FADE_IN:
				animationState[0].StartingColor = RgbColor(0);	// black
				animationState[0].EndingColor = fadeInColour;
				//animations.StartAnimation(0, 500, BlendAnimUpdate);

				stripState = LED_ANIMATION_UPDATE;
				nextState = FADE_TO_OFF;
				break;

			case FADE_TO_OFF:
				animationState[0].StartingColor = strip.GetPixelColor(0);
				animationState[0].EndingColor = RgbColor(0);  // black
				//animations.StartAnimation(0, 500, BlendAnimUpdate);
				stripState = LED_ANIMATION_UPDATE;
				nextState = LED_OFF;
				break;

			case CO2_SCALE:

				startColor = HslColor(0.25f, 1.0f, 0.1f);
				stopColor = HslColor(0.0f, 1.0f, 0.1f);
				DrawPixels(true, startColor, stopColor);
				vTaskDelay(1000 / portTICK_PERIOD_MS);	 // vTaskDelay wants ticks, not milliseconds

				break;

			case LED_ANIMATION_UPDATE:
				if (animations.IsAnimating()) {
					animations.UpdateAnimations();
					strip.Show();
					vTaskDelay(FrameTime / portTICK_PERIOD_MS);	 // vTaskDelay wants ticks, not milliseconds
				} else {
					stripState = nextState;
				}
				break;

			default:
				ESP_LOGE("LED Strip", "hit default -> stripState = STARTUP_LEDS");
				stripState = STARTUP_LEDS;
				break;
		}
	}
}

void DrawPixels(bool corrected, HslColor startColor, HslColor stopColor) {
	uint8_t fullPixels = strip.PixelCount() * ((co2-500) /1500.0f);
	//Serial.println(fullPixels);

	strip.ClearTo(0);

	// for (uint8_t index = 0; index < strip.PixelCount(); index++) {
	// 	float progress = index / static_cast<float>(strip.PixelCount() - 1);
	// 	RgbColor color = HslColor::LinearBlend<NeoHueBlendCounterClockwiseDirection>(startColor, stopColor, progress);

	// 	color = colorGamma.Correct(color);
	// 	strip.SetPixelColor(index, color);
	// }
	for (uint8_t index = 0; index < fullPixels; index++) {
	float progress = index / static_cast<float>(strip.PixelCount() - 1);
	RgbColor color = HslColor::LinearBlend<NeoHueBlendCounterClockwiseDirection>(startColor, stopColor, progress);

	color = colorGamma.Correct(color);
	strip.SetPixelColor(index, color);
	}

	strip.Show();
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

/*void I2CScan(void *parameter) {
	Wire.begin();
	vTaskDelay(3000 / portTICK_PERIOD_MS);	// allow time for boot

	byte error, address;
	int nDevices;
	Serial.println("Scanning..."); //ESP32 starts scanning available I2C devices
	nDevices = 0;
	for (address = 1; address < 127; address++) { //for loop to check number of devices on 127 address
		Wire.beginTransmission(address);
		error = Wire.endTransmission();
		if (error == 0) {									//if I2C device found
			Serial.print("I2C device found at address 0x"); //print this line if I2C device found
			if (address < 16) {
				Serial.print("0");
			}
			Serial.println(address, HEX); //prints the HEX value of I2C address
			nDevices++;
		} else if (error == 4) {
			Serial.print("Unknown error at address 0x");
			if (address < 16) {
				Serial.print("0");
			}
			Serial.println(address, HEX);
		}
	}
	if (nDevices == 0) {
		Serial.println("No I2C devices found\n"); ///If no I2C device attached print this message
	} else {
		Serial.println("done\n");
	}
	vTaskDelete(NULL);
}*/

void LightSensor(void *parameter) {
	DFRobot_VEML7700 als;
	vTaskDelay(3000 / portTICK_PERIOD_MS);	// allow time for boot

	als.begin();

	while (true){
		als.getAutoALSLux(lux);	 // Get the measured ambient light value
		vTaskDelay(100 / portTICK_PERIOD_MS);	// allow time for boot
	}
	vTaskDelete(NULL);
}

void printUint16Hex(uint16_t value) {
	Serial.print(value < 4096 ? "0" : "");
	Serial.print(value < 256 ? "0" : "");
	Serial.print(value < 16 ? "0" : "");
	Serial.print(value, HEX);
}

void printSerialNumber(uint16_t serial0, uint16_t serial1, uint16_t serial2) {
	Serial.print("SCD4x Serial Number: 0x");
	printUint16Hex(serial0);
	printUint16Hex(serial1);
	printUint16Hex(serial2);
	Serial.println();
}

void CO2Sensor(void *parameter) {
	//vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
	Wire.begin();

	uint16_t error;
	char errorMessage[256];

	scd4x.begin(Wire);

	// stop potentially previously started measurement
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
		printSerialNumber(serial0, serial1, serial2);
	}

	// Start Measurement
	error = scd4x.startPeriodicMeasurement();
	if (error) {
		Serial.print("Error trying to execute startPeriodicMeasurement(): ");
		errorToString(error, errorMessage, 256);
		Serial.println(errorMessage);
	}

	//Serial.println("Waiting for first measurement... (5 sec)");
	Serial.print("\nCO2 (ppm),Temp (degC),Humidity (%RH)\n");

	while (true) {
		uint16_t error;
		char errorMessage[256];

		vTaskDelay(1000 / portTICK_PERIOD_MS);  // vTaskDelay wants ticks, not milliseconds

		// Read Measurement
		
		float temperature = 0.0f;
		float humidity = 0.0f;
		bool isDataReady = false;
		error = scd4x.getDataReadyFlag(isDataReady);
		//Serial.println(isDataReady);
		if (error) {
			Serial.print("Error trying to execute getDataReadyFlag(): ");
			errorToString(error, errorMessage, 256);
			Serial.println(errorMessage);
			
		}else if (isDataReady) {
			error = scd4x.readMeasurement(co2, temperature, humidity);
			if (error) {
				Serial.print("Error trying to execute readMeasurement(): ");
				errorToString(error, errorMessage, 256);
				Serial.println(errorMessage);
			} else if (co2 == 0) {
				Serial.println("Invalid sample detected, skipping.");
			} else {
				Serial.printf("%i,%.1f,%.1f\n",co2,temperature,humidity);
			}
		}
		
	}
	vTaskDelete(NULL);
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
}

void loop() {
	switch (state) {
		case STARTUP_DEVICE:

			// 			Function, Name (for debugging), Stack size, Params, Priority, Handle
			//xTaskCreate(accessPoint, "accessPoint", 5000, NULL, 1, NULL);
			xTaskCreate(ARGBLEDs, "ARGBLEDs", 5000, NULL, 1, NULL);
			//xTaskCreate(I2CScan, "I2CScan", 5000, NULL, 1, NULL);
			xTaskCreate(LightSensor, "LightSensor", 5000, NULL, 1, NULL);
			xTaskCreatePinnedToCore(CO2Sensor, "CO2Sensor", 5000, NULL, 1, NULL,	1);

			state = IDLE;
			break;

		case IDLE:
			//ESP_LOGV("deviceState", "IDLE");
			vTaskDelay(100 / portTICK_PERIOD_MS);
			break;

		case PRE_RECORDING:
			ESP_LOGV("deviceState", "PRE_RECORDING");
			state = RECORDING;
			break;

		case RECORDING:
			ESP_LOGD("deviceState", "RECORDING");
			vTaskDelay(RECORDING_TIME / portTICK_PERIOD_MS);
			state = POST_RECORDING;
			break;

		case POST_RECORDING:
			ESP_LOGV("deviceState", "POST_RECORDING");
			// xTaskCreate(appendLineToCSV, "appendLineToCSV", 5000, NULL, 1, NULL);
			state = IDLE;
			break;

		case CHARGING:
			break;

		default:
			ESP_LOGE("Hit default Case in state machine", "Restarting...");
			vTaskDelay(1000 / portTICK_PERIOD_MS);	// vTaskDelay wants ticks, not milliseconds
			ESP.restart();
			break;
	}
	vTaskDelay(1);	// Keep RTOS Happy with a 1 tick delay when there is nothing to do
}