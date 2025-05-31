/*
 * soracom-uptime-httpclient.ino
 * Copyright (C) Seeed K.K.
 * MIT License
 */

////////////////////////////////////////////////////////////////////////////////
// Libraries:
//   http://librarymanager#ArduinoJson 7.0.4
//   http://librarymanager#ArduinoHttpClient 0.6.1

#include <Adafruit_TinyUSB.h>
#include <csignal>
#include <map>
#include <WioCellular.h>
#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include <Grove_Temperature_And_Humidity_Sensor.h>
#include <Ultrasonic.h>
#include <SCD30.h>

#define SEARCH_ACCESS_TECHNOLOGY (WioCellularNetwork::SearchAccessTechnology::LTEM)  // https://seeedjp.github.io/Wiki/Wio_BG770A/kb/kb4.html
#define LTEM_BAND (WioCellularNetwork::NTTDOCOMO_LTEM_BAND)                          // https://seeedjp.github.io/Wiki/Wio_BG770A/kb/kb4.html
static const char APN[] = "soracom.io";

static const char GET_HOST[] = "metadata.soracom.io";
static const char GET_PATH[] = "/v1/userdata";
static constexpr int GET_PORT = 80;

static const char POST_HOST[] = "uni.soracom.io";
static const char POST_PATH[] = "/record.json";
static constexpr int POST_PORT = 80;

static constexpr int INTERVAL = 1000 * 60 * 1;         // [ms]
static constexpr int POWER_ON_TIMEOUT = 1000 * 20;     // [ms]
static constexpr int NETWORK_TIMEOUT = 1000 * 60 * 2;  // [ms]
static constexpr int RECEIVE_TIMEOUT = 1000 * 10;      // [ms]

#define ULTRASONIC_PIN (D30)  // Grove - Digital (P1)
#define DHT_PIN D28  // Grove -Analog (P2)
#define DHT_TYPE DHT22  // Define sensor type as DHT22

Ultrasonic UltrasonicRanger(ULTRASONIC_PIN);
DHT dht(DHT_PIN, DHT_TYPE);  // Create DHT object

struct HttpResponse {
  int statusCode;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct SensorData {
  long distance;
  int co2;
  float temperature;
  float humidity;
  const char* measure_date;
};

static void abortHandler(int sig) {
  while (true) {
    ledOn(LED_BUILTIN);
    delay(100);
    ledOff(LED_BUILTIN);
    delay(100);
  }
}

/**
 * Custom function to parse date header into a tm structure.
 */
bool parseDateHeader(const char* dateHeader, struct tm& tm) {
  int day, year, hour, minute, second;
  char month[4];
  if (sscanf(dateHeader, "%*3s, %d %3s %d %d:%d:%d %*s", &day, month, &year, &hour, &minute, &second) == 6) {
    const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* pos = strstr(months, month);
    if (pos) {
      tm.tm_mday = day;
      tm.tm_mon = (pos - months) / 3;
      tm.tm_year = year - 1900;
      tm.tm_hour = hour;
      tm.tm_min = minute;
      tm.tm_sec = second;
      return true;
    }
  }
  return false;
}

static JsonDocument JsonDoc;

void setup(void) {
  signal(SIGABRT, abortHandler);
  Serial.begin(115200);
  {
    const auto start = millis();
    while (!Serial && millis() - start < 5000) {
      delay(2);
    }
  }
  Serial.println();
  Serial.println();

  Serial.println("Startup");
  digitalWrite(LED_BUILTIN, HIGH);

  // Network configuration
  WioNetwork.config.searchAccessTechnology = SEARCH_ACCESS_TECHNOLOGY;
  WioNetwork.config.ltemBand = LTEM_BAND;
  WioNetwork.config.apn = APN;

  // Start WioCellular
  WioCellular.begin();

  // Initialize sensors
  WioCellular.enableGrovePower();
  dht.begin();
  Wire.begin();
  scd30.initialize();

  // Power on the cellular module
  if (WioCellular.powerOn(POWER_ON_TIMEOUT) != WioCellularResult::Ok) abort();
  WioNetwork.begin();

  // Wait for communication available
  if (!WioNetwork.waitUntilCommunicationAvailable(NETWORK_TIMEOUT)) abort();

  digitalWrite(LED_BUILTIN, LOW);
}

void loop(void) {
  Serial.println("### Measuring");
  digitalWrite(LED_BUILTIN, HIGH);

  // Wait for DHT22 sensor to stabilize
  delay(1000);

  HttpResponse response;
  {
    WioCellularArduinoTcpClient<WioCellularModule> client{ WioCellular, WioNetwork.config.pdpContextId };
    response = httpRequest(client, GET_HOST, GET_PORT, GET_PATH, "GET", "application/json", "");
  }

  // Check if the body contains a number and assign it to app_id
  int app_id = 0;
  if (response.body.find_first_not_of("0123456789") == std::string::npos) {
    app_id = std::stoi(response.body);
  } else {
    Serial.println("Response body does not contain a valid number.");
  }

  // Extract the "Date" header and convert it to ISO 8601 format
  char now_iso8601Date[30];
  if (response.headers.find("Date") != response.headers.end()) {
    const std::string& dateHeader = response.headers["Date"];

    // Convert the Date header to ISO 8601 format
    struct tm tm;
    if (parseDateHeader(dateHeader.c_str(), tm)) {
      strftime(now_iso8601Date, sizeof(now_iso8601Date), "%Y-%m-%dT%H:%M:%SZ", &tm);
    } else {
      Serial.println("Failed to parse date header.");
    }
  } else {
    Serial.println("Date header not found in the response.");
  }

  SensorData sensorData = {0};

  // Read distance from ultrasonic ranger
  sensorData.distance = UltrasonicRanger.MeasureInCentimeters();

  // Read temperature and humidity from SCD30 sensor
  if (scd30.isAvailable()) {
    float result[3] = {0};
    scd30.getCarbonDioxideConcentration(result);
    sensorData.co2 = result[0];
    sensorData.temperature = result[1];
    sensorData.humidity = result[2];
  }

  // Read temperature and humidity from DHT22 sensor
  float temp_hum_val[2];
  if (!dht.readTempAndHumidity(temp_hum_val)){
    if (temp_hum_val[0] != 0 && temp_hum_val[1] != 0) {
      sensorData.temperature = temp_hum_val[1];
      sensorData.humidity = temp_hum_val[0];
    }
  }

  Serial.println("### Completed");

  sensorData.measure_date = now_iso8601Date;

  JsonDoc.clear();
  if (generateRequestBody(JsonDoc, app_id, sensorData)) {
    std::string jsonStr;
    serializeJson(JsonDoc, jsonStr);
    Serial.println(jsonStr.c_str());

    HttpResponse response;
    {
      WioCellularArduinoTcpClient<WioCellularModule> client{ WioCellular, WioNetwork.config.pdpContextId };
      response = httpRequest(client, POST_HOST, POST_PORT, POST_PATH, "POST", "application/json", jsonStr.c_str());
    }

    Serial.println("Header(s):");
    for (auto header : response.headers) {
      Serial.print("  ");
      Serial.print(header.first.c_str());
      Serial.print(" : ");
      Serial.print(header.second.c_str());
      Serial.println();
    }
    Serial.print("Body: ");
    Serial.println(response.body.c_str());
  }

  digitalWrite(LED_BUILTIN, LOW);

  WioCellular.doWorkUntil(INTERVAL);
}

/**
 * Generate request body for many sensor data.
 */
static bool generateRequestBody(JsonDocument& doc, int app_id, const SensorData& sensorData) {

  doc["app"] = app_id;

  JsonObject record = doc.createNestedObject("record");

  if (sensorData.distance != 0) {
    record["distance"]["value"] = sensorData.distance;
  }
  if (sensorData.co2 != 0) {
    record["co2"]["value"] = sensorData.co2;
  }
  if (sensorData.temperature != 0) {
    record["temperature"]["value"] = round(sensorData.temperature * 10) / 10.0;
  }
  if (sensorData.humidity != 0) {
    record["humidity"]["value"] = round(sensorData.humidity * 10) / 10.0;
  }
  record["measure_date"]["value"] = sensorData.measure_date;


  return true;
}

static HttpResponse httpRequest(Client& client, const char* host, int port, const char* path, const char* method, const char* contentType, const char* requestBody) {
  HttpResponse httpResponse;
  Serial.print("### Requesting to [");
  Serial.print(host);
  Serial.println("]");

  HttpClient httpClient(client, host, port);
  int err = httpClient.startRequest(path, method, contentType, strlen(requestBody), (const byte*)requestBody);
  if (err != 0) {
    httpClient.stop();
    httpResponse.statusCode = err;
    return httpResponse;
  }

  int statusCode = httpClient.responseStatusCode();
  if (!statusCode) {
    httpClient.stop();
    httpResponse.statusCode = statusCode;
    return httpResponse;
  }

  Serial.print("Status code returned ");
  Serial.println(statusCode);
  httpResponse.statusCode = statusCode;

  while (httpClient.headerAvailable()) {
    String headerName = httpClient.readHeaderName();
    String headerValue = httpClient.readHeaderValue();
    httpResponse.headers[headerName.c_str()] = headerValue.c_str();
  }

  int length = httpClient.contentLength();
  if (length >= 0) {
    Serial.print("Content length: ");
    Serial.println(length);
  }
  if (httpClient.isResponseChunked()) {
    Serial.println("The response is chunked");
  }

  String responseBody = httpClient.responseBody();
  httpResponse.body = responseBody.c_str();

  httpClient.stop();

  Serial.println("### End HTTP request");
  Serial.println();

  return httpResponse;
}
