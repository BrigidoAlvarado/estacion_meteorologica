#include <avr/pgmspace.h>
#include <avr/sleep.h>       // para dormir 
#include <avr/power.h>       // para apagar los perifericos no usaros
#include <DHTStable.h>
#include <SoftwareSerial.h>

/*
MQ:
- siempre calientes
- muestras cada 30 s
- promedio cada 10 min
PMS:
- duerme 9 min 30 s
- despierta 30 s antes
- estabiliza
- toma 1 lectura
- duerme otra vez

Mega:
- idle sleep entre ciclos , esto porque necesita d los timers
*/

// --- Versión ---
#define VERSION "v19.0.0 (optimizacion de consumo)"

// pines
// Serial1 (Pines 18/19) → NodeMCU
#define DHTPIN        4
#define PMS_RX_PIN   12
#define PMS_TX_PIN   13
#define PMS_SET_PIN   7    // pin set del pms necesario par poder dormirlo / levantarlo
#define MQ7_PIN      A0
#define MQ135_PIN    A1
#define MQ131_PIN    A2
#define MQ136_PIN    A5

// configs de tiempos y muestras , se usa UL aca porque hay funciones que retornan longs
#define SAMPLE_INTERVAL   30000UL   // 30 seg entre cada muestra
#define DHT_READ_INTERVAL  3000UL   // 3 seg entre lecturas DHT
#define NUM_SAMPLES           20    // 20 muestras × 30s = 600s ≈ 10 minutos
// tiempo para calentar al pms
#define PMS_WARMUP_MS      30000UL

// objetos
SoftwareSerial pmsSerial(PMS_RX_PIN, PMS_TX_PIN);
DHTStable dht;

// control del tiempo
unsigned long prevSensorMillis = 0;
unsigned long prevDhtMillis    = 0;

// estados
bool dhtReadSuccess    = false;
bool lastPMSReadSuccess = false;

// variables de los sensores, ahora se inicializan en 0
float temperatura = 0.0, humedad = 0.0;
float ppm_nh3 = 0.0, ppm_nox = 0.0, ppm_co2 = 0.0;
int   adc_mq7 = 0, adc_mq135 = 0, adc_mq131 = 0, adc_mq136 = 0;
float ppm_so2 = 0.0;

struct PMSData {
  uint16_t pm1_0  = 0;
  uint16_t pm2_5  = 0;
  uint16_t pm10_0 = 0;
  bool valid = false;
};
PMSData pmsData;

// Esta e suna nueva estructura que acumula la suma de lecturas, 
// al llegar a la cantidad estipulada se divide y se envia el promedio, se veria como data real

struct Accumulator {
  float temp = 0, hum = 0;
  float co = 0, o3 = 0;
  float nh3 = 0, nox = 0, co2 = 0, so2 = 0;
  long  pm1 = 0, pm25 = 0, pm10 = 0;
  int   mq135_adc = 0;   
  int   count  = 0;      // contador de muestras
};
Accumulator acc;

// buffer tramas
char dataBuffer[220];

// prototipos
void  sendDataToNodeMCU(float ppm_co, float ppm_o3, int mq135_adc_avg);
bool  readDhtWithRetry();
bool  readPMSDataToGlobal();
void  calculateMQ135Gases(int adc_value);
float calculateMQ136SO2(int adc_value);
void  acumularMuestra(float ppm_co, float ppm_o3);
void  enviarPromedio();
void  resetAccumulator();
void  pmsWakeUp();
void  pmsSleep();
void  entrarIdleSleep();

//  setup
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

void setup() {
  Serial.begin(9600);
  Serial1.begin(9600);

  // Pin SET del PMS500 arranca con HIGH que es activo
  pinMode(PMS_SET_PIN, OUTPUT);
  digitalWrite(PMS_SET_PIN, HIGH);

  power_spi_disable();  // estos perifericos consumen energia y no se usan jsj
  power_twi_disable();

  Serial.print(F("\n--- ARDUINO MEGA SENSOR HUB ("));
  Serial.println(F(") ---"));
  Serial.println(F("  SLEEP_MODE_IDLE entre muestras"));
  Serial.println(F("  PMS5003 duerme entre lecturas"));
  Serial.print(F("  Envio cada ~"));
  Serial.print((SAMPLE_INTERVAL / 1000UL) * NUM_SAMPLES / 60UL);
  Serial.print(F(" minutos (promedio de "));
  Serial.print(NUM_SAMPLES);
  Serial.println(F(" muestras)"));

  dhtReadSuccess = readDhtWithRetry();

  // aca se apaga el pms
  pmsSleep();
  prevSensorMillis = millis();
  prevDhtMillis = millis();
}

