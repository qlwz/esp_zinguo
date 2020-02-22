#include "Zinguo.h"

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
}

void Zinguo::loop()
{
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
                    Debug::AddInfo("TouchKey: 0x%0X", key);
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
            Wifi::setupWifiManager(false);
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
            Mqtt::publish(Mqtt::getStatTopic("close"), "OFF", globalConfig.mqtt.retain);
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
            Debug::AddInfo("Ventilation Timeout %d %d", ventilationTime, perSecond);
            ventilationTime = 0;
            switchVentilation(false);
        }

        if (warmTime != 0 && warmTime <= perSecond)
        {
            Debug::AddInfo("Warm Timeout %d %d", warmTime, perSecond);
            warmTime = 0;
            switchWarm1(false);
            switchWarm2(false);
        }

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
}

void Zinguo::resetConfig()
{
    Debug::AddInfo(PSTR("moduleResetConfig . . . OK"));
    memset(&config, 0, sizeof(ZinguoConfigMessage));
    config.dual_motor = true;
    config.dual_warm = true;
    config.delay_blow = 30;
    config.linkage = 1;
    config.max_temp = 40;
    config.close_warm = 30;
    config.close_ventilation = 30;
    config.beep = true;
}

void Zinguo::saveConfig(bool isEverySecond)
{
    Config::moduleSaveConfig(MODULE_CFG_VERSION, ZinguoConfigMessage_size, ZinguoConfigMessage_fields, &config);
}
#pragma endregion

#pragma region MQTT

