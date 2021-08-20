#include "Zinguo.h"
#ifdef USE_EXPAND
#include "Rtc.h"
#endif
#ifdef USE_HOMEKIT
#include "HomeKit.h"
#endif

#pragma region 继承

void Zinguo::init()
{
    pinMode(PIN_DATA, OUTPUT);      //74HC595数据
    pinMode(PIN_LOAD, OUTPUT);      //74HC595锁存
    pinMode(PIN_CLOCK, OUTPUT);     //74HC595时钟
    pinMode(PIN_BEEP, OUTPUT);      //风铃器引脚
    delay(1000);                    //延时1s,等待初始化,,for sc09a
    pinMode(twi_sda, INPUT_PULLUP); //SDA,for sc09a
    pinMode(twi_scl, INPUT_PULLUP); //SCL,for sc09a
    convertTemp();                  //初始化读取温度
    dispCtrl();                     //初始化输出端口
    schTicker = new Ticker();

#ifdef USE_EXPAND
    pinMode(LED_IO, OUTPUT); // LED
    pinMode(BUTTON_IO, INPUT_PULLUP);
    if (digitalRead(BUTTON_IO))
    {
        buttonStateFlag2 |= DEBOUNCED_STATE | UNSTABLE_STATE;
    }
    checkCanLed(true);
#endif
}

void Zinguo::loop()
{
#ifdef USE_EXPAND
    cheackButton();
#endif
    dispCtrl();
    unsigned short key = getKey(); //获取键值
    if (key != 0x00)
    {
        if (buttonTiming == false)
        {
            buttonTiming = true;
            buttonTimingStart = millis();
        }
        else
        {
            if (touchKey != key) //如果前后按键不相等，则处理按键值
            {
                if (millis() >= (buttonTimingStart + buttonDebounceTime))
                {
                    Log::Info(PSTR("TouchKey: 0x%0X"), key);
                    touchKey = key; //缓冲当前按键键值
                    analysisKey(touchKey);
                }
            }

            if (millis() >= (buttonTimingStart + buttonLongPressTime))
            {
                buttonAction = 2;
            }
        }
    }
    else
    {
        buttonTiming = false;
        if (buttonAction == 2 && touchKey == 0x1000) // 执行长按动作
        {
            WifiMgr::setupWifiManager(false);
        }
        touchKey = key;
        buttonAction = 0;
    }
    dispCtrl(); //刷新数码管、LED灯、74HC595

    if (bitRead(operationFlag, 0))
    {
        bitClear(operationFlag, 0);

#ifndef SkyNet
        if (bitRead(controlLED, KEY_CLOSE_ALL - 1))
        {
            controlLED &= ~(1 << 2);
            controlOut &= ~(1 << 2);
            Mqtt::publish(Mqtt::getStatTopic(F("close")), "off", globalConfig.mqtt.retain);
        }
#endif

        if (perSecond % 5 == 0 && !mqttTemp)
        {
            // 每5s读取一次温度值
            convertTemp();
        }

        if (closeBlowTime != 127)
        {
            if (closeBlowTime <= 1)
            {
                closeBlowTime = 127;
                switchBlowReal(false);
            }
            else
            {
                closeBlowTime--;
            }
        }

        if (ventilationTime != 0 && ventilationTime <= perSecond)
        {
            Log::Info(PSTR("Ventilation Timeout %d %d"), ventilationTime, perSecond);
            ventilationTime = 0;
            switchVentilation(false);
        }

        if (warmTime != 0 && warmTime <= perSecond)
        {
            Log::Info(PSTR("Warm Timeout %d %d"), warmTime, perSecond);
            warmTime = 0;
            switchWarm1(false);
            if (config.dual_warm)
            {
                switchWarm2(false);
            }
        }

#ifdef USE_EXPAND
        if (perSecond % 60 == 0)
        {
            checkCanLed();
        }
#endif

        if (config.report_interval > 0 && (perSecond % config.report_interval) == 0)
        {
            reportPower();
        }
    }
}

void Zinguo::perSecondDo()
{
    bitSet(operationFlag, 0);
}
#pragma endregion

#pragma region 配置

void Zinguo::readConfig()
{
    Config::moduleReadConfig(MODULE_CFG_VERSION, sizeof(ZinguoConfigMessage), ZinguoConfigMessage_fields, &config);

#ifdef USE_EXPAND
    if (config.led_light == 0)
    {
        config.led_light = 100;
    }
    if (config.led_time == 0)
    {
        config.led_time = 2;
    }
    ledLight = config.led_light * 10 + 23;
#endif
}

void Zinguo::resetConfig()
{
    Log::Info(PSTR("moduleResetConfig . . . OK"));
    memset(&config, 0, sizeof(ZinguoConfigMessage));
    config.dual_motor = true;
    config.dual_warm = true;
    config.delay_blow = 30;
    config.linkage = 1;
    config.max_temp = 40;
    config.close_warm = 30;
    config.close_ventilation = 30;
    config.beep = true;

#ifdef USE_EXPAND
    config.led_light = 50;
    config.led_time = 3;
#endif

#ifdef WIFI_SSID
    config.dual_warm = false;

    config.led_type = 2;
    config.led_start = 1800;
    config.led_end = 2300;

    config.report_interval = 60 * 5;
    globalConfig.mqtt.interval = 60 * 60;
    globalConfig.debug.type = globalConfig.debug.type | 4;
#endif
}

void Zinguo::saveConfig(bool isEverySecond)
{
    Config::moduleSaveConfig(MODULE_CFG_VERSION, ZinguoConfigMessage_size, ZinguoConfigMessage_fields, &config);
}
#pragma endregion

#pragma region MQTT