// loop
void loop() {
  unsigned long currentMillis = millis();

  // --- 1. Lectura DHT cada 3 segundos ---
  if (currentMillis - prevDhtMillis >= DHT_READ_INTERVAL) {
    prevDhtMillis = currentMillis;
    dhtReadSuccess = readDhtWithRetry();
  }

  // --- 2. Ciclo de muestreo cada 30 segundos ---
  if (currentMillis - prevSensorMillis >= SAMPLE_INTERVAL) {
    prevSensorMillis = currentMillis;

    Serial.print(F("\n[Muestra "));
    Serial.print(acc.count + 1);
    Serial.print(F("/"));
    Serial.print(NUM_SAMPLES);
    Serial.println(F("]"));

    // 2a. Lectura de sensores MQ (siempre activos, lectura instantánea)
    adc_mq7   = analogRead(MQ7_PIN);
    adc_mq135 = analogRead(MQ135_PIN);
    adc_mq131 = analogRead(MQ131_PIN);
    adc_mq136 = analogRead(MQ136_PIN);
    calculateMQ135Gases(adc_mq135);
    ppm_so2 = calculateMQ136SO2(adc_mq136);

    float ppm_co = adc_mq7  * (5.0 / 1023.0) * 20.0;
    float ppm_o3 = adc_mq131 * (5.0 / 1023.0) * 10.0;

    // 2b. Asegurar lectura DHT válida
    if (!dhtReadSuccess) dhtReadSuccess = readDhtWithRetry();

    // Aca el flujo del pms seria, despertar, calentarse , leer y volver a dormir
    
    // se acumula en el promediador
    acumularMuestra(ppm_co, ppm_o3);
    
    // si se llega  ala cantidad de tramas, se primedia y se envia jsjs
    if (acc.count >= NUM_SAMPLES) {
        Serial.println(F("Despertando PMS5003 para lectura final"));
        pmsWakeUp();
        delay(PMS_WARMUP_MS);
        lastPMSReadSuccess = readPMSDataToGlobal();
        pmsSleep();
        enviarPromedio();
    }
  }
  // entrar en idle sleep para ahorrar energía entre muestras
  // el CPU se detiene pero millis() sigue corriendo.
  entrarIdleSleep();
}

// Acumulador, aca se agrega una muestra al promediador
void acumularMuestra(float ppm_co, float ppm_o3) {
  acc.temp += temperatura;
  acc.hum += humedad;
  acc.co += ppm_co;
  acc.o3 += ppm_o3;
  acc.nh3 += ppm_nh3;
  acc.nox += ppm_nox;
  acc.co2 += ppm_co2;
  acc.so2 += ppm_so2;
  acc.mq135_adc += adc_mq135;
  acc.count++;

  Serial.print(F("  Acumuladas: T="));
  Serial.print(temperatura, 1);
  Serial.print(F(" CO="));
  Serial.print(ppm_co, 1);
  Serial.print(F(" SO2="));
  Serial.println(ppm_so2, 1);
}
// Aca envia el promedio
void enviarPromedio() {
  float n = (float)acc.count;

  temperatura = acc.temp  / n;
  humedad = acc.hum / n;
  ppm_nh3 = acc.nh3 / n;
  ppm_nox = acc.nox / n;
  ppm_co2 = acc.co2 / n;
  ppm_so2 = acc.so2 / n;

  float avg_co = acc.co / n;
  float avg_o3 = acc.o3 / n;
  int   avg_mq135 = (int)(acc.mq135_adc / n);

  // solo si el pms tuvo lecturas validas
  Serial.println(F("\n Enviando promedio al NodeMCU"));
  Serial.print(F("  Muestras promediadas: "));
  Serial.println(acc.count);

  sendDataToNodeMCU(avg_co, avg_o3, avg_mq135);

  Serial.print(F("  Trama: "));
  Serial.println(dataBuffer);

  // Resetear acumulador para el siguiente ciclo
  resetAccumulator();
}

// reseteador del acumulador
void resetAccumulator() {
  acc.temp = acc.hum = acc.co = acc.o3 = 0;
  acc.nh3  = acc.nox = acc.co2 = acc.so2 = 0;
  acc.pm1  = acc.pm25 = acc.pm10 = 0;
  acc.mq135_adc = 0;
  acc.count = 0;
}
// --- FUNCIONES AUXILIARES DE SENSORES Y ENVÍO ---
/**
 * Empaqueta y envía todos los datos de sensores al NodeMCU a través de Serial1.
 * Formato de la trama (CSV): 
 * T,H,CO,O3,MQ135_ADC,NH3,NOx,CO2,PM1,PM25,PM10,SO2
 */
