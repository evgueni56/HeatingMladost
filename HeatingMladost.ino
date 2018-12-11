extern "C" {
#include "user_interface.h"
}
#include <SPI.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Time.h>
#include <stdlib.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <BlynkSimpleEsp8266.h>
#include <SimpleTimer.h>
#include <WidgetRTC.h>
#include <ArduinoOTA.h>  
#include <Button.h>
#include <ESP8266mDNS.h>

#define DEBUG
#define CLOUD

constexpr auto SonoffButton = 0;

// Data wire is plugged into port 2
constexpr auto ONE_WIRE_BUS = 14;

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature.... 
DallasTemperature sensors(&oneWire);

// arrays to hold device address
DeviceAddress insideThermometer;

// Instantiate Sonnof button
Button SButton(SonoffButton);

//Timer instantiate
BlynkTimer SleepTimer;
WidgetRTC rtc;

#ifdef DEBUG
WiFiServer TelnetServer(23);
WiFiClient Telnet;
WidgetTerminal terminal(V2);
#endif

#if defined(CLOUD)
char auth[] = "2a0827e0a5f64341a286feff0df25d7d"; // Mladoct CLOUD server
#else
char auth[] = "63020a5eab8540feac7e3301667233bb"; // Mladost Local server
#endif

const char t_ssdi[] = "ehome", t_pw[] = "ewgekrs61";

const int Rellay = 12;
const int Led = 13;

float tempC = 0, oldT = 0; // Avoid false Window open

						   // Times for the schedule in soconds of the day
long OnTime[2] /*= 18*3600 +30*60 18:30*/;
long OffTime[2] /*= 23*3600 + 30*60 23:30*/;
long this_second;
int blynk_timer, led_timer; // IDs of the Simple timers
bool relay_status = false, OnSwitch = false, wifi_cause = false;
int WindowOpen = 0;
bool EmergencyMode = false; // No Blynk connection or Button override
bool led_state = false; // False to lit the led
float req_temp = 20.0, low_temp = 15.0; // Default values if no Blynk connection
int blynk_relay_status = 0;
long wait_time;
long button_timeout;
bool button_state = false;
bool manual_mode = false;

void setup()
{
//	Serial.begin(74880);
	pinMode(14, INPUT_PULLUP);
	Wire.begin();
	SetupTemeratureSensor();
	pinMode(Rellay, OUTPUT);
	pinMode(Led, OUTPUT);
	digitalWrite(Led, led_state);
	digitalWrite(Rellay, 0);
	SButton.begin();
	WindowOpen = 0;
	WiFi.mode(WIFI_STA);
	WiFi.hostname("Heating");
	WiFi.begin(t_ssdi, t_pw);
	this_second = millis();
	while (WiFi.status() != WL_CONNECTED)
	{
		yield();
		if (millis() - this_second > 30000L)
		{
			wifi_cause = true;
			break;
		}

	}
	if (wifi_cause) // No connection - emergency mode
	{
		EmergencyMode = true;
		led_timer = SleepTimer.setInterval(500, led_blink);
		blynk_timer = SleepTimer.setInterval(10 * 1000, SleepTFunc);
		return;
	}
#if defined(CLOUD)
	Blynk.config(auth); // Mladost CLOD server
#else
	Blynk.config(auth, IPAddress(192, 168, 7, 130)); // Mladost local server
#endif

	setSyncInterval(60 * 60);
	blynk_timer = SleepTimer.setInterval(10 * 1000, SleepTFunc);
	ArduinoOTA.begin();
	MDNS.begin("Heating");
	digitalWrite(Led, 1);

#ifdef DEBUG
	TelnetServer.begin();
	TelnetServer.setNoDelay(true);
#endif

}

void loop()
{
	if (!wifi_cause) // Connected state
	{
		ArduinoOTA.handle();
		Blynk.run();
	}

	yield();
	SleepTimer.run();
#ifdef DEBUG
	handleTelnet();
#endif
	if (manual_mode) // Set by a button press
	{
		HandleHeating(22.0); // Yes, start unconditional heating to 22�
		if (SButton.pressed()) // Press button again to stop it
		{
			manual_mode = false;
			OnSwitch = false;
			Blynk.virtualWrite(V13, OnSwitch);
		}
		return; // Ignore the rest of the loop
	}
	if (SButton.pressed()) // Manually start heating at 22�
	{
		manual_mode = true;
		OnSwitch = true;
		Blynk.virtualWrite(V13, OnSwitch);
		return;
	}

	// Is it Emergency state?
	if (EmergencyMode)
	{
		HandleEmergency();
		return;
	}

	if (HandleWindow())
	{
		return;
	}

	if (CheckTime(OnTime[0], OffTime[0]) || CheckTime(OnTime[1], OffTime[1]))
	{
		HandleHeating(req_temp);
		return;
	}

	HandleHeating(low_temp);
}