bool Zinguo::mqttCallback(char *topic, char *payload, char *cmnd)
{
    if (strcmp(cmnd, "light") == 0)
    {
        switchLight(strcmp(payload, "on") == 0 ? true : (strcmp(payload, "off") == 0 ? false : !bitRead(controlOut, KEY_LIGHT - 1)));
        return true;
    }
    else if (strcmp(cmnd, "ventilation") == 0)
    {
        switchVentilation(strcmp(payload, "on") == 0 ? true : (strcmp(payload, "off") == 0 ? false : !bitRead(controlOut, KEY_VENTILATION - 1)));
        return true;
    }
    else if (strcmp(cmnd, "close") == 0)
    {
        switchCloseAll(strcmp(payload, "on") == 0 ? true : (strcmp(payload, "off") == 0 ? false : !bitRead(controlOut, KEY_VENTILATION - 1)));
        return true;
    }
    else if (strcmp(cmnd, "warm2") == 0)
    {
        switchWarm2(strcmp(payload, "on") == 0 ? true : (strcmp(payload, "off") == 0 ? false : !bitRead(controlOut, KEY_WARM_2 - 1)));
        return true;
    }
    else if (strcmp(cmnd, "blow") == 0)
    {
        switchBlow(strcmp(payload, "on") == 0 ? true : (strcmp(payload, "off") == 0 ? false : !bitRead(controlOut, KEY_BLOW - 1)));
        return true;
    }
    else if (strcmp(cmnd, "warm1") == 0)
    {
        switchWarm1(strcmp(payload, "on") == 0 ? true : (strcmp(payload, "off") == 0 ? false : !bitRead(controlOut, KEY_WARM_1 - 1)));
        return true;
    }
    else if (strcmp(cmnd, "temp") == 0)
    {
        mqttTemp = true;
        controlTemp = atof(payload);
        Mqtt::publish(Mqtt::getStatTopic(F("temp")), payload, globalConfig.mqtt.retain);
        if (controlTemp >= config.max_temp)
        {
            switchWarm1(false);
            if (config.dual_warm)
            {
                switchWarm2(false);
            }
        }
        return true;
    }
    else if (strcmp(cmnd, "report") == 0)
    {
        reportPower();
        return true;
    }
    return false;
}

void Zinguo::mqttConnected()
{
    if (globalConfig.mqtt.discovery)
    {
        mqttDiscovery(true);
    }
    reportPower();
}

void Zinguo::mqttDiscovery(bool isEnable)
{
    char topic[100];
    char message[500];

    String tims[] = {F("light"), F("ventilation"), F("close"), F("blow"), F("warm1"), F("warm2")};

    for (size_t i = 0; i < (config.dual_warm ? 6 : 5); i++)
    {
        sprintf(topic, PSTR("%s/%s/%s_%s/config"), globalConfig.mqtt.discovery_prefix, tims[i] == F("light") ? F("light") : F("switch"), UID, tims[i].c_str());
        if (isEnable)
        {
            sprintf(message,
                    PSTR("{\"name\":\"%s_%s\","
                         "\"cmd_t\":\"%s\","
                         "\"stat_t\":\"%s\","
                         "\"pl_off\":\"off\","
                         "\"pl_on\":\"on\","
                         "\"avty_t\":\"%s\","
                         "\"pl_avail\":\"online\","
                         "\"pl_not_avail\":\"offline\"}"),
                    UID, tims[i].c_str(),
                    Mqtt::getCmndTopic(tims[i]).c_str(),
                    Mqtt::getStatTopic(tims[i]).c_str(),
                    Mqtt::getTeleTopic(F("availability")).c_str());
            //Log::Info(PSTR("discovery: %s - %s"), topic, message);
            Mqtt::publish(topic, message, true);
        }
        else
        {
            Mqtt::publish(topic, "", true);
        }
    }

    sprintf(topic, PSTR("%s/sensor/%s_temp/config"), globalConfig.mqtt.discovery_prefix, UID);
    if (isEnable)
    {
        sprintf(message, PSTR("{\"name\":\"%s_temp\",\"stat_t\":\"%s\",\"unit_of_meas\":\"°C\"}"), UID, Mqtt::getStatTopic(F("temp")).c_str());
        //Log::Info(PSTR("discovery: %s - %s"), topic, message);
        Mqtt::publish(topic, message, true);
    }
    else
    {
        Mqtt::publish(topic, "", true);
    }
    if (isEnable)
    {
        Mqtt::availability();
    }
}
#pragma endregion

#pragma region HTTP

void Zinguo::httpAdd(ESP8266WebServer *server)
{
    server->on(F("/zinguo_setting"), std::bind(&Zinguo::httpSetting, this, server));
    server->on(F("/zinguo_do"), std::bind(&Zinguo::httpDo, this, server));
    server->on(F("/ha"), std::bind(&Zinguo::httpHa, this, server));
#ifdef USE_HOMEKIT
    server->on(F("/homekit"), std::bind(&homekit_http, server));
#endif
}

String Zinguo::httpGetStatus(ESP8266WebServer *server)
{
    String data = F("\"zinguo_light\":");
    data += bitRead(controlOut, KEY_LIGHT - 1) ? 1 : 0;
    data += F(",\"zinguo_ventilation\":");
    data += bitRead(controlOut, KEY_VENTILATION - 1) ? 1 : 0;
    data += F(",\"zinguo_warm1\":");
    data += bitRead(controlOut, KEY_WARM_1 - 1) ? 1 : 0;
    data += F(",\"zinguo_warm2\":");
    data += bitRead(controlOut, KEY_WARM_2 - 1) ? 1 : 0;
    data += F(",\"zinguo_blow\":");
    data += bitRead(controlOut, KEY_BLOW - 1) ? 1 : 0;
    return data;
}

