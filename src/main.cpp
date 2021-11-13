#include <Arduino.h>
#include <LiquidCrystal.h>
#include <WiFi.h>
#include <DHT.h>

#define LCD_BACKLIGHT_PIN 2
#define BACKLIGHT_TIME 5000 // ms
#define BUTTON_PIN 12
#define AP_SSID "Guest"
#define AP_PSK "p4ssw0rd"
#define HOSTNAME "htsensoresp32"
#define DHTPIN 13
#define DHTTYPE DHT22

//                RS  EN   4   5   6   7
LiquidCrystal lcd(14, 27, 26, 25, 33, 32);
WiFiServer server(8000);
DHT dht(DHTPIN, DHTTYPE);

// Custom "°" character for temperature display
byte degrees[8] = {B00100, B01010, B00100, B00000, B00000, B00000, B00000};
int screen_page;
float temperature;
float humidity;
TimerHandle_t xBacklightTimer = NULL;

void lcd_off() {
    lcd.clear();
    digitalWrite(LCD_BACKLIGHT_PIN, LOW);
    screen_page = -1;
}

void lcd_off_callback(TimerHandle_t xTimer) {
    lcd_off();
}

void refresh_display(int page) {
    // Loop back to first page if needed
    screen_page = page % 2;
    lcd.clear();
    lcd.setCursor(0, 0);
    digitalWrite(LCD_BACKLIGHT_PIN, HIGH);
    switch (screen_page) {
        case 0:
            lcd.printf("Temp:   %5.2f %cC", temperature, 0);
            lcd.setCursor(0, 1);
            lcd.printf("Humi:   %5.2f %%", humidity);
            break;

        case 1:
            lcd.printf(WiFi.status() == WL_CONNECTED ? "Connected!" : "Offline...");
            lcd.setCursor(0, 1);
            lcd.print(WiFi.localIP());
            break;
    }
}

void scan_button(void * params) {
    for (;;) {
        if (digitalRead(BUTTON_PIN) == LOW) {
            refresh_display(++screen_page);
            xTimerReset(xBacklightTimer, 10);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

void query_sensor(void * params) {
    for (;;) {
        float temperature_reading = dht.readTemperature();
        float humidity_reading = dht.readHumidity();
        if (isnan(temperature_reading) || isnan(humidity_reading)) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            temperature = temperature_reading;
            humidity = humidity_reading;
            vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }
}

void setup() {
    // Initialise pins
    pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
    digitalWrite(LCD_BACKLIGHT_PIN, HIGH);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    // Initialise peripherals
    dht.begin();
    lcd.createChar(0, degrees);
    lcd.begin(16, 2);
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(AP_SSID, AP_PSK);

    lcd.print("Connecting...");
    while (WiFi.status() != WL_CONNECTED) {
        if (WiFi.status() == WL_NO_SSID_AVAIL) {
            lcd.clear();
            lcd.print("Cannot connect!");
            for (;;) {}
        }
        delay(500);
    }

    // Display network status
    refresh_display(1);
    server.begin();

    // Run the tasks updating temp/humi and button state on core 0
    xTaskCreatePinnedToCore(query_sensor, "query_sensor", 4096, NULL, 0, NULL, 0);
    xTaskCreatePinnedToCore(scan_button, "scan_button", 4096, NULL, 0, NULL, 0);

    // Create a timer allowing the LCD screen to turn off after a few seconds
    xBacklightTimer = xTimerCreate("lcd_off", pdMS_TO_TICKS(BACKLIGHT_TIME), pdFALSE, NULL, lcd_off_callback);
    xTimerReset(xBacklightTimer, 10);
}

void loop() {
    WiFiClient client = server.available();
    if (client) {
        bool currentLineIsBlank = true;
        String header;
        while (client.connected()) {
            if (client.available()) {
                char c = client.read();
                header += c;
                if (c == '\n' && currentLineIsBlank) {
                    if (header.indexOf("GET / HTTP") >= 0) {
                        client.println("HTTP/1.1 200 OK");
                        client.println("Content-Type: text/plain; version=0.0.4; charset=utf-8");
                        client.println();
                        client.printf(
                            "# HELP room_temperature_celsius Room temperature\n"
                            "# TYPE room_temperature_celsius gauge\n"
                            "room_temperature_celsius{instance=\"%s\",job=\"air-quality\"} %f\n"
                            "# HELP room_humidity_ratio Room humidity\n"
                            "# TYPE room_humidity_ratio gauge\n"
                            "room_humidity_ratio{instance=\"%s\",job=\"air-quality\"} %f\n"
                            , HOSTNAME, temperature, HOSTNAME, humidity
                        );
                    } else {
                        client.println("HTTP/1.1 404 Not Found");
                        client.println("Content-type: text/html");
                        client.println("Connection: close");
                        client.println();
                    }
                    client.printf("\n");
                    break;
                }

                if (c == '\n') {
                    currentLineIsBlank = true;
                } else if (c != '\r') {
                    currentLineIsBlank = false;
                }
            }
        }
        delay(10);
        client.stop();
    }
}