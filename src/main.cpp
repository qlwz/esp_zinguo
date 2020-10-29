#include <Arduino.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WiFi.h>

#include "Framework.h"
#include "Zinguo.h"

void setup()
{
    Framework::one(115200);

    module = new Zinguo();

    Framework::setup();
}

void loop()
{
    Framework::loop();
}