void Zinguo::httpHtml(ESP8266WebServer *server)
{
    server->sendContent_P(
        PSTR("<table class='gridtable'><thead><tr><th colspan='2'>控制浴霸</th></tr></thead><tbody>"
             "<tr colspan='2' style='text-align: center'><td>"
             "<button type='button' style='width:56px' onclick=\"ajaxPost('/zinguo_do', 'key=8');\" id='zinguo_warm1' class='btn-success'>风暖1</button>"
             " <button type='button' style='width:50px' onclick=\"ajaxPost('/zinguo_do', 'key=7');\" id='zinguo_blow' class='btn-success'>吹风</button>"
             " <button type='button' style='width:56px' onclick=\"ajaxPost('/zinguo_do', 'key=6');\" id='zinguo_warm2' class='btn-success'>风暖2</button>"
             " <button type='button' style='width:50px' onclick=\"ajaxPost('/zinguo_do', 'key=1');\" id='zinguo_light' class='btn-success'>照明</button>"
             " <button type='button' style='width:50px' onclick=\"ajaxPost('/zinguo_do', 'key=2');\" id='zinguo_ventilation' class='btn-success'>换气</button>"
             " <button type='button' style='width:50px' onclick=\"ajaxPost('/zinguo_do', 'key=3');\" id='zinguo_close' class='btn-info'>全关</button>"
             "</td></tr></tbody></table>"));

    server->sendContent_P(
        PSTR("<form method='post' action='/zinguo_setting' onsubmit='postform(this);return false'>"
             "<table class='gridtable'><thead><tr><th colspan='2'>浴霸设置</th></tr></thead><tbody>"
             "<tr><td>电机数量</td><td>"
             "<label class='bui-radios-label'><input type='radio' name='dual_motor' value='0'/><i class='bui-radios'></i> 单电机</label>&nbsp;&nbsp;&nbsp;&nbsp;"
             "<label class='bui-radios-label'><input type='radio' name='dual_motor' value='1'/><i class='bui-radios'></i> 双电机</label>"
             "</td></tr>"

             "<tr><td>风暖数量</td><td>"
             "<label class='bui-radios-label'><input type='radio' name='dual_warm' value='0'/><i class='bui-radios'></i> 单风暖</label>&nbsp;&nbsp;&nbsp;&nbsp;"
             "<label class='bui-radios-label'><input type='radio' name='dual_warm' value='1'/><i class='bui-radios'></i> 双风暖</label>"
             "</td></tr>"

             "<tr><td>吹风联动</td><td>"
             "<label class='bui-radios-label'><input type='radio' name='linkage' value='0'/><i class='bui-radios'></i> 不联动</label><br/>"
             "<label class='bui-radios-label'><input type='radio' name='linkage' value='1'/><i class='bui-radios'></i> 暖1或暖2联动</label><br/>"
             "<label class='bui-radios-label'><input type='radio' name='linkage' value='2'/><i class='bui-radios'></i> 暖1联动</label><br/>"
             "<label class='bui-radios-label'><input type='radio' name='linkage' value='3'/><i class='bui-radios'></i> 暖2联动</label>"
             "</td></tr>"));

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("<tr><td>吹风延时</td><td><input type='number' min='0' max='90' name='delay_blow' required value='%d'>&nbsp;秒</td></tr>"),
               config.delay_blow == 127 ? 0 : config.delay_blow);
    server->sendContent_P(tmpData);

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("<tr><td>过温保护</td><td><input type='number' min='15' max='45' name='max_temp' required value='%d'>&nbsp;度</td></tr>"),
               config.max_temp);
    server->sendContent_P(tmpData);

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("<tr><td>取暖定时关闭</td><td><input type='number' min='1' max='90' name='close_warm' required value='%d'>&nbsp;分钟</td></tr>"),
               config.close_warm);
    server->sendContent_P(tmpData);

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("<tr><td>换气定时关闭</td><td><input type='number' min='1' max='90' name='close_ventilation' required value='%d'>&nbsp;分钟</td></tr>"),
               config.close_ventilation);
    server->sendContent_P(tmpData);

    server->sendContent_P(
        PSTR("<tr><td>按键声音</td><td>"
             "<label class='bui-radios-label'><input type='radio' name='beep' value='0'/><i class='bui-radios'></i> 关闭</label>&nbsp;&nbsp;&nbsp;&nbsp;"
             "<label class='bui-radios-label'><input type='radio' name='beep' value='1'/><i class='bui-radios'></i> 开启</label>"
             "</td></tr>"

             "<tr><td>LED颜色</td><td>"
             "<label class='bui-radios-label'><input type='radio' name='reverse_led' value='0'/><i class='bui-radios'></i> 待机蓝色</label>&nbsp;&nbsp;&nbsp;&nbsp;"
             "<label class='bui-radios-label'><input type='radio' name='reverse_led' value='1'/><i class='bui-radios'></i> 待机红色</label>"
             "</td></tr>"));

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("<tr><td>主动上报间隔</td><td><input type='number' min='0' max='3600' name='report_interval' required value='%d'>&nbsp;秒，0关闭</td></tr>"),
               config.report_interval);
    server->sendContent_P(tmpData);

