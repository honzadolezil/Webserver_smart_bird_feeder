/*********
  ESP32-CAM with BME280 weather sensor and Flask server for wifi comunication
  Integrated PIR sensor and limit on how many photos can be taken per minute
*********/

#include "Arduino.h"
#include "soc/soc.h" //disable brownout problems
#include "soc/rtc_cntl_reg.h"  //disable brownout problems - power sensing disabled (using external regulator)
#include <WiFi.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>

// Include BME280 libraries FIRST to avoid sensor_t conflict - workaround
// sensor_t is defined in both Adafruit BME280 and esp_camera libraries.. but different data structures
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// Then include camera with redefined "sensor_t"... im not using the structre anyway
#define sensor_t camera_sensor_t
#include "esp_camera.h"
#include "esp_http_server.h"
#undef sensor_t

// My wifi settings
const char* ssid = "H1";
const char* password = "ahoj1234";

// Flask server address - wehre it uploads photos and weather json data
const char* flask_server = "http://192.168.244.100:5000/upload";
const char* weather_server = "http://192.168.244.100:5000/weather";

Adafruit_BME280 bme;
bool bme280_found = false;

#define WEATHER_PER_MINUTE 1 // how many weather measurements per mintue

// timing limits
unsigned long lastWeatherSend = 0; //caching the time
const unsigned long weatherInterval = 60000/WEATHER_PER_MINUTE; 

// BME280 I2C pins 
#define I2C_SDA 14
#define I2C_SCL 15

// PIR sensor pin
#define PIR_PIN 13

// PIR variables
int pirState = LOW;             // current PIR state
int val = 0;                    // PIR status
unsigned long lastMotionTime = 0;
unsigned long motionDebounceTime = 2000;  // debounce time - prevent multiple captures
unsigned long motionPhotoCount = 0;       
unsigned long lastMotionMinute = 0;      // tracking minute for photo count reset
const int maxPhotosPerMinute = 5;        // max photos per minute

// camera pins
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22


httpd_handle_t camera_httpd = NULL;


// Function for capturing photos and uploading to Flask server. activated by GET request to /capture 
// this is for the manual capcuring
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL; // pointer to frame buff
  esp_err_t res = ESP_OK;
  bool uploadSuccess = false;
    
  // Clear old frames from buffer - memory hungry
  fb = esp_camera_fb_get();
  if (fb) {
    esp_camera_fb_return(fb);
  }
   // ensure camera ready 
  delay(200);
  

  fb = esp_camera_fb_get(); // captures to frame buffer
  if (!fb) {// failure to capture
    httpd_resp_send_500(req); //sends 500 Internal Server Error to the flask
    return ESP_FAIL;
  }
  

  // Upload to Flask server FIRST, then respond to client
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(flask_server); // connects to flask server
    http.addHeader("Content-Type", "image/jpeg"); // Head so the server knows what is recieving
    http.setTimeout(10000); // 10 second timeout
    
    int httpResponseCode = http.POST(fb->buf, fb->len); // uploads the image buffer to Flask server
    
    if (httpResponseCode > 0) {
      uploadSuccess = true;
    }
    http.end();
  
  
  // Now send response to client based on upload result
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); // can make request from any origin

  // messages to client serverside - veiw them on espipadress/capture
  if (uploadSuccess) {
    httpd_resp_send(req, "Image captured and uploaded", HTTPD_RESP_USE_STRLEN);
  } else {
    httpd_resp_send(req, "Image captured but upload failed", HTTPD_RESP_USE_STRLEN);
  }
  
  // Return the frame buffer
  esp_camera_fb_return(fb);
  return res;
 }
}
// Function for handling weather data requests - manual one
static esp_err_t weather_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json"); // response will be JSON format
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"); // can make request from any origin
  
  if (!bme280_found) { // if the bmw280 isnt set up - exit and tell the server
    httpd_resp_send(req, "{\"error\":\"BME280 sensor not found\"}", HTTPD_RESP_USE_STRLEN); //sends string on the server
    return ESP_OK;
  }
  
  //  BME280 to take a reading force
  bme.takeForcedMeasurement();
  
  // data sensor read
  float temperature = bme.readTemperature();
  float humidity = bme.readHumidity();
  float pressure = bme.readPressure() / 100.0F; // converting to hpa
  
  // Check if readings are valid
  if (isnan(temperature) || isnan(humidity) || isnan(pressure)) { 
    httpd_resp_send(req, "{\"error\":\"Failed to read sensor data\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;// check for numbers 
  }
  
  // Json rsponse
  char json_response[200];
  snprintf(json_response, sizeof(json_response), 
    "{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"timestamp\":%lu}",
    temperature, humidity, pressure, millis()); // printing int buffewr all the data
  
  // again sending data to server...
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(weather_server);
    http.addHeader("Content-Type", "application/json");
    
    int httpResponseCode = http.POST(json_response);
    http.end();
  }
  
  httpd_resp_send(req, json_response, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}
