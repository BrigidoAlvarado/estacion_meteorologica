#include <avr/pgmspace.h>
#include <DHTStable.h>
#include <SoftwareSerial.h> 
// --- Versión del programa ---
#define VERSION "v18.2.0 (Mega Sensor Hub + MQ136 SO2)" 
// --- Pines ---
// Serial1 (Pins 18/19) se usa para enviar datos al NodeMCU.
#define DHTPIN 4
#define PMS_RX_PIN 13    
#define PMS_TX_PIN 12    
#define MQ7_PIN A0
#define MQ135_PIN A1
#define MQ131_PIN A2
#define MQ136_PIN A5   // <-- NUEVO: Sensor de Azufre (SO2 / H2S)
// --- Objetos ---
SoftwareSerial pmsSerial(PMS_RX_PIN, PMS_TX_PIN); 
DHTStable dht;
// --- Tiempos ---
#define SENSOR_INTERVAL 30000  // 30 segundos entre envíos de datos
#define DHT_READ_INTERVAL 3000  // 3 segundos para lecturas DHT
unsigned long prevSensorMillis, prevDhtMillis;
// --- Estado ---
bool dhtReadSuccess = false;
bool lastPMSReadSuccess = false;
// --- Variables de sensores ---
float temperatura = 0.0, humedad = 0.0;
float ppm_nh3 = 0.0, ppm_nox = 0.0, ppm_co2 = 0.0;
int adc_mq7 = 0, adc_mq135 = 0, adc_mq131 = 0;
int adc_mq136 = 0;        // <-- NUEVO: ADC del MQ136
float ppm_so2 = 0.0;      // <-- NUEVO: concentración de SO2 estimada
// --- Variables de PMS5003 ---
struct PMSData {
  uint16_t pm1_0 = 0;
  uint16_t pm2_5 = 0;
  uint16_t pm10_0 = 0;
  bool valid = false;
};
PMSData pmsData;
// --- Buffer ---
char dataBuffer[220];  // Buffer para la trama de datos a enviar al NodeMCU (aumentado por campo extra)
// --- Prototipos ---
void sendDataToNodeMCU(float ppm_co, float ppm_o3);
bool readDhtWithRetry();
bool readPMSDataToGlobal();
void calculateMQ135Gases(int adc_value);
float calculateMQ136SO2(int adc_value);   // <-- NUEVO prototipo
void setup() {
  Serial.begin(9600);
  // Inicialización: HARDWARE SERIAL 1 para NodeMCU (Pines 18, 19)
  Serial1.begin(9600); 
  
  Serial.print(F("\n--- ARDUINO MEGA SENSOR HUB ("));
  Serial.print(F(VERSION));
  Serial.println(F(") ---"));
  Serial.println(F("Enviando datos a NodeMCU por Serial1 (9600 bps)"));
  Serial.println(F("CONEXION: TX1(Pin18)->NodeMCU D2, GND->GND"));
  Serial.println(F("MQ136 (Azufre/SO2) conectado en A3"));
  
  readDhtWithRetry();
  pmsData.valid = false;
  
  prevSensorMillis = millis();
  prevDhtMillis = millis();
}
void loop() {
  unsigned long currentMillis = millis();
  
  // --- Lectura DHT (cada 3 segundos) ---
  if (currentMillis - prevDhtMillis >= DHT_READ_INTERVAL) {
    prevDhtMillis = currentMillis;
    dhtReadSuccess = readDhtWithRetry();
  }
  
  // --- Lectura sensores y envío (cada 30 segundos) ---
  if (currentMillis - prevSensorMillis >= SENSOR_INTERVAL) {
    prevSensorMillis = currentMillis;
    
    Serial.println(F("\n--- Ciclo de Lectura y Envío ---"));
    
    // 1. Lectura MQ y cálculo de gases
    adc_mq7 = analogRead(MQ7_PIN);
    adc_mq135 = analogRead(MQ135_PIN);
    adc_mq131 = analogRead(MQ131_PIN);
    adc_mq136 = analogRead(MQ136_PIN);   // <-- NUEVO: lectura del MQ136
    calculateMQ135Gases(adc_mq135);
    ppm_so2 = calculateMQ136SO2(adc_mq136);  // <-- NUEVO: cálculo de SO2
    
    // Cálculo de CO y O3 para el envío
    float ppm_co = adc_mq7 * (5.0 / 1023.0) * 20.0;
    float ppm_o3 = adc_mq131 * (5.0 / 1023.0) * 10.0;
    
    if (!dhtReadSuccess) dhtReadSuccess = readDhtWithRetry();
    
    // 2. Lectura PMS5003
    Serial.println(F("Leyendo datos del sensor PMS5003..."));
    lastPMSReadSuccess = readPMSDataToGlobal();
    
    // 3. Envío de datos al NodeMCU
    sendDataToNodeMCU(ppm_co, ppm_o3); 
    
    // 4. Log de sensores (incluido el PMS5003)
    Serial.print(F("Trama enviada: "));
    Serial.println(dataBuffer);
  }
  
  // Opcional: Reenvío de datos desde Serial al NodeMCU (para debug)
  if (Serial.available()) Serial1.write(Serial.read()); 
}
// --- FUNCIONES AUXILIARES DE SENSORES Y ENVÍO ---
/**
 * Empaqueta y envía todos los datos de sensores al NodeMCU a través de Serial1.
 * Formato de la trama (CSV): 
 * T,H,CO,O3,MQ135_ADC,NH3,NOx,CO2,PM1,PM25,PM10,SO2
 */