#ifdef USE_EXPAND
    server->sendContent_P(
        PSTR("<tr><td>面板指示灯</td><td>"
             "<label class='bui-radios-label'><input type='radio' name='led_type' value='0'/><i class='bui-radios'></i> 无</label>&nbsp;&nbsp;&nbsp;&nbsp;"
             "<label class='bui-radios-label'><input type='radio' name='led_type' value='1'/><i class='bui-radios'></i> 普通</label>&nbsp;&nbsp;&nbsp;&nbsp;"
             "<label class='bui-radios-label'><input type='radio' name='led_type' value='2'/><i class='bui-radios'></i> 呼吸灯</label>&nbsp;&nbsp;&nbsp;&nbsp;"
             //"<label class='bui-radios-label'><input type='radio' name='led_type' value='3'/><i class='bui-radios'></i> WS2812</label>"
             "</td></tr>"));

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("<tr><td>指示灯亮度</td><td><input type='range' min='1' max='100' name='led_light' value='%d' onchange='ledLightRangOnChange(this)'/>&nbsp;<span>%d%</span></td></tr>"),
               config.led_light, config.led_light);
    server->sendContent_P(tmpData);

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("<tr><td>渐变时间</td><td><input type='number' name='relay_led_time' value='%d'>毫秒</td></tr>"),
               config.led_time);
    server->sendContent_P(tmpData);

    String tmp = "";
    for (uint8_t i = 0; i <= 23; i++)
    {
        tmp += F("<option value='{v1}'>{v}:00</option>");
        tmp += F("<option value='{v2}'>{v}:30</option>");
        tmp.replace(F("{v1}"), String(i * 100));
        tmp.replace(F("{v2}"), String(i * 100 + 30));
        tmp.replace(F("{v}"), i < 10 ? "0" + String(i) : String(i));
    }

    server->sendContent_P(
        PSTR("<tr><td>指示灯时间段</td><td>"
             "<select id='led_start' name='led_start'>"));

    server->sendContent(tmp);

    server->sendContent_P(
        PSTR("</select>"
             "&nbsp;&nbsp;到&nbsp;&nbsp;"
             "<select id='led_end' name='led_end'>"));
    server->sendContent(tmp);
    server->sendContent_P(PSTR("</select>"));
    server->sendContent_P(PSTR("</td></tr>"));
#endif

    server->sendContent_P(
        PSTR("<tr><td colspan='2'><button type='submit' class='btn-info'>设置</button><br>"
             "<button type='button' class='btn-success' style='margin-top:10px' onclick='window.location.href=\"/ha\"'>下载HA配置文件</button></td></tr>"
             "</tbody></table></form>"));

#ifdef USE_HOMEKIT
    homekit_html(server);
#endif

    server->sendContent_P(
        PSTR("<script type='text/javascript'>"
             "function setDataSub(data,key){if(key.substr(0,6)=='zinguo'){id(key).setAttribute('class',data[key]==1?'btn-success':'btn-info');return true}return false}"));

    if (!bitRead(controlOut, KEY_LIGHT - 1))
    {
        server->sendContent_P(PSTR("id('zinguo_light').setAttribute('class', 'btn-info');"));
    }
    if (!bitRead(controlOut, KEY_VENTILATION - 1))
    {
        server->sendContent_P(PSTR("id('zinguo_ventilation').setAttribute('class', 'btn-info');"));
    }
    if (!bitRead(controlOut, KEY_WARM_1 - 1))
    {
        server->sendContent_P(PSTR("id('zinguo_warm1').setAttribute('class', 'btn-info');"));
    }
    if (!bitRead(controlOut, KEY_WARM_2 - 1))
    {
        server->sendContent_P(PSTR("id('zinguo_warm2').setAttribute('class', 'btn-info');"));
    }
    if (!bitRead(controlOut, KEY_BLOW - 1))
    {
        server->sendContent_P(PSTR("id('zinguo_blow').setAttribute('class', 'btn-info');"));
    }

#ifdef USE_EXPAND
    snprintf_P(tmpData, sizeof(tmpData), PSTR("setRadioValue('led_type', '%d');id('led_start').value=%d;id('led_end').value=%d;"),
               config.led_type, config.led_start, config.led_end);
    server->sendContent_P(tmpData);
    server->sendContent_P(PSTR("function ledLightRangOnChange(the){the.nextSibling.nextSibling.innerHTML=the.value+'%'};"));
#endif

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("setRadioValue('dual_warm', '%d');"
                    "setRadioValue('linkage', '%d');"
                    "setRadioValue('beep', '%d');"
                    "setRadioValue('reverse_led', '%d');"
                    "setRadioValue('dual_motor', '%d');"
                    "</script>"),
               config.dual_warm ? 1 : 0,
               config.linkage,
               config.beep ? 1 : 0,
               config.reverse_led ? 1 : 0,
               config.dual_motor ? 1 : 0);
    server->sendContent_P(tmpData);
}

void Zinguo::httpDo(ESP8266WebServer *server)
{
    analysisKey(server->arg(F("key")).toInt());

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1,\"msg\":\"操作成功\",\"data\":{"));
    server->sendContent(httpGetStatus(server));
    server->sendContent_P(PSTR("}}"));
}

void Zinguo::httpSetting(ESP8266WebServer *server)
{
    config.dual_motor = server->arg(F("dual_motor")) == "1" ? true : false;
    config.dual_warm = server->arg(F("dual_warm")) == "1" ? true : false;
    config.linkage = server->arg(F("linkage")).toInt();
    config.delay_blow = server->arg(F("delay_blow")) == "0" ? 127 : server->arg(F("delay_blow")).toInt();
    config.max_temp = server->arg(F("max_temp")).toInt();
    config.close_warm = server->arg(F("close_warm")).toInt();
    config.close_ventilation = server->arg(F("close_ventilation")).toInt();
    config.beep = server->arg(F("beep")) == "1" ? true : false;
    config.reverse_led = server->arg(F("reverse_led")) == "1" ? true : false;
    config.report_interval = server->arg(F("report_interval")).toInt();

#ifdef USE_EXPAND
    if (server->hasArg(F("led_type")))
    {
        config.led_type = server->arg(F("led_type")).toInt();
    }

    if (server->hasArg(F("led_start")) && server->hasArg(F("led_end")))
    {
        config.led_start = server->arg(F("led_start")).toInt();
        config.led_end = server->arg(F("led_end")).toInt();
    }

    if (server->hasArg(F("led_light")))
    {
        config.led_light = server->arg(F("led_light")).toInt();
        ledLight = config.led_light * 10 + 23;
    }
    if (server->hasArg(F("relay_led_time")))
    {
        config.led_time = server->arg(F("relay_led_time")).toInt();
        if (config.led_type == 2 && ledTicker.active())
        {
            ledTicker.detach();
        }
    }
    checkCanLed(true);
#endif

    server->send_P(200, PSTR("application/json"), PSTR("{\"code\":1,\"msg\":\"已经设置。\"}"));
}

