/* 
 * ESTE CÓDIGO DEBE SER FLASHEADO EN EL NODEMCU ESP8266.
 * El NodeMCU actuará como gateway: recibirá los datos del Arduino Mega 
 * por su puerto Serial (Pins TX/RX) y los enviará a Ubidots.
 * 
 * VERSION v3.0 - Low Power, controlado por Mega via RST
 * CONEXIONES:
 * Nodemcu D2  -> TX1 Mega (pin 18)
 * Nodemcu GND -> GND Mega
 * Nodemcu RST -> Mega pin 8      <- NUEVO cable
 * Nodemcu D0  -> Nodemcu RST     <- necesario para deepSleep(0)
 * 
 * FLUJO:
 * 1. Mega tiene datos listos → pulsa RST del NodeMCU
 * 2. NodeMCU arranca setup() desde cero
 * 3. Espera CSV del Mega máx 10s
 * 4. WiFi → MQTT → publica → Deep Sleep indefinido
 * 5. Duerme hasta que el Mega lo vuelva a resetear
 */

 #include <ESP8266WiFi.h>
 #include <PubSubClient.h>
 #include <SoftwareSerial.h>
 
 // --- Configuración WiFi y Ubidots ---
 #define WIFI_SSID        "A25"
 #define WIFI_PASS        "12345678"
 #define UBIDOTS_TOKEN    "BBUS-jo48DbrTmARlT2O2B6myKqFRGyOt78"
 #define DEVICE_LABEL     "estacion-meteorologica"
 #define MQTT_CLIENT_NAME "nodemcu-gateway"
 
 // --- Pines ---
 // NodeMCU D2 (GPIO4) como RX para recibir datos del Mega TX1
 #define MEGA_RX_PIN D2
 
 // Tiempo máximo esperando datos del Mega (ms)
 #define MAX_WAIT_FOR_MEGA_MS 10000UL
 
 // --- SoftwareSerial ---
 SoftwareSerial megaSerial(MEGA_RX_PIN, -1); // RX, TX (-1 = no usado)
 
 // --- Objetos ---
 WiFiClient   espClient;
 PubSubClient client(espClient);
 
 // --- Trama de datos recibida del Mega ---
 // Formato CSV: T,H,CO,O3,MQ135_ADC,NH3,NOx,CO2,PM1,PM25,PM10,SO2
 struct SensorData {
   float T         = 0;
   float H         = 0;
   float CO        = 0;
   float O3        = 0;
   int   MQ135_ADC = 0;
   float NH3       = 0;
   float NOx       = 0;
   float CO2       = 0;
   int   PM1       = 0;
   int   PM25      = 0;
   int   PM10      = 0;
   float SO2       = 0;
 };
 SensorData data;
 
 // --- Prototipos ---
 bool waitForMegaData();
 bool parseAndSaveData(const char* dataStr);
 void connectToWiFi();
 void connectToUbidots();
 void publishDataToUbidots(const SensorData& d);
 void printParsedData(const SensorData& d);
 void dormirNodeMCU();
 
 // ============================================================
 // SETUP — todo el trabajo ocurre aquí, loop queda vacío
 // ============================================================
 
 void setup() {
   Serial.begin(9600);
   megaSerial.begin(9600);
 
   Serial.println(F("\n--- NodeMCU Ubidots Gateway v3.0 (Low Power) ---"));
   Serial.println(F("Despertado por Mega via RST"));
   Serial.println(F("Recepción de Mega en D2 (GPIO4) a 9600 baudios."));
   Serial.println(F("CONEXION: Mega TX1(Pin18) -> NodeMCU D2, GND->GND"));
 
   // WiFi apagado desde el inicio — no se enciende hasta tener datos
   WiFi.mode(WIFI_STA);
   WiFi.forceSleepBegin();
   delay(1);
 
   client.setServer("industrial.api.ubidots.com", 1883);
 
   // --- PASO 1: Esperar datos del Mega ---
   if (!waitForMegaData()) {
     Serial.println(F("Sin datos del Mega. Durmiendo..."));
     dormirNodeMCU();
     return;
   }
 
   printParsedData(data);
 
   // --- PASO 2: Encender WiFi y conectar ---
   connectToWiFi();
 
   if (WiFi.status() != WL_CONNECTED) {
     Serial.println(F("ERROR: WiFi falló. Durmiendo..."));
     dormirNodeMCU();
     return;
   }
 
   // --- PASO 3: Conectar MQTT ---
   connectToUbidots();
 
   if (!client.connected()) {
     Serial.println(F("ERROR: MQTT falló. Durmiendo..."));
     dormirNodeMCU();
     return;
   }
 
   // --- PASO 4: Publicar datos ---
   publishDataToUbidots(data);
 
   // Pausa para asegurar que el broker recibe el mensaje
   delay(2000);
   client.disconnect();
 
   // --- PASO 5: Deep Sleep indefinido ---
   // El Mega controlará cuándo despertar via pulso en RST
   dormirNodeMCU();
 }
 
 // Loop vacío — el NodeMCU nunca llega aquí
 void loop() {}
 
 // ============================================================
 // ESPERAR TRAMA DEL MEGA
 // Lee el puerto serie hasta recibir una línea válida de 12
 // campos o hasta agotar MAX_WAIT_FOR_MEGA_MS
 // ============================================================
 
 bool waitForMegaData() {
   Serial.println(F("Esperando CSV del Mega..."));
   unsigned long inicio = millis();
 
   while (millis() - inicio < MAX_WAIT_FOR_MEGA_MS) {
 
     if (megaSerial.available()) {
       String dataStr = megaSerial.readStringUntil('\n');
       dataStr.trim();
 
       Serial.print(F("\n[RX] Trama CSV recibida del Mega: "));
       Serial.println(dataStr);
 
       if (dataStr.length() > 0 && parseAndSaveData(dataStr.c_str())) {
         Serial.println(F("CSV OK."));
         return true;
       } else if (dataStr.length() > 0) {
         Serial.println(F("ERROR: Formato incorrecto o incompleto. Reintentando..."));
       }
     }
 
     delay(10);
   }
 
   return false;
 }
 
 // ============================================================
 // CONECTIVIDAD
 // ============================================================
 
 void connectToWiFi() {
   Serial.println(F("Encendiendo WiFi..."));
 
   // Despertar el radio WiFi que estaba apagado
   WiFi.forceSleepWake();
   delay(1);
   WiFi.mode(WIFI_STA);
   WiFi.begin(WIFI_SSID, WIFI_PASS);
 
   Serial.print(F("Conectando a WiFi"));
   int attempts = 0;
 
   while (WiFi.status() != WL_CONNECTED && attempts < 20) {
     delay(500);
     Serial.print(F("."));
     attempts++;
   }
 
   Serial.println();
 
   if (WiFi.status() == WL_CONNECTED) {
     Serial.print(F("WiFi Conectado! IP: "));
     Serial.println(WiFi.localIP());
   } else {
     Serial.println(F("ERROR: No se pudo conectar a WiFi. Verifica SSID y contraseña."));
   }
 }
 
 void connectToUbidots() {
   Serial.print(F("Conectando a Ubidots (MQTT)..."));
 
   if (client.connect(MQTT_CLIENT_NAME, UBIDOTS_TOKEN, UBIDOTS_TOKEN)) {
     Serial.println(F(" Conectado!"));
   } else {
     Serial.print(F(" Falló, rc="));
     Serial.print(client.state());
     Serial.println(F(". Reintentando en próximo ciclo."));
     // rc=5: Token incorrecto
     // rc=-2: Conexión de red fallida
   }
 }
 
 // ============================================================
 // PARSEO CSV
 // Lee la trama del Mega y guarda en la estructura global data
 // Formato: T,H,CO,O3,MQ135_ADC,NH3,NOx,CO2,PM1,PM25,PM10,SO2
 // ============================================================
 
 bool parseAndSaveData(const char* dataStr) {
   int result = sscanf(dataStr,
     "%f,%f,%f,%f,%d,%f,%f,%f,%d,%d,%d,%f",
     &data.T, &data.H,
     &data.CO, &data.O3,
     &data.MQ135_ADC,
     &data.NH3, &data.NOx, &data.CO2,
     &data.PM1, &data.PM25, &data.PM10,
     &data.SO2
   );
 
   return (result == 12);  // válido solo si se parsearon los 12 campos
 }
 
 // ============================================================
 // DEBUG — imprime datos parseados en el Monitor Serial
 // ============================================================
 
 void printParsedData(const SensorData& d) {
   Serial.println(F("--- DATOS PARSEADOS EN NODEMCU ---"));
   Serial.println(F("Parseo exitoso (12 campos encontrados)."));
   Serial.print(F("Temperatura (T): ")); Serial.print(d.T);
   Serial.print(F(" | Humedad (H): ")); Serial.println(d.H);
 
   Serial.print(F("CO: "));  Serial.print(d.CO);
   Serial.print(F(" | O3: ")); Serial.print(d.O3);
   Serial.print(F(" | ADC MQ135: ")); Serial.println(d.MQ135_ADC);
 
   Serial.print(F("NH3: ")); Serial.print(d.NH3);
   Serial.print(F(" | NOx: ")); Serial.print(d.NOx);
   Serial.print(F(" | CO2: ")); Serial.println(d.CO2);
 
   Serial.print(F("PM1/2.5/10: "));
   Serial.print(d.PM1); Serial.print(F("/"));
   Serial.print(d.PM25); Serial.print(F("/"));
   Serial.println(d.PM10);
 
   Serial.print(F("SO2 (MQ136): ")); Serial.print(d.SO2); Serial.println(F(" ppm"));
   Serial.println(F("----------------------------------"));
 }
 
 // ============================================================
 // PUBLICAR EN UBIDOTS
 // Arma el payload JSON y lo publica por MQTT
 // Topic: /v1.6/devices/<device_label>
 // ============================================================
 
 void publishDataToUbidots(const SensorData& d) {
   if (!client.connected()) {
     Serial.println(F("Error: Cliente MQTT desconectado."));
     return;
   }
 
   char payload[340];
   sprintf(payload,
     "{\"temperatura\":%.1f,"
     "\"humedad\":%.1f,"
     "\"co\":%.1f,"
     "\"o3\":%.1f,"
     "\"mq135_adc\":%d,"
     "\"nh3\":%.1f,"
     "\"nox\":%.1f,"
     "\"co2\":%.1f,"
     "\"pm1\":%d,"
     "\"pm25\":%d,"
     "\"pm10\":%d,"
     "\"so2\":%.1f}",
     d.T, d.H,
     d.CO, d.O3,
     d.MQ135_ADC,
     d.NH3, d.NOx, d.CO2,
     d.PM1, d.PM25, d.PM10,
     d.SO2
   );
 
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
 
 // ============================================================
 // DEEP SLEEP INDEFINIDO
 // El Mega despierta el NodeMCU con pulso en RST (pin 8 Mega)
 // D0 (GPIO16) debe estar conectado a RST en el NodeMCU
 // ============================================================
 
 void dormirNodeMCU() {
   Serial.println(F("Apagando WiFi..."));
   WiFi.disconnect(true);
   WiFi.mode(WIFI_OFF);
   WiFi.forceSleepBegin();
   delay(1);
 
   Serial.println(F("Entrando Deep Sleep indefinido."));
   Serial.println(F("Esperando reset del Mega..."));
 
   // 0 = sleep indefinido
   // El Mega lo despierta bajando RST por 150ms cada ~10 minutos
   ESP.deepSleep(0);
 }