void sendDataToNodeMCU(float ppm_co, float ppm_o3, int mq135_adc_avg) {
  char t_str[8], h_str[8], co_str[8], o3_str[8];
  char nh3_str[8], nox_str[8], co2_str[8], so2_str[8];

  dtostrf(temperatura, 4, 1, t_str);
  dtostrf(humedad, 4, 1, h_str);
  dtostrf(ppm_co, 4, 1, co_str);
  dtostrf(ppm_o3, 4, 1, o3_str);
  dtostrf(ppm_nh3, 4, 1, nh3_str);
  dtostrf(ppm_nox, 4, 1, nox_str);
  dtostrf(ppm_co2, 5, 1, co2_str);
  dtostrf(ppm_so2, 4, 1, so2_str);

  int pm1  = pmsData.valid ? pmsData.pm1_0  : 0;
  int pm25 = pmsData.valid ? pmsData.pm2_5  : 0;
  int pm10 = pmsData.valid ? pmsData.pm10_0 : 0;

  sprintf(dataBuffer, "%s,%s,%s,%s,%d,%s,%s,%s,%d,%d,%d,%s",
          t_str, h_str, co_str, o3_str, mq135_adc_avg,
          nh3_str, nox_str, co2_str, pm1, pm25, pm10, so2_str);

  Serial1.println(dataBuffer);
}

//  PMS5003 aca se controla el encendido y apagado por medio del pin SET
//  HIGH = modo activo 
//  LOW  = modo sleep 
void pmsWakeUp() {
  digitalWrite(PMS_SET_PIN, HIGH);
  Serial.println(F("  PMS5003 despertado"));
}

void pmsSleep() {
  digitalWrite(PMS_SET_PIN, LOW);
  Serial.println(F("  PMS5003 dormido"));
}

//  IDLE SLEEP detiene el CPU pero mantiene timers y periféricos activos.
//  millis() sigue funcionando, los timers del loop no se afectan.
void entrarIdleSleep() {
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();
  sleep_cpu();        
  sleep_disable();    
}

//  Lectura del pms

bool readPMSDataToGlobal() {
  pmsSerial.begin(9600);
  delay(100);

  const unsigned long timeout = 3000;
  unsigned long startTime = millis();
  uint8_t rx_buffer[32];
  int     index = 0;
  bool    success = false;

  while (pmsSerial.available()) pmsSerial.read();

  while (millis() - startTime < timeout && !success) {
    while (pmsSerial.available()) {
      uint8_t c = pmsSerial.read();

      if (index == 0 && c != 0x42) continue;
      if (index == 1 && c != 0x4D) { index = 0; continue; }
      if (index < 32) rx_buffer[index++] = c;

      if (index == 32) {
        uint16_t checksum = 0;
        for (uint8_t i = 0; i < 30; i++) checksum += rx_buffer[i];

        uint16_t receivedChecksum = (rx_buffer[30] << 8) | rx_buffer[31];

        if (checksum == receivedChecksum) {
          pmsData.pm1_0  = (rx_buffer[4] << 8) | rx_buffer[5];
          pmsData.pm2_5  = (rx_buffer[6] << 8) | rx_buffer[7];
          pmsData.pm10_0 = (rx_buffer[8] << 8) | rx_buffer[9];
          pmsData.valid  = true;
          success = true;
        }
        break;
      }
    }
    delay(10);
  }

  pmsSerial.end();
  delay(10);

  if (!success) pmsData.valid = false;
  return success;
}

//  LECTURA DHT11 
bool readDhtWithRetry() {
  for (byte i = 0; i < 3; i++) {
    if (dht.read11(DHTPIN) == 0) {
      float h = dht.getHumidity();
      float t = dht.getTemperature();
      if (t > -40 && t < 80 && h >= 0 && h <= 100) {
        humedad     = h;
        temperatura = t;
        return true;
      }
    }
    delay(500);
  }
  return false;
}

void calculateMQ135Gases(int adc_value) {
  float voltaje = adc_value * (5.0 / 1023.0);
  ppm_nh3 = constrain(voltaje * 10.0,           0,   200);
  ppm_nox = constrain(voltaje * 15.0,           0,   300);
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
  return constrain(voltaje * 40.0, 0, 200);
}
