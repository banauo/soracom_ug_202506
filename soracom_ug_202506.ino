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
#include <DHT.h>  // Include DHT library

#define SEARCH_ACCESS_TECHNOLOGY (WioCellularNetwork::SearchAccessTechnology::LTEM)  // https://seeedjp.github.io/Wiki/Wio_BG770A/kb/kb4.html
#define LTEM_BAND (WioCellularNetwork::NTTDOCOMO_LTEM_BAND)                          // https://seeedjp.github.io/Wiki/Wio_BG770A/kb/kb4.html
static const char APN[] = "soracom.io";

static const char GET_HOST[] = "metadata.soracom.io";
static const char GET_PATH[] = "/v1/userdata";
static constexpr int GET_PORT = 80;

static const char POST_HOST[] = "uni.soracom.io";
static const char POST_PATH[] = "/record.json";
static constexpr int POST_PORT = 80;

static constexpr int INTERVAL = 1000 * 60 * 5;         // [ms]
static constexpr int POWER_ON_TIMEOUT = 1000 * 20;     // [ms]
static constexpr int NETWORK_TIMEOUT = 1000 * 60 * 2;  // [ms]
static constexpr int RECEIVE_TIMEOUT = 1000 * 10;      // [ms]

#define DHTPIN D28  // Pin for DHT22 sensor
#define DHTTYPE DHT22  // Define sensor type as DHT22

DHT dht(DHTPIN, DHTTYPE);  // Create DHT object

struct HttpResponse {
  int statusCode;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct SensorData {
  int distance;
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

  // Initialize DHT sensor
  dht.begin();

  // Power on the cellular module
  if (WioCellular.powerOn(POWER_ON_TIMEOUT) != WioCellularResult::Ok) abort();
  WioNetwork.begin();

  // Wait for communication available
  if (!WioNetwork.waitUntilCommunicationAvailable(NETWORK_TIMEOUT)) abort();

  digitalWrite(LED_BUILTIN, LOW);
}

void loop(void) {
  digitalWrite(LED_BUILTIN, HIGH);
  WioCellular.enableGrovePower();

  // Wait for DHT22 sensor to stabilize
  delay(1000);

  HttpResponse response;
  {
    WioCellularArduinoTcpClient<WioCellularModule> client{ WioCellular, WioNetwork.config.pdpContextId };
    response = httpRequest(client, GET_HOST, GET_PORT, GET_PATH, "GET", "application/json", "");
  }
  Serial.print("Body: ");

  // Check if the body contains a number and assign it to app_id
  int app_id = 0;
  if (response.body.find_first_not_of("0123456789") == std::string::npos) {
    app_id = std::stoi(response.body);
    Serial.print("Extracted app_id: ");
    Serial.println(app_id);
  } else {
    Serial.println("Response body does not contain a valid number.");
  }

  // Create SensorData instance and populate it with sensor data
  SensorData sensorData;
  sensorData.distance = 100;  // Example value
  sensorData.co2 = 400;       // Example value

  // Read temperature and humidity from DHT22 sensor
  sensorData.temperature = dht.readTemperature();  // Get temperature
  sensorData.humidity = dht.readHumidity();        // Get humidity

  // Check for sensor read errors
  if (isnan(sensorData.temperature) || isnan(sensorData.humidity)) {
    Serial.println("Failed to read from DHT sensor!");
    sensorData.temperature = 0;  // Default value
    sensorData.humidity = 0;     // Default value
  }

  WioCellular.disableGrovePower();

  sensorData.measure_date = "2025-04-25T12:00:00Z"; // Example ISO 8601 date

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
  Serial.println("### Measuring");

  doc["app"] = app_id;

  JsonObject record = doc.createNestedObject("record");

  record["distance"]["value"] = sensorData.distance;
  record["co2"]["value"] = sensorData.co2;
  record["temperature"]["value"] = sensorData.temperature;
  record["humidity"]["value"] = sensorData.humidity;
  record["measure_date"]["value"] = sensorData.measure_date;

  Serial.println("### Completed");

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