void sendDataToNodeMCU(float ppm_co, float ppm_o3) {
  char t_str[8], h_str[8], co_str[8], o3_str[8];
  char nh3_str[8], nox_str[8], co2_str[8];
  char so2_str[8];   // <-- NUEVO
  
  dtostrf(temperatura, 4, 1, t_str);
  dtostrf(humedad, 4, 1, h_str);
  dtostrf(ppm_co, 4, 1, co_str);
  dtostrf(ppm_o3, 4, 1, o3_str);
  dtostrf(ppm_nh3, 4, 1, nh3_str);
  dtostrf(ppm_nox, 4, 1, nox_str);
  dtostrf(ppm_co2, 5, 1, co2_str);
  dtostrf(ppm_so2, 4, 1, so2_str);   // <-- NUEVO
  int pm1 = pmsData.valid ? pmsData.pm1_0 : 0;
  int pm25 = pmsData.valid ? pmsData.pm2_5 : 0;
  int pm10 = pmsData.valid ? pmsData.pm10_0 : 0;
  
  sprintf(dataBuffer, "%s,%s,%s,%s,%d,%s,%s,%s,%d,%d,%d,%s",
          t_str, h_str, co_str, o3_str, adc_mq135,
          nh3_str, nox_str, co2_str, pm1, pm25, pm10,
          so2_str);   // <-- NUEVO campo al final
          
  Serial1.println(dataBuffer);
}
bool readPMSDataToGlobal() {
  pmsSerial.begin(9600);
  delay(100); 
  const unsigned long timeout = 3000; 
  unsigned long startTime = millis();
  uint8_t rx_buffer[32]; 
  int index = 0;
  bool success = false;
  
  while (pmsSerial.available()) pmsSerial.read();
  
  while (millis() - startTime < timeout && !success) {
    while (pmsSerial.available()) {
      uint8_t c = pmsSerial.read();
      
      if (index == 0 && c != 0x42) continue;
      if (index == 1 && c != 0x4D) {
        index = 0;
        continue;
      }
      
      if (index < 32) rx_buffer[index++] = c;
      
      if (index == 32) {
        uint16_t checksum = 0;
        for (uint8_t i = 0; i < 30; i++) {
          checksum += rx_buffer[i];
        }
        
        uint16_t receivedChecksum = (rx_buffer[30] << 8) | rx_buffer[31];
        
        if (checksum == receivedChecksum) {
          pmsData.pm1_0 = (rx_buffer[4] << 8) | rx_buffer[5];
          pmsData.pm2_5 = (rx_buffer[6] << 8) | rx_buffer[7];
          pmsData.pm10_0 = (rx_buffer[8] << 8) | rx_buffer[9];
          pmsData.valid = true;
          success = true;
        } 
        break;
      }
    }
    delay(10);
  }
  
  pmsSerial.end();
  delay(10);
  
  if (!success) {
    pmsData.valid = false;
  }
  
  return success;
}
bool readDhtWithRetry() {
  for (byte i = 0; i < 3; i++) {
    if (dht.read11(DHTPIN) == 0) {
      humedad = dht.getHumidity();
      temperatura = dht.getTemperature();
      if (temperatura > -40 && temperatura < 80 && humedad >= 0 && humedad <= 100) {
        return true;
      }
    }
    delay(500);
  }
  return false;
}
void calculateMQ135Gases(int adc_value) {
  float voltaje = adc_value * (5.0 / 1023.0);
  ppm_nh3 = constrain(voltaje * 10.0, 0, 200);
  ppm_nox = constrain(voltaje * 15.0, 0, 300);
  ppm_co2 = constrain(350.0 + (voltaje * 1000.0), 350, 10000);
}
/**
 * Cálculo aproximado de SO2 a partir del ADC del MQ136.
 * NOTA: El MQ136 es principalmente sensible a H2S y también responde a SO2.
 * Para mediciones de precisión se requiere calibración con gas patrón
 * y aplicar la curva logarítmica Rs/R0 del datasheet.
 * Esta fórmula es una estimación lineal, coherente con el estilo usado
 * para MQ7 y MQ131 en este proyecto.
 */
float calculateMQ136SO2(int adc_value) {
  float voltaje = adc_value * (5.0 / 1023.0);
  // Factor de escala aproximado: rango típico 1-200 ppm para SO2/H2S
  float ppm = voltaje * 40.0;
  return constrain(ppm, 0, 200);
}