void Zinguo::httpHa(ESP8266WebServer *server)
{
    char attachment[100];
    snprintf_P(attachment, sizeof(attachment), PSTR("attachment; filename=%s.yaml"), UID);

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->sendHeader(F("Content-Disposition"), attachment);
    server->send_P(200, PSTR("Content-Type: application/octet-stream"), "");

    String availability = Mqtt::getTeleTopic(F("availability"));
    server->sendContent_P(PSTR("light:\r\n"));
    String tims[] = {F("light"), F("ventilation"), F("close"), F("blow"), F("warm1"), F("warm2")};
    for (size_t i = 0; i < (config.dual_warm ? 6 : 5); i++)
    {
        if (i == 1)
        {
            server->sendContent_P(PSTR("switch:\r\n"));
        }

        snprintf_P(tmpData, sizeof(tmpData),
                   PSTR("  - platform: mqtt\r\n"
                        "    name: \"%s_%s\"\r\n"
                        "    state_topic: \"%s\"\r\n"
                        "    command_topic: \"%s\"\r\n"
                        "    payload_on: \"on\"\r\n"
                        "    payload_off: \"off\"\r\n"
                        "    availability_topic: \"%s\"\r\n"
                        "    payload_available: \"online\"\r\n"
                        "    payload_not_available: \"offline\"\r\n\r\n"),
                   UID, tims[i].c_str(), Mqtt::getStatTopic(tims[i]).c_str(), Mqtt::getCmndTopic(tims[i]).c_str(), availability.c_str());
        server->sendContent_P(tmpData);
    }

    snprintf_P(tmpData, sizeof(tmpData),
               PSTR("sensor:\r\n"
                    "  - platform: mqtt\r\n"
                    "    name: \"%s_temp\"\r\n"
                    "    state_topic: \"%s\"\r\n"
                    "    unit_of_measurement: \"°C\"\r\n\r\n"),
               UID, Mqtt::getStatTopic("temp").c_str());
    server->sendContent_P(tmpData);
}
#pragma endregion

void Zinguo::analysisKey(unsigned short code)
{
    switch (code) //键值处理，对应键位
    {
    case KEY_LIGHT:
    case 0x0400: //key1:照明,Q
        switchLight(!bitRead(controlOut, KEY_LIGHT - 1));
        break;
    case KEY_VENTILATION:
    case 0x0800: //key2:排风,Q
        switchVentilation(!bitRead(controlOut, KEY_VENTILATION - 1));
        break;
    case KEY_CLOSE_ALL:
    case 0x1000: //key3:全关
        switchCloseAll(!bitRead(controlOut, KEY_CLOSE_ALL - 1));
        break;
    case KEY_WARM_2:
    case 0x0100: //key6:取暖2,R
        switchWarm2(!bitRead(controlOut, KEY_WARM_2 - 1));
        break;
    case KEY_BLOW:
    case 0x0200: //key7:吹风,Q
        switchBlow(!bitRead(controlOut, KEY_BLOW - 1));
        break;
    case KEY_WARM_1:
    case 0x0080: //key8:取暖1,R
        switchWarm1(!bitRead(controlOut, KEY_WARM_1 - 1));
        break;
    }
}

// 风铃器，参数为响声次数
void Zinguo::beepBeep(char i)
{
    digitalWrite(PIN_BEEP, HIGH); //风铃器开启
    schTicker->once_ms(70, []()
                       { digitalWrite(PIN_BEEP, LOW); });
}

void Zinguo::dispCtrl() //显示、控制数据的输出
{
    //控制缓冲区：0温度，1按键，2输出端子，3LED缓冲，4输出缓冲
    //第二个595先送数据，第一个595后送数据。
    //数码管十位，刷新数码管温度值
    byte The1st595 = B10000001 | controlPin;                      //8Bit：控制蓝灯(0关/1开),7~3Bit:继电器可控硅，2~1Bit：数码管位
    byte The2nd595 = B10000000 | DigitNUM[int(controlTemp) / 10]; //8Bit：关闭红灯(1关/0开)，7~1Bit：数码管A-F(1关/0开)
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, The2nd595);           //第二个595,
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, The1st595);           //第一个595，控制继电器
    digitalWrite(PIN_LOAD, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_LOAD, LOW);

    //数码管个位，刷新数码管温度值
    The1st595 = B10000010 | controlPin;                      //8Bit：控制蓝灯(0关/1开),7~3Bit:继电器可控硅，2~1Bit：数码管位
    The2nd595 = B10000000 | DigitNUM[int(controlTemp) % 10]; //8Bit：关闭红灯(1关/0开)，7~1Bit：数码管A-F(1关/0开)
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, The2nd595);      //第二个595,
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, The1st595);      //第一个595，控制继电器
    digitalWrite(PIN_LOAD, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_LOAD, LOW);

    if (config.reverse_led)
    {
        //红色LED，待机状态
        The1st595 = B00000000 | controlPin;  //8Bit：控制红灯(1关/0开),7~3Bit:继电器可控硅，2~1Bit：数码管位
        The2nd595 = B10000000 | ~controlLED; //8Bit：控制蓝灯(0关/1开)，7~1Bit：红色LED(1关/0开)
    }
    else
    {
        //蓝色LED，待机状态
        The1st595 = B10000000 | controlPin; //8Bit：控制红灯(1关/0开),7~3Bit:继电器可控硅，2~1Bit：数码管位
        The2nd595 = B00111111 ^ controlLED; //8Bit：控制蓝灯(0关/1开)，7~1Bit：红色LED(1关/0开)
    }
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, The2nd595); //第二个595,
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, The1st595); //第一个595，控制继电器
    digitalWrite(PIN_LOAD, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_LOAD, LOW);

    if (config.reverse_led)
    {
        //蓝色LED，激活状态
        The1st595 = B10000000 | controlPin; //8Bit：控制红灯(1关/0开),7~3Bit:继电器可控硅，2~1Bit：数码管位
        The2nd595 = B00000000 | controlLED; //8Bit：控制蓝灯(0关/1开)，7~1Bit：蓝灯LED(1关/0开)
    }
    else
    {
        //红色LED，激活状态
        The1st595 = B00000000 | controlPin; //8Bit：控制红灯(1关/0开),7~3Bit:继电器可控硅，2~1Bit：数码管位
        The2nd595 = B10000000 | controlLED; //8Bit：控制蓝灯(0关/1开)，7~1Bit：蓝灯LED(1关/0开)
    }
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, The2nd595); //第二个595,
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, The1st595); //第一个595，控制继电器
    digitalWrite(PIN_LOAD, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_LOAD, LOW);
}

static void ICACHE_RAM_ATTR twi_delay(unsigned char v) //对Esp8266进行功能修正,for sc09a
{
    unsigned int i;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
    unsigned int reg;
    for (i = 0; i < v; i++)
    {
        reg = GPI;
    }
    (void)reg;
#pragma GCC diagnostic pop
}

unsigned short Zinguo::getKey() //SC09A芯片iic精简协议模拟,for sc09a
{
    unsigned char bitnum, temp, addr;
    unsigned int key2byte;
    uint8_t bit_temp;
    addr = CON_ADDR;
    key2byte = 0xffff;
    SDA_LOW(); // 拉低SDA 端口送出START 信号
    twi_delay(5);
    for (bitnum = 0; bitnum < 8; bitnum++) //发送8 位地址字节(A[6:0]+RWB)
    {
        SCL_LOW();
        temp = addr & 0x80;
        if (temp == 0x80)
        {
            SDA_HIGH();
        }
        else
        {
            SDA_LOW();
        }
        addr = addr << 1;
        twi_delay(5);
        SCL_HIGH();
        twi_delay(5);
    }
    SDA_HIGH(); //释放SDA 端口,将SDA 设置为输入端口
    SCL_LOW();
    twi_delay(5);
    SCL_HIGH();
    twi_delay(5);
    bit_temp = SDA_READ();
    //读ack 回应
    if (bit_temp)
    {
        return 0;
    }
    for (bitnum = 0; bitnum < 16; bitnum++) //读16 位按键数据字节(D[15:0])
    {
        SCL_LOW();
        twi_delay(5);
        SCL_HIGH();
        twi_delay(5);
        bit_temp = SDA_READ();
        if (bit_temp)
        {
            key2byte = key2byte << 1;
            key2byte = key2byte | 0x01;
        }
        else
        {
            key2byte = key2byte << 1;
        }
    }
    SCL_LOW();
    SDA_HIGH();
    twi_delay(5);
    SCL_HIGH();
    twi_delay(5);
    SCL_LOW();
    SDA_LOW(); //发送NACK 信号
    twi_delay(5);
    SCL_HIGH();
    twi_delay(5);
    SDA_HIGH();                   //释放SDA 端口,将SDA 设置为输入端口
    key2byte = key2byte ^ 0xffff; //取反操作，便于阅读键值
    return key2byte;              //返回按键数据
}

//ADC读取NTC温度转换程序
void Zinguo::convertTemp(void)
{
    float hq = 3.50 * (analogRead(A0)) / 1023;            //计算ADC采样值为电压值
    float x = 225 * hq / (3.50 - hq);                     //计算当前的电阻值
    float hs = log(x / 150);                              //计算NTC对应阻值的对数值
    float temp = 1.0 / (hs / 3453 + 1 / 298.15) - 273.15; //计算当前的温度值，可能的B值=3435、3450、3950、3980、4200

    if (abs(controlTemp - temp) > 0.5f)
    {
        controlTemp = temp; //输出温度值
        Mqtt::publish(Mqtt::getStatTopic(F("temp")), String(temp).c_str(), globalConfig.mqtt.retain);

        if (controlTemp >= config.max_temp)
        {
            switchWarm1(false);
            if (config.dual_warm)
            {
                switchWarm2(false);
            }
        }
    }
}

// 照明 Key1
void Zinguo::switchLight(bool isOn, bool isBeep)
{
    if (bitRead(controlOut, KEY_LIGHT - 1) == isOn)
    {
        return;
    }
    if (isOn)
    {
        controlPin &= ~(1 << 2);
        controlLED |= (1 << 0);
        controlOut |= (1 << 0);
    }
    else
    {
        controlPin |= (1 << 2);
        controlLED &= ~(1 << 0);
        controlOut &= ~(1 << 0);
    }

    if (isBeep && config.beep)
    {
        beepBeep(1);
    }
    Mqtt::publish(Mqtt::getStatTopic(F("light")), isOn ? "on" : "off", globalConfig.mqtt.retain);
#ifdef USE_EXPAND
    if (Zinguo::canLed)
    {
        led(isOn);
    }
#endif
}