void Zinguo::mqttCallback(String topicStr, String str)
{
    if (topicStr.endsWith("/light"))
    {
        switchLight(str == "ON" ? true : (str == "OFF" ? false : !bitRead(controlOut, KEY_LIGHT - 1)));
    }
    else if (topicStr.endsWith("/ventilation"))
    {
        switchVentilation(str == "ON" ? true : (str == "OFF" ? false : !bitRead(controlOut, KEY_VENTILATION - 1)));
    }
    else if (topicStr.endsWith("/close"))
    {
        switchCloseAll(str == "ON" ? true : (str == "OFF" ? false : !bitRead(controlOut, KEY_VENTILATION - 1)));
    }
    else if (topicStr.endsWith("/warm2"))
    {
        switchWarm2(str == "ON" ? true : (str == "OFF" ? false : !bitRead(controlOut, KEY_WARM_2 - 1)));
    }
    else if (topicStr.endsWith("/blow"))
    {
        switchBlow(str == "ON" ? true : (str == "OFF" ? false : !bitRead(controlOut, KEY_BLOW - 1)));
    }
    else if (topicStr.endsWith("/warm1"))
    {
        switchWarm1(str == "ON" ? true : (str == "OFF" ? false : !bitRead(controlOut, KEY_WARM_1 - 1)));
    }
    else if (topicStr.endsWith("/temp"))
    {
        mqttTemp = true;
        controlTemp = str.toFloat();
        Mqtt::publish(Mqtt::getStatTopic("temp"), str.c_str(), globalConfig.mqtt.retain);
        if (controlTemp >= config.max_temp)
        {
            switchWarm1(false);
            switchWarm2(false);
        }
    }
    else if (topicStr.endsWith("/report"))
    {
        reportPower();
    }
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

    String tims[] = {"light", "ventilation", "close", "blow", "warm1", "warm2"};

    for (size_t i = 0; i < (config.dual_warm ? 6 : 5); i++)
    {
        sprintf(topic, "%s/%s/%s_%s/config", globalConfig.mqtt.discovery_prefix, tims[i] == "light" ? "light" : "switch", UID, tims[i].c_str());
        if (isEnable)
        {
            sprintf(message, HASS_DISCOVER_ZINGUO, UID, tims[i].c_str(),
                    Mqtt::getCmndTopic(tims[i]).c_str(),
                    Mqtt::getStatTopic(tims[i]).c_str(),
                    Mqtt::getTeleTopic(F("availability")).c_str());
            //Debug::AddInfo(PSTR("discovery: %s - %s"), topic, message);
            Mqtt::publish(topic, message, true);
        }
        else
        {
            Mqtt::publish(topic, "", true);
        }
    }

    sprintf(topic, "%s/sensor/%s_temp/config", globalConfig.mqtt.discovery_prefix, UID);
    if (isEnable)
    {
        sprintf(message, "{\"name\":\"%s_temp\",\"stat_t\":\"%s\",\"unit_of_meas\":\"°C\"}", UID, Mqtt::getStatTopic("temp").c_str());
        //Debug::AddInfo(PSTR("discovery: %s - %s"), topic, message);
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
    String radioJs = F("<script type='text/javascript'>");
    radioJs += F("function setDataSub(data,key){if(key.substr(0,6)=='zinguo'){id(key).setAttribute('class',data[key]==1?'btn-success':'btn-info');return true}return false}");
    String page = F("<table class='gridtable'><thead><tr><th colspan='2'>控制浴霸</th></tr></thead><tbody>");
    page += F("<tr colspan='2' style='text-align: center'><td>");
    page += F("<button type='button' style='width:56px' onclick=\"ajaxPost('/zinguo_do', 'key=8');\" id='zinguo_warm1' class='btn-success'>风暖1</button>");
    page += F(" <button type='button' style='width:50px' onclick=\"ajaxPost('/zinguo_do', 'key=7');\" id='zinguo_blow' class='btn-success'>吹风</button>");
    page += F(" <button type='button' style='width:56px' onclick=\"ajaxPost('/zinguo_do', 'key=6');\" id='zinguo_warm2' class='btn-success'>风暖2</button>");
    page += F(" <button type='button' style='width:50px' onclick=\"ajaxPost('/zinguo_do', 'key=1');\" id='zinguo_light' class='btn-success'>照明</button>");
    page += F(" <button type='button' style='width:50px' onclick=\"ajaxPost('/zinguo_do', 'key=2');\" id='zinguo_ventilation' class='btn-success'>换气</button>");
    page += F(" <button type='button' style='width:50px' onclick=\"ajaxPost('/zinguo_do', 'key=3');\" id='zinguo_close' class='btn-info'>全关</button>");
    page += F("</td></tr></tbody></table>");
    if (!bitRead(controlOut, KEY_LIGHT - 1))
    {
        radioJs += F("id('zinguo_light').setAttribute('class', 'btn-info');");
    }
    if (!bitRead(controlOut, KEY_VENTILATION - 1))
    {
        radioJs += F("id('zinguo_ventilation').setAttribute('class', 'btn-info');");
    }
    if (!bitRead(controlOut, KEY_WARM_1 - 1))
    {
        radioJs += F("id('zinguo_warm1').setAttribute('class', 'btn-info');");
    }
    if (!bitRead(controlOut, KEY_WARM_2 - 1))
    {
        radioJs += F("id('zinguo_warm2').setAttribute('class', 'btn-info');");
    }
    if (!bitRead(controlOut, KEY_BLOW - 1))
    {
        radioJs += F("id('zinguo_blow').setAttribute('class', 'btn-info');");
    }

    page += F("<form method='post' action='/zinguo_setting' onsubmit='postform(this);return false'>");
    page += F("<table class='gridtable'><thead><tr><th colspan='2'>浴霸设置</th></tr></thead><tbody>");
    page += F("<tr><td>电机数量</td><td>");
    page += F("<label class='bui-radios-label'><input type='radio' name='dual_motor' value='0'/><i class='bui-radios'></i> 单电机</label>&nbsp;&nbsp;&nbsp;&nbsp;");
    page += F("<label class='bui-radios-label'><input type='radio' name='dual_motor' value='1'/><i class='bui-radios'></i> 双电机</label>");
    page += F("</td></tr>");
    radioJs += F("setRadioValue('dual_motor', '{v}');");
    radioJs.replace(F("{v}"), config.dual_motor ? "1" : "0");

    page += F("<tr><td>风暖数量</td><td>");
    page += F("<label class='bui-radios-label'><input type='radio' name='dual_warm' value='0'/><i class='bui-radios'></i> 单风暖</label>&nbsp;&nbsp;&nbsp;&nbsp;");
    page += F("<label class='bui-radios-label'><input type='radio' name='dual_warm' value='1'/><i class='bui-radios'></i> 双风暖</label>");
    page += F("</td></tr>");
    radioJs += F("setRadioValue('dual_warm', '{v}');");
    radioJs.replace(F("{v}"), config.dual_warm ? "1" : "0");

    page += F("<tr><td>吹风联动</td><td>");
    page += F("<label class='bui-radios-label'><input type='radio' name='linkage' value='0'/><i class='bui-radios'></i> 不联动</label><br/>");
    page += F("<label class='bui-radios-label'><input type='radio' name='linkage' value='1'/><i class='bui-radios'></i> 暖1或暖2联动</label><br/>");
    page += F("<label class='bui-radios-label'><input type='radio' name='linkage' value='2'/><i class='bui-radios'></i> 暖1联动</label><br/>");
    page += F("<label class='bui-radios-label'><input type='radio' name='linkage' value='3'/><i class='bui-radios'></i> 暖2联动</label>");
    page += F("</td></tr>");
    radioJs += F("setRadioValue('linkage', '{v}');");
    radioJs.replace(F("{v}"), String(config.linkage));

    page += F("<tr><td>吹风延时</td><td><input type='number' min='0' max='90' name='delay_blow' required value='{delay_blow}'>&nbsp;秒</td></tr>");
    page.replace(F("{delay_blow}"), config.delay_blow == 127 ? "0" : String(config.delay_blow));

    page += F("<tr><td>过温保护</td><td><input type='number' min='15' max='45' name='max_temp' required value='{max_temp}'>&nbsp;度</td></tr>");
    page.replace(F("{max_temp}"), String(config.max_temp));

    page += F("<tr><td>取暖定时关闭</td><td><input type='number' min='1' max='90' name='close_warm' required value='{close_warm}'>&nbsp;分钟</td></tr>");
    page.replace(F("{close_warm}"), String(config.close_warm));

    page += F("<tr><td>换气定时关闭</td><td><input type='number' min='1' max='90' name='close_ventilation' required value='{close_ventilation}'>&nbsp;分钟</td></tr>");
    page.replace(F("{close_ventilation}"), String(config.close_ventilation));

    page += F("<tr><td>按键声音</td><td>");
    page += F("<label class='bui-radios-label'><input type='radio' name='beep' value='0'/><i class='bui-radios'></i> 关闭</label>&nbsp;&nbsp;&nbsp;&nbsp;");
    page += F("<label class='bui-radios-label'><input type='radio' name='beep' value='1'/><i class='bui-radios'></i> 开启</label>");
    page += F("</td></tr>");
    radioJs += F("setRadioValue('beep', '{v}');");
    radioJs.replace(F("{v}"), config.beep ? "1" : "0");

    page += F("<tr><td>LED颜色</td><td>");
    page += F("<label class='bui-radios-label'><input type='radio' name='reverse_led' value='0'/><i class='bui-radios'></i> 待机蓝色</label>&nbsp;&nbsp;&nbsp;&nbsp;");
    page += F("<label class='bui-radios-label'><input type='radio' name='reverse_led' value='1'/><i class='bui-radios'></i> 待机红色</label>");
    page += F("</td></tr>");
    radioJs += F("setRadioValue('reverse_led', '{v}');");
    radioJs.replace(F("{v}"), config.reverse_led ? "1" : "0");

    page += F("<tr><td>主动上报间隔</td><td><input type='number' min='0' max='3600' name='report_interval' required value='{v}'>&nbsp;秒，0关闭</td></tr>");
    page.replace(F("{v}"), String(config.report_interval));

    page += F("<tr><td colspan='2'><button type='submit' class='btn-info'>设置</button><br>");
    page += F("<button type='button' class='btn-success' style='margin-top:10px' onclick='window.location.href=\"/ha\"'>下载HA配置文件</button></td></tr>");
    page += F("</tbody></table></form>");
    radioJs += F("</script>");

    server->sendContent(page);
    server->sendContent(radioJs);
}

void Zinguo::httpDo(ESP8266WebServer *server)
{
    analysisKey(server->arg(F("key")).toInt());

    server->send(200, F("text/html"), "{\"code\":1,\"msg\":\"操作成功\",\"data\":{" + httpGetStatus(server) + "}}");
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

    server->send(200, F("text/html"), F("{\"code\":1,\"msg\":\"已经设置。\"}"));
}

void Zinguo::httpHa(ESP8266WebServer *server)
{
    char attachment[100];
    snprintf_P(attachment, sizeof(attachment), PSTR("attachment; filename=%s.yaml"), UID);

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->sendHeader(F("Content-Disposition"), attachment);
    server->send(200, F("Content-Type: application/octet-stream"), "");

    String availability = Mqtt::getTeleTopic(F("availability"));
    server->sendContent(F("light:\r\n"));
    String tims[] = {"light", "ventilation", "close", "blow", "warm1", "warm2"};
    for (size_t i = 0; i < (config.dual_warm ? 6 : 5); i++)
    {
        if (i == 1)
        {
            server->sendContent(F("switch:\r\n"));
        }
        server->sendContent(F("  - platform: mqtt\r\n    name: \""));
        server->sendContent(UID);
        server->sendContent(F("_"));
        server->sendContent(tims[i]);
        server->sendContent(F("\"\r\n    state_topic: \""));
        server->sendContent(Mqtt::getStatTopic(tims[i]));
        server->sendContent(F("\"\r\n    command_topic: \""));
        server->sendContent(Mqtt::getCmndTopic(tims[i]));
        server->sendContent(F("\"\r\n    payload_on: \"ON\"\r\n    payload_off: \"OFF\"\r\n    availability_topic: \""));
        server->sendContent(availability);
        server->sendContent(F("\"\r\n    payload_available: \"online\"\r\n    payload_not_available: \"offline\"\r\n\r\n"));
    }

    server->sendContent(F("sensor:\r\n"));
    server->sendContent(F("  - platform: mqtt\r\n    name: \""));
    server->sendContent(UID);
    server->sendContent(F("_temp"));
    server->sendContent(F("\"\r\n    state_topic: \""));
    server->sendContent(Mqtt::getStatTopic("temp").c_str());
    server->sendContent(F("\"\r\n    unit_of_measurement: \"°C\"\r\n\r\n"));
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
    schTicker->once_ms(70, []() { digitalWrite(PIN_BEEP, LOW); });
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
        Mqtt::publish(Mqtt::getStatTopic("temp"), String(temp).c_str(), globalConfig.mqtt.retain);

        if (controlTemp >= config.max_temp)
        {
            switchWarm1(false);
            switchWarm2(false);
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
    Mqtt::publish(Mqtt::getStatTopic("light"), isOn ? "ON" : "OFF", globalConfig.mqtt.retain);
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
                Mqtt::publish(Mqtt::getStatTopic("ventilation"), bitRead(controlOut, KEY_VENTILATION - 1) ? "ON" : "OFF", globalConfig.mqtt.retain);
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
    Mqtt::publish(Mqtt::getStatTopic("ventilation"), isOn ? "ON" : "OFF", globalConfig.mqtt.retain);
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
        if ((config.linkage == 1 && !bitRead(controlOut, KEY_WARM_2 - 1)) || config.linkage == 2)
        {
            closeBlowTime = config.delay_blow;
            if (closeBlowTime == 127)
            {
                switchBlow(false);
            }
        }
        if (!bitRead(controlOut, KEY_WARM_2 - 1))
        {
            warmTime = 0;
        }
    }

    if (isBeep && config.beep)
    {
        beepBeep(1);
    }
    Mqtt::publish(Mqtt::getStatTopic("warm1"), isOn ? "ON" : "OFF", globalConfig.mqtt.retain);
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
        if (config.linkage == 1 || config.linkage == 3) // 暖2联动
        {
            switchBlow(true, false);
        }
        controlPin |= (1 << 6);
        controlLED |= (1 << 5);
        controlOut |= (1 << 5);
        closeBlowTime = 127;
        if (warmTime == 0 && config.close_warm > 0)
        {
            warmTime = perSecond + (config.close_warm * 60); // 通过每秒定时器处理
        }
    }
    else
    {
        controlPin &= ~(1 << 6);
        controlLED &= ~(1 << 5);
        controlOut &= ~(1 << 5);
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

    if (isBeep && config.beep)
    {
        beepBeep(1);
    }

    // 单风暖的时候 控制风暖1
    if (!config.dual_warm)
    {
        switchWarm1(isOn, false);
        return;
    }

    Mqtt::publish(Mqtt::getStatTopic("warm2"), isOn ? "ON" : "OFF", globalConfig.mqtt.retain);
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
    Mqtt::publish(Mqtt::getStatTopic("blow"), isOn ? "ON" : "OFF", globalConfig.mqtt.retain);
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
        if ((config.linkage == 1 || config.linkage == 3))
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
    Mqtt::publish("cmnd/rsq/POWER", isOn ? "ON" : "OFF", globalConfig.mqtt.retain);
#else
    dispCtrl();
    Mqtt::publish(Mqtt::getStatTopic("close"), isOn ? "ON" : "OFF", globalConfig.mqtt.retain);
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
    Mqtt::publish(Mqtt::getStatTopic("temp"), String(controlTemp).c_str(), globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic("light"), bitRead(controlOut, KEY_LIGHT - 1) ? "ON" : "OFF", globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic("ventilation"), bitRead(controlOut, KEY_VENTILATION - 1) ? "ON" : "OFF", globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic("warm1"), bitRead(controlOut, KEY_WARM_1 - 1) ? "ON" : "OFF", globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic("warm2"), bitRead(controlOut, KEY_WARM_2 - 1) ? "ON" : "OFF", globalConfig.mqtt.retain);
    Mqtt::publish(Mqtt::getStatTopic("blow"), bitRead(controlOut, KEY_BLOW - 1) ? "ON" : "OFF", globalConfig.mqtt.retain);
}