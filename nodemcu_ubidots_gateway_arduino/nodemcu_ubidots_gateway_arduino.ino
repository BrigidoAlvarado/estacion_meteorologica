/* 
 * ESTE CÓDIGO DEBE SER FLASHEADO EN EL NODEMCU ESP8266.
 * El NodeMCU actuará como gateway: recibirá los datos del Arduino Mega 
 * por su puerto Serial (Pins TX/RX) y los enviará a Ubidots.
 * 
 * VERSION v2.1 - Integración MQ136 (SO2 / Azufre)
 * CONEXIONES:
 * Nodemcu D2  -> TX1 Mega
 * Nodemcu GND -> GND Mega 
 */
#include <ESP8266WiFi.h>
#include <PubSubClient.h> 
#include <SoftwareSerial.h>
// --- Configuración WiFi y Ubidots ---
#define WIFI_SSID "A25"
#define WIFI_PASS "12345678"
#define UBIDOTS_TOKEN "BBUS-jo48DbrTmARlT2O2B6myKqFRGyOt78"
#define DEVICE_LABEL "estacion-meteorologica"
#define MQTT_CLIENT_NAME "nodemcu-gateway"
// --- Pines para SoftwareSerial (Recepción desde el Mega) ---
// NodeMCU D2 (GPIO4) como RX (Para recibir datos del Mega TX1) RX ES USADO POR USB
#define MEGA_RX_PIN D2 
// CORRECCION: El primer parametro es RX, el segundo es TX
SoftwareSerial megaSerial(MEGA_RX_PIN, -1); // RX, TX (-1 = no usado)
// --- Objetos ---
WiFiClient espClient;
PubSubClient client(espClient);
// --- Tiempos ---
unsigned long lastConnectAttempt = 0;
const long connectInterval = 10000;
// --- Trama de datos recibida del Mega ---
struct SensorData {
  float T = 0, H = 0, CO = 0, O3 = 0;
  int MQ135_ADC = 0;
  float NH3 = 0, NOx = 0, CO2 = 0;
  int PM1 = 0, PM25 = 0, PM10 = 0;
  float SO2 = 0;   // <-- NUEVO: azufre medido por MQ136
};
SensorData data;
// --- Prototipos ---
void connectToWiFi();
void connectToUbidots();
bool parseAndSaveData(const char* dataStr);
void publishDataToUbidots(const SensorData& d);
void printParsedData(const SensorData& d); 
void setup() {
  // Serial del NodeMCU (para DEBUG a la PC)
  Serial.begin(9600); 
  // Serial de Software (para ESCUCHAR al Mega)
  megaSerial.begin(9600);
  
  Serial.println(F("\n--- NodeMCU Ubidots Gateway v2.1 (MQ136 SO2) ---"));
  Serial.println(F("DEBUG a PC en Serial."));
  Serial.println(F("Recepción de Mega en D2 (GPIO4) a 9600 baudios."));
  Serial.println(F("CONEXION: Mega TX1(Pin18) -> NodeMCU D2, GND->GND"));
  // Configura MQTT
  client.setServer("industrial.api.ubidots.com", 1883); 
  
  // Inicia Wi-Fi
  connectToWiFi();
}
void loop() {
  // 1. Mantiene la conexión MQTT
  if (!client.connected()) {
    if (millis() - lastConnectAttempt > connectInterval) {
      connectToUbidots();
      lastConnectAttempt = millis();
    }
  }
  client.loop();
  
  // 2. Lee datos del Arduino Mega (Software Serial)
  if (megaSerial.available()) {
    String dataStr = megaSerial.readStringUntil('\n');
    dataStr.trim();
    Serial.print(F("\n[RX] Trama CSV recibida del Mega: "));
    Serial.println(dataStr); 
    
    if (dataStr.length() > 0 && parseAndSaveData(dataStr.c_str())) {
      printParsedData(data);
      
      // 3. Si el NodeMCU está conectado, publica los datos
      if (client.connected()) {
        publishDataToUbidots(data);
      } else {
        Serial.println(F("Ubidots no conectado. Intentando reconectar..."));
        connectToUbidots();
      }
    } else if (dataStr.length() > 0) {
      Serial.println(F("ERROR: Fallo al parsear la trama de datos (Formato incorrecto o incompleto)."));
    }
  }
}
// --- FUNCIONES DE CONECTIVIDAD ---
void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("Conectando a WiFi"));
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(F("."));
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print(F("WiFi Conectado! IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println(F("ERROR: No se pudo conectar a WiFi. Verifica SSID y contraseña."));
  }
}
void connectToUbidots() {
  Serial.print(F("Conectando a Ubidots (MQTT)..."));
  
  if (client.connect(MQTT_CLIENT_NAME, UBIDOTS_TOKEN, UBIDOTS_TOKEN)) {
    Serial.println(F(" ¡Conectado!"));
  } else {
    Serial.print(F(" Falló, rc="));
    Serial.print(client.state());
    Serial.println(F(". Reintentando..."));
    // rc=5: Token incorrecto
    // rc=-2: Conexión de red fallida
  }
}
// --- FUNCIÓN DE PARSEO Y PUBLICACIÓN ---
/**
 * Lee la trama CSV del Mega y la guarda en la estructura global 'data'.
 * Formato esperado: T,H,CO,O3,MQ135_ADC,NH3,NOx,CO2,PM1,PM25,PM10,SO2
 */