void SetupTemeratureSensor()
{
	sensors.begin();
	sensors.getDeviceCount();
	sensors.getAddress(insideThermometer, 0);
	sensors.setResolution(insideThermometer, 12);
}

/*BLYNK_WRITE(V10) // Temperature
{
	low_temp = param.asFloat();
}*/

BLYNK_WRITE(V11) // Temperature
{
	req_temp = param.asFloat();
}

BLYNK_WRITE(V12) // Time schedule 1
{
	OnTime[0] = param[0].asLong();
	OffTime[0] = param[1].asLong();
}

BLYNK_WRITE(V14) // Time schedule 2
{
	OnTime[1] = param[0].asLong();
	OffTime[1] = param[1].asLong();
}

BLYNK_WRITE(V13)
{
	OnSwitch = param.asInt();
}

BLYNK_CONNECTED()
{
	rtc.begin();
	Blynk.syncAll();
}

void SleepTFunc()
{
	oldT = tempC;
	sensors.requestTemperatures();
	unsigned long timelatch = millis();
	while (millis() - timelatch < 1000) // Sensor needs 750us to convert the temp
		yield();
	tempC = sensors.getTempCByIndex(0) - 2.0;
	tempC = floor(tempC*10.0 + 0.5) / 10.0;

#ifdef DEBUG
	Telnet.println(tempC);
	terminal.println(tempC);
#endif

	if (!Blynk.connected()) // Not yet connected to server
	{
		return;
	}
	// Now push the values
	Blynk.virtualWrite(V0, tempC); // Current temperature
	Blynk.virtualWrite(V1, blynk_relay_status); // ON/OFF Status
	return;
}

void led_blink(void)
{
	led_state = !led_state;
	digitalWrite(Led, led_state);
}

void HandleEmergency()
{
	if (HandleWindow())
		return;

	HandleHeating(req_temp);

	// Retry WiFi
	WiFi.begin(t_ssdi, t_pw);
	this_second = millis();
	while (WiFi.status() != WL_CONNECTED)
	{
		yield();
		if (millis() - this_second > 30000L)
		{
			wifi_cause = true;
			break;
		}

	}
	if (!wifi_cause) // Connection restored
	{
		EmergencyMode = false;
		SleepTimer.disable(led_timer);
		led_state = true;
		digitalWrite(Led, led_state);
		return;
	}

}

bool CheckTime(long OnTime, long OffTime)
{
	if (OnTime == OffTime)
		return false; // No time set for the given interval

	this_second = elapsedSecsToday(now());

	if (OnTime < OffTime)
	{
		if (this_second > OnTime && this_second < OffTime)
			return true;
		else
			return false;
	}
	else
	{
		if (this_second > OffTime && this_second < OnTime)
			return false;
		else
			return true;
	}
}

bool HandleWindow()
{
	float riseTemp;
	if (WindowOpen == 1) // Wait 3 min after the window was closed
		if (millis() - wait_time > 180000L)
		{
			if (tempC - riseTemp >= 1.0) // Avoid false positive temp rise
			{
				WindowOpen = 0;
				return false;
			}
			else
			{
				WindowOpen = 2;
				return true;
			}
		}
		else
			return true;

	if (WindowOpen == 2)
	{
		if (oldT < tempC) //Temp. started to rise -> window closed. Start 3 min waiting
		{
			wait_time = millis();
			WindowOpen = 1;
			riseTemp = tempC;
		}
		return true;
	}

	if (oldT - tempC > 0.2) // If open window (fast temp. drop) -> disable heating and set WO flag
	{
		digitalWrite(Rellay, 0);
		digitalWrite(Led, 1);
		blynk_relay_status = 0;
		WindowOpen = 2;
		return true;
	}
	return false;
}

void HandleHeating(float reqtemp)
{
	if (OnSwitch)
	{
		if (tempC < reqtemp - 0.5)	// 0.5 degree hysteresis
		{
			digitalWrite(Rellay, 1);
			digitalWrite(Led, 0);
			blynk_relay_status = 255;
		}
		else if (tempC > reqtemp)
		{
			digitalWrite(Rellay, 0);
			digitalWrite(Led, 1);
			blynk_relay_status = 0;
		}
	}
	else
	{
		digitalWrite(Rellay, 0);
		digitalWrite(Led, 1);
		blynk_relay_status = 0;
	}
}


#ifdef DEBUG
void handleTelnet()
{
	if (TelnetServer.hasClient())
	{
		// client is connected
		if (!Telnet || !Telnet.connected())
		{
			if (Telnet) Telnet.stop();          // client disconnected
			Telnet = TelnetServer.available(); // ready for new client
		}
		else
		{
			TelnetServer.available().stop();  // have client, block new conections
		}
	}
}
#endif