// Function to send weather data automatically every time interval
void sendWeatherDataAuto() {
  if (!bme280_found) return;
  
  bme.takeForcedMeasurement();
  
  float temperature = bme.readTemperature();
  float humidity = bme.readHumidity();
  float pressure = bme.readPressure() / 100.0F; 

  if (isnan(temperature) || isnan(humidity) || isnan(pressure)) {
    Serial.println("Failed to read from BME280 sensor!");
    return;
  }
    
  // Send to Flask server
  HTTPClient http;
  if (WiFi.status() == WL_CONNECTED) {
    http.begin(weather_server);
    http.addHeader("Content-Type", "application/json");
    
    String json_data = "{\"temperature\":" + String(temperature) + 
                      ",\"humidity\":" + String(humidity) + 
                      ",\"pressure\":" + String(pressure) + "}";
    
    int httpResponseCode = http.POST(json_data);
    http.end();
  }
}
// camera server start
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  
  // reduce memory usage
  config.max_open_sockets = 3;
  config.max_uri_handlers = 3;
  config.max_resp_headers = 4;
  config.backlog_conn = 2;
  
  httpd_uri_t capture_uri = {
    .uri       = "/capture",
    .method    = HTTP_GET,
    .handler   = capture_handler,
    .user_ctx  = NULL
  };//handler for capturing photos req
  
  httpd_uri_t weather_uri = {
    .uri       = "/weather",
    .method    = HTTP_GET,
    .handler   = weather_handler,
    .user_ctx  = NULL
  };// handler for weather data req
  
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &weather_uri);
  }
}

// Main setup function 
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout 
 
  Serial.begin(115200);
  Serial.setDebugOutput(false); // dont want automatic debug msgs
    
  //I2C on pins
  Wire.begin(I2C_SDA, I2C_SCL);
  
  // BME280.. search both adresses
  if (bme.begin(0x76)) { 
    bme280_found = true;
  } else if (bme.begin(0x77)) {  
    bme280_found = true;
  } else {
    bme280_found = false; // disable weather functionality in handlers etc..
  }
  
  if (bme280_found) {
    // BME280 config
    bme.setSampling(Adafruit_BME280::MODE_FORCED,     // Operating mode - for power saving make the sensor sleep
                    Adafruit_BME280::SAMPLING_X2,     // Temp. oversampling
                    Adafruit_BME280::SAMPLING_X16,    // Pressure oversampling
                    Adafruit_BME280::SAMPLING_X1,     // Humidity oversampling
                    Adafruit_BME280::FILTER_X16,      // Filtering
                    Adafruit_BME280::STANDBY_MS_500); // Standby time
  }
  
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG; 
  
  // Optimize camera settings for photo capture
  if(psramFound()) {
    config.frame_size = FRAMESIZE_UXGA; // 1600x1200 
    config.jpeg_quality = 10;  // better quality
    config.fb_count = 1;       // Use only 1 frame buffer
  } else {
    config.frame_size = FRAMESIZE_SVGA; // 800x600
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  
  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    return;
  }
  
  // PIR init
  pinMode(PIR_PIN, INPUT);
  
  // Configure static IP
  IPAddress local_IP(192, 168, 244, 101);
  IPAddress gateway(192, 168, 244, 7);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress primaryDNS(8, 8, 8, 8);
  IPAddress secondaryDNS(8, 8, 4, 4);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }
  
  // Wi-Fi connection
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  Serial.println(WiFi.localIP());
  
  // camera server
  startCameraServer();
}

void loop() {
  // Check WiFi connection and reconnect if needed
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected! Attempting to reconnect...");
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 6) { // Try for 3 seconds
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi reconnected!");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("\nWiFi reconnection failed. Will try again next loop.");
    }
  }
  
  // auto weather data sending
  unsigned long currentTime = millis();
  if (currentTime - lastWeatherSend >= weatherInterval) {
    sendWeatherDataAuto();
    lastWeatherSend = currentTime;
  }
  
  // reset motion photo count every minute
  unsigned long currentMinute = currentTime / 60000;
  if (currentMinute != lastMotionMinute) {
    motionPhotoCount = 0;
    lastMotionMinute = currentMinute;
  }
  
  val = digitalRead(PIR_PIN);
  if (val == HIGH) {
    // motion detected
    if (pirState == LOW) { //checks for debounce
      // debounced - need to check if we can capture a photo on time
      if (motionPhotoCount < maxPhotosPerMinute && 
          (currentTime - lastMotionTime >= motionDebounceTime)) {
        capturePhotoOnMotion(); //capturing photo
        motionPhotoCount++;
        lastMotionTime = currentTime;
      }
      pirState = HIGH;
    }
  } else {
    pirState = LOW;
  }
  delay(100); 
}

void capturePhotoOnMotion() {
 // similar to capture_handler, but without HTTP request
  camera_fb_t * fb = NULL;
  fb = esp_camera_fb_get();
  if (fb) {
    esp_camera_fb_return(fb);
  }
   delay(200);
  
  fb = esp_camera_fb_get();
  if (!fb) {
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(flask_server);
    http.addHeader("Content-Type", "image/jpeg");
    http.setTimeout(10000); // 10 second timeout
    
    int httpResponseCode = http.POST(fb->buf, fb->len);
    
    http.end();
  } 
  esp_camera_fb_return(fb);
}