bool parseAndSaveData(const char* dataStr) {
  int result = sscanf(dataStr, "%f,%f,%f,%f,%d,%f,%f,%f,%d,%d,%d,%f",
                      &data.T, &data.H, &data.CO, &data.O3, &data.MQ135_ADC,
                      &data.NH3, &data.NOx, &data.CO2, &data.PM1, &data.PM25, &data.PM10,
                      &data.SO2);   // <-- NUEVO campo
  return (result == 12);   // <-- Ahora son 12 campos
}
/**
 * Imprime los datos parseados para debug en el Monitor Serial.
 */
void printParsedData(const SensorData& d) {
  Serial.println(F("--- DATOS PARSEADOS EN NODEMCU ---"));
  Serial.println(F("Parseo exitoso (12 campos encontrados)."));
  Serial.print(F("Temperatura (T): ")); Serial.print(d.T);
  Serial.print(F(" | Humedad (H): ")); Serial.print(d.H);
  Serial.print(F(" | CO: ")); Serial.println(d.CO);
  
  Serial.print(F("Ozono (O3): ")); Serial.print(d.O3);
  Serial.print(F(" | ADC MQ135: ")); Serial.print(d.MQ135_ADC);
  Serial.print(F(" | NH3: ")); Serial.println(d.NH3);
  
  Serial.print(F("NOx: ")); Serial.print(d.NOx);
  Serial.print(F(" | CO2: ")); Serial.print(d.CO2);
  Serial.print(F(" | PM1/2.5/10: ")); 
  Serial.print(d.PM1); Serial.print(F("/"));
  Serial.print(d.PM25); Serial.print(F("/"));
  Serial.println(d.PM10);
  Serial.print(F("SO2 (MQ136): ")); Serial.print(d.SO2); Serial.println(F(" ppm"));   // <-- NUEVO
  Serial.println(F("----------------------------------"));
}
/**
 * Publica todos los valores a Ubidots en una sola publicación MQTT.
 */
void publishDataToUbidots(const SensorData& d) {
  if (!client.connected()) {
    Serial.println(F("Error: Cliente MQTT desconectado."));
    return;
  }
  char payload[340];   // Ampliado ligeramente por el campo SO2
  
  sprintf(payload, "{\"temperatura\":%.1f,\"humedad\":%.1f,\"co\":%.1f,\"o3\":%.1f,\"mq135_adc\":%d,\"nh3\":%.1f,\"nox\":%.1f,\"co2\":%.1f,\"pm1\":%d,\"pm25\":%d,\"pm10\":%d,\"so2\":%.1f}",
          d.T, d.H, d.CO, d.O3, d.MQ135_ADC,
          d.NH3, d.NOx, d.CO2, d.PM1, d.PM25, d.PM10,
          d.SO2);   // <-- NUEVO campo en el JSON
          
  char topic[50];
  sprintf(topic, "/v1.6/devices/%s", DEVICE_LABEL);
  
  if (client.publish(topic, payload)) {
    Serial.print(F("[TX] Datos MQTT enviados a: "));
    Serial.println(topic);
    Serial.print(F("[TX] Payload: "));
    Serial.println(payload);
  } else {
    Serial.println(F("ERROR: Fallo en la publicación MQTT."));
  }
}