// 换气 Key2
void Zinguo::switchVentilation(bool isOn, bool isBeep)
{
    if (bitRead(controlOut, KEY_VENTILATION - 1) == isOn)
    {
        return;
    }
    if (isOn)
    {
        if (!config.dual_motor) // 单电机
        {
            if ((config.linkage == 1 && (bitRead(controlOut, KEY_WARM_1 - 1) || bitRead(controlOut, KEY_WARM_2 - 1))) // 暖1暖2联动  开启时不能开启换气
                || (config.linkage == 2 && bitRead(controlOut, KEY_WARM_1 - 1))                                       // 暖1联动 开启时不能开启换气
                || (config.linkage == 3 && bitRead(controlOut, KEY_WARM_2 - 1))                                       // 暖2联动 开启时不能开启换气
                || closeBlowTime != 127                                                                               // 延时关闭吹风
            )
            {
                if (isBeep && config.beep)
                {
                    beepBeep(2);
                }
                Mqtt::publish(Mqtt::getStatTopic(F("ventilation")), bitRead(controlOut, KEY_VENTILATION - 1) ? "on" : "off", globalConfig.mqtt.retain);
                return;
            }
            switchBlowReal(false, false); // 单电机要关吹风
        }
        controlPin &= ~(1 << 4);
        controlLED |= (1 << 1);
        controlOut |= (1 << 1);
        if (config.close_ventilation > 0)
        {
            ventilationTime = perSecond + (config.close_ventilation * 60); // 通过每秒定时器处理
        }
    }
    else
    {
        controlPin |= (1 << 4);
        controlLED &= ~(1 << 1);
        controlOut &= ~(1 << 1);
        ventilationTime = 0;
    }

    if (isBeep && config.beep)
    {
        beepBeep(1);
    }
    Mqtt::publish(Mqtt::getStatTopic(F("ventilation")), isOn ? "on" : "off", globalConfig.mqtt.retain);
}

// 取暖1 Key8
void Zinguo::switchWarm1(bool isOn, bool isBeep)
{
    if (bitRead(controlOut, KEY_WARM_1 - 1) == isOn)
    {
        return;
    }
    if (isOn)
    {
        if (config.linkage == 1 || config.linkage == 2) // 暖1联动
        {
            switchBlow(true, false);
        }
        controlPin |= (1 << 5);
        controlLED |= (1 << 3);
        controlOut |= (1 << 7);
        closeBlowTime = 127;
        if (warmTime == 0 && config.close_warm > 0)
        {
            warmTime = perSecond + (config.close_warm * 60); // 通过每秒定时器处理
        }
    }
    else
    {
        controlPin &= ~(1 << 5);
        controlLED &= ~(1 << 3);
        controlOut &= ~(1 << 7);
        if ((config.linkage == 1 && (!config.dual_warm || !bitRead(controlOut, KEY_WARM_2 - 1))) || config.linkage == 2)
        {
            closeBlowTime = config.delay_blow;
            if (closeBlowTime == 127)
            {
                switchBlow(false);
            }
        }
        if (!config.dual_warm || !bitRead(controlOut, KEY_WARM_2 - 1))
        {
            warmTime = 0;
        }
    }

    if (isBeep && config.beep)
    {
        beepBeep(1);
    }
    Mqtt::publish(Mqtt::getStatTopic(F("warm1")), isOn ? "on" : "off", globalConfig.mqtt.retain);
}

// 取暖2 Key6
void Zinguo::switchWarm2(bool isOn, bool isBeep)
{
    if (bitRead(controlOut, KEY_WARM_2 - 1) == isOn)
    {
        return;
    }
    if (isOn)
    {
        if (config.dual_warm)
        {
            if (config.linkage == 1 || config.linkage == 3) // 暖2联动
            {
                switchBlow(true, false);
            }
            closeBlowTime = 127;
            if (warmTime == 0 && config.close_warm > 0)
            {
                warmTime = perSecond + (config.close_warm * 60); // 通过每秒定时器处理
            }
        }
        controlPin |= (1 << 6);
        controlLED |= (1 << 5);
        controlOut |= (1 << 5);
    }
    else
    {
        controlPin &= ~(1 << 6);
        controlLED &= ~(1 << 5);
        controlOut &= ~(1 << 5);
        if (config.dual_warm)
        {
            if ((config.linkage == 1 && !bitRead(controlOut, KEY_WARM_1 - 1)) || config.linkage == 3)
            {
                closeBlowTime = config.delay_blow;
                if (closeBlowTime == 127)
                {
                    switchBlow(false);
                }
            }
            if (!bitRead(controlOut, KEY_WARM_1 - 1))
            {
                warmTime = 0;
            }
        }
    }

    if (isBeep && config.beep)
    {
        beepBeep(1);
    }

    Mqtt::publish(Mqtt::getStatTopic(F("warm2")), isOn ? "on" : "off", globalConfig.mqtt.retain);
}

// 吹风 Key7
void Zinguo::switchBlowReal(bool isOn, bool isBeep)
{
    if (bitRead(controlOut, KEY_BLOW - 1) == isOn)
    {
        return;
    }
    if (isOn)
    {
        if (!config.dual_motor) // 单电机
        {
            switchVentilation(false, false);
        }
        controlPin &= ~(1 << 3);
        controlLED |= (1 << 4);
        controlOut |= (1 << 6);
    }
    else
    {
        controlPin |= (1 << 3);
        controlLED &= ~(1 << 4);
        controlOut &= ~(1 << 6);
    }

    if (isBeep && config.beep)
    {
        beepBeep(1);
    }
    Mqtt::publish(Mqtt::getStatTopic(F("blow")), isOn ? "on" : "off", globalConfig.mqtt.retain);
}

void Zinguo::switchBlow(bool isOn, bool isBeep)
{
    if (isOn)
    {
        switchBlowReal(isOn, isBeep);
    }
    else
    {
        if (closeBlowTime != 127)
        {
            return;
        }
        if (config.linkage == 1 || config.linkage == 2)
        {
            switchWarm1(false, false);
        }
        if (config.dual_warm && (config.linkage == 1 || config.linkage == 3))
        {
            switchWarm2(false, false);
        }
        if (config.linkage == 0 || closeBlowTime == 127)
        {
            switchBlowReal(isOn, false);
        }
        if (isBeep && config.beep)
        {
            beepBeep(1);
        }
    }
}

// 全关 Key3
void Zinguo::switchCloseAll(bool isOn, bool isBeep)
{
    if (isOn)
    {
        controlLED |= (1 << 2);
        controlOut |= (1 << 2);
    }
    else
    {
        controlLED &= ~(1 << 2);
        controlOut &= ~(1 << 2);
    }
#ifdef SkyNet
    Mqtt::publish("cmnd/rsq/POWER", isOn ? "on" : "off", globalConfig.mqtt.retain);
#else
    dispCtrl();
    Mqtt::publish(Mqtt::getStatTopic(F("close")), isOn ? "on" : "off", globalConfig.mqtt.retain);
    switchLight(false, false);
    switchVentilation(false, false);
    switchBlow(false, false);
    switchWarm1(false, false);
    switchWarm2(false, false);
#endif
    if (isBeep && config.beep)
    {
        beepBeep(1);
    }
}

void Zinguo::reportPower()
{
    Mqtt::publish(Mqtt::getStatTopic(F("temp")), String(controlTemp).c_str(), globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic(F("light")), bitRead(controlOut, KEY_LIGHT - 1) ? "on" : "off", globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic(F("ventilation")), bitRead(controlOut, KEY_VENTILATION - 1) ? "on" : "off", globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic(F("warm1")), bitRead(controlOut, KEY_WARM_1 - 1) ? "on" : "off", globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic(F("warm2")), bitRead(controlOut, KEY_WARM_2 - 1) ? "on" : "off", globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic(F("blow")), bitRead(controlOut, KEY_BLOW - 1) ? "on" : "off", globalConfig.mqtt.retain);
}

#ifdef USE_EXPAND

void Zinguo::cheackButton()
{
    bool currentState = digitalRead(BUTTON_IO);
    if (currentState != ((buttonStateFlag2 & UNSTABLE_STATE) != 0))
    {
        buttonTimingStart2 = millis();
        buttonStateFlag2 ^= UNSTABLE_STATE;
    }
    else if (millis() - buttonTimingStart2 >= BUTTON_DEBOUNCE_TIME)
    {
        if (currentState != ((buttonStateFlag2 & DEBOUNCED_STATE) != 0))
        {
            buttonTimingStart2 = millis();
            buttonStateFlag2 ^= DEBOUNCED_STATE;

            switchCount2 += 1;
            buttonIntervalStart2 = millis();

            if (!currentState)
            {
                if (!bitRead(controlOut, KEY_LIGHT - 1))
                {
                    switchLight(true);
                }
                else
                {
                    last2 = 1;
                }
            }
            else
            {
                if (last2 == 1)
                {
                    switchLight(false);
                    last2 = 0;
                }
            }
        }
    }

    // 如果经过的时间大于超时并且计数大于0，则填充并重置计数
    if (switchCount2 > 0 && (millis() - buttonIntervalStart2) > specialFunctionTimeout2)
    {
        Led::led(200);
        Log::Info(PSTR("switchCount: %d"), switchCount2);
        if (switchCount2 == 20)
        {
            WifiMgr::setupWifiManager(false);
        }
        switchCount2 = 0;
    }
}

#pragma region Led

void Zinguo::ledTickerHandle()
{
    if (!bitRead(controlOut, KEY_LIGHT - 1))
    {
        analogWrite(LED_IO, ledLevel);
    }
    if (ledUp)
    {
        ledLevel++;
        if (ledLevel >= ledLight)
        {
            ledUp = false;
        }
    }
    else
    {
        ledLevel--;
        if (ledLevel <= 50)
        {
            ledUp = true;
        }
    }
}

void Zinguo::ledPWM(bool isOn)
{
    if (isOn)
    {
        analogWrite(LED_IO, 0);
        if (ledTicker.active())
        {
            ledTicker.detach();
            // Log::Info(PSTR("ledTicker detach"));
        }
    }
    else
    {
        if (!ledTicker.active())
        {
            ledTicker.attach_ms(config.led_time, []()
                                { ((Zinguo *)module)->ledTickerHandle(); });
            // Log::Info(PSTR("ledTicker active"));
        }
    }
}

void Zinguo::led(bool isOn)
{
    if (config.led_type == 0)
    {
        return;
    }

    if (config.led_type == 1)
    {
        analogWrite(LED_IO, isOn ? 0 : ledLight);
    }
    else if (config.led_type == 2)
    {
        ledPWM(isOn);
    }
}

bool Zinguo::checkCanLed(bool re)
{
    bool result;
    if (config.led_start != config.led_end && Rtc::rtcTime.valid)
    {
        uint16_t nowTime = Rtc::rtcTime.hour * 100 + Rtc::rtcTime.minute;
        if (config.led_start > config.led_end) // 开始时间大于结束时间 跨日
        {
            result = (nowTime >= config.led_start || nowTime < config.led_end);
        }
        else
        {
            result = (nowTime >= config.led_start && nowTime < config.led_end);
        }
    }
    else
    {
        result = true; // 没有正确时间为一直亮
    }
    if (result != Zinguo::canLed || re)
    {
        if ((!result || config.led_type != 2) && ledTicker.active())
        {
            ledTicker.detach();
            // Log::Info(PSTR("ledTicker detach2"));
        }
        Zinguo::canLed = result;
        Log::Info(result ? PSTR("led can light") : PSTR("led can not light"));

        result &&config.led_type != 0 ? led(bitRead(controlOut, KEY_LIGHT - 1)) : analogWrite(LED_IO, 0);
    }

    return result;
}
#pragma endregion

#endif