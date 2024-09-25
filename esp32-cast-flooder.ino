#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebSocketsClient.h>

#define MAX_DEVICES 5                  // Maximum number of Cast devices to handle
#define DISCOVERY_TIMEOUT 10000        // 10 seconds timeout for device discovery
#define DISCOVERY_RETRIES 3            // Max retries for device discovery
#define WS_CONNECT_RETRIES 3           // Max retries for WebSocket connections
#define SSID_MIN_LEN 1                 // Minimum length for SSID
#define SSID_MAX_LEN 32                // Maximum length for SSID
#define PASS_MIN_LEN 8                 // Minimum length for password
#define PASS_MAX_LEN 64                // Maximum length for password

// Cast device information
String castDeviceIP[MAX_DEVICES];
int castDevicePort[MAX_DEVICES];
int numDevices = 0;

// WebSocket client objects for each device
WebSocketsClient webSockets[MAX_DEVICES];

// Media URL to cast (replace with your own media URL)
String mediaURL = "https://example.com/video.mp4";

// Function prototypes
void discoverCastDevicesTask(void * parameter);
void sendCastCommandTask(void * parameter);
void connectToWiFi();
void getWiFiCredentials();
bool validateCredentials(const char* ssid, const char* password);
void sendCastCommand(String mediaURL, int deviceIndex);
void connectToCastDevice(String ip, int port, int deviceIndex);
void retryWiFiConnection();

// Global variables for Wi-Fi credentials
char ssid[50];
char password[50];

void setup() {
  Serial.begin(115200);

  // Get Wi-Fi credentials from serial input with validation
  getWiFiCredentials();

  // Connect to Wi-Fi with retry logic
  retryWiFiConnection();

  // Create a task on Core 0 to discover Cast devices
  xTaskCreatePinnedToCore(
    discoverCastDevicesTask,   // Function to run
    "DiscoverCastDevices",     // Task name
    4096,                      // Stack size
    NULL,                      // Task parameter
    1,                         // Task priority
    NULL,                      // Task handle
    0                          // Core 0
  );
}

void loop() {
  // WebSocket maintenance for all devices
  for (int i = 0; i < numDevices; i++) {
    webSockets[i].loop();
  }
}

// Function to get Wi-Fi credentials from the serial console with validation
void getWiFiCredentials() {
  bool valid = false;

  while (!valid) {
    Serial.println("Enter Wi-Fi SSID: ");
    while (Serial.available() == 0) {}  // Wait for user input
    Serial.readBytesUntil('\n', ssid, sizeof(ssid));

    Serial.println("Enter Wi-Fi Password: ");
    while (Serial.available() == 0) {}  // Wait for user input
    Serial.readBytesUntil('\n', password, sizeof(password));

    // Validate SSID and Password
    if (validateCredentials(ssid, password)) {
      valid = true;
      Serial.println("Wi-Fi Credentials received.");
    } else {
      Serial.println("Invalid SSID or password. Please try again.");
    }
  }
}

// Function to validate Wi-Fi credentials (SSID and password)
bool validateCredentials(const char* ssid, const char* password) {
  int ssidLen = strlen(ssid);
  int passLen = strlen(password);

  if (ssidLen < SSID_MIN_LEN || ssidLen > SSID_MAX_LEN) {
    Serial.println("SSID must be between 1 and 32 characters.");
    return false;
  }

  if (passLen < PASS_MIN_LEN || passLen > PASS_MAX_LEN) {
    Serial.println("Password must be between 8 and 64 characters.");
    return false;
  }

  return true;
}

// Function to retry Wi-Fi connection in case of failure
void retryWiFiConnection() {
  int retries = 0;

  while (retries < 3) {
    connectToWiFi();

    if (WiFi.status() == WL_CONNECTED) {
      break;
    }

    retries++;
    Serial.println("Retrying Wi-Fi connection (" + String(retries) + "/3)");
    delay(2000);  // Delay before retrying
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Failed to connect to Wi-Fi after 3 retries. Restarting...");
    ESP.restart();  // Restart the ESP32 if Wi-Fi connection fails
  }
}

// Function to connect to Wi-Fi
void connectToWiFi() {
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  int connectionAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && connectionAttempts < 10) {
    delay(1000);
    Serial.print(".");
    connectionAttempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to Wi-Fi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to Wi-Fi.");
  }
}

// Task running on Core 0 to discover Cast devices using mDNS with retries
void discoverCastDevicesTask(void * parameter) {
  int retries = 0;

  while (retries < DISCOVERY_RETRIES) {
    Serial.println("Starting Cast device discovery...");

    if (!MDNS.begin("esp32")) {
      Serial.println("Error starting mDNS");
      vTaskDelete(NULL);  // End the task if mDNS fails
    }

    unsigned long startTime = millis();
    bool devicesDiscovered = false;

    while (millis() - startTime < DISCOVERY_TIMEOUT && numDevices < MAX_DEVICES) {
      int n = MDNS.queryService("googlecast", "tcp");

      if (n > 0) {
        devicesDiscovered = true;
        for (int i = 0; i < n && i < MAX_DEVICES; i++) {
          Serial.print("Found device: ");
          Serial.print(MDNS.hostname(i));
          Serial.print(" at ");
          Serial.print(MDNS.IP(i));
          Serial.print(":");
          Serial.println(MDNS.port(i));

          castDeviceIP[numDevices] = MDNS.IP(i).toString();
          castDevicePort[numDevices] = MDNS.port(i);
          numDevices++;

          // Connect to each discovered Cast device
          connectToCastDevice(castDeviceIP[i], castDevicePort[i], i);
        }
      } else {
        Serial.println("No Cast devices found yet...");
        delay(1000);  // Retry discovery every second
      }
    }

    if (devicesDiscovered) {
      break;
    } else {
      retries++;
      Serial.println("Retrying Cast device discovery (" + String(retries) + "/" + String(DISCOVERY_RETRIES) + ")");
    }
  }

  if (numDevices == 0) {
    Serial.println("No Cast devices discovered after retries.");
    vTaskDelete(NULL);  // End the task if no devices are found
  }

  // Create a task on Core 1 to send cast commands to all devices
  xTaskCreatePinnedToCore(
    sendCastCommandTask,
    "SendCastCommand",
    4096,
    NULL,
    1,
    NULL,
    1
  );

  vTaskDelete(NULL);  // End the discovery task once discovery is done
}

// Function to connect to a Cast device via WebSocket with retry mechanism
void connectToCastDevice(String ip, int port, int deviceIndex) {
  int retries = 0;

  while (retries < WS_CONNECT_RETRIES) {
    Serial.println("Connecting to Cast device: " + ip + ":" + String(port));

    webSockets[deviceIndex].begin(ip.c_str(), port, "/");

    // WebSocket event handler
    webSockets[deviceIndex].onEvent([deviceIndex](WStype_t type, uint8_t * payload, size_t length) {
      switch (type) {
        case WStype_CONNECTED:
          Serial.println("Connected to Cast device " + String(deviceIndex));
          break;
        case WStype_DISCONNECTED:
          Serial.println("Disconnected from Cast device " + String(deviceIndex));
          break;
        case WStype_TEXT:
          Serial.println("Received message:");
          Serial.println((char*)payload);
          break;
        default:
          break;
      }
    });

    delay(1000);  // Give WebSocket a moment to establish connection

    if (webSockets[deviceIndex].isConnected()) {
      Serial.println("Successfully connected to Cast device " + String(deviceIndex));
      return;  // Exit the retry loop if connection is successful
    } else {
      retries++;
      Serial.println("Retrying connection to Cast device (" + String(retries) + "/" + String(WS_CONNECT_RETRIES) + ")");
    }
  }

  Serial.println("Failed to connect to Cast device " + String(deviceIndex) + " after retries.");
}
