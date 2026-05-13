#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/power.h>

#include <DHTStable.h>
#include <SoftwareSerial.h>

/*
============================================================
 ARDUINO MEGA - SENSOR HUB LOW POWER
============================================================

FUNCIONAMIENTO:

1. Mega duerme en IDLE
2. Cada 10 minutos:
    - despierta PMS5003
    - espera calentamiento
    - lee TODOS los sensores
    - despierta ESP8266
    - envia CSV
    - PMS vuelve a dormir
3. ESP8266:
    - recibe CSV
    - conecta WiFi
    - envia MQTT
    - entra Deep Sleep

============================================================
*/

// ============================================================
// VERSION
// ============================================================

#define VERSION "v20.0.0 LOW POWER FINAL"

// ============================================================
// PINES
// ============================================================

#define DHTPIN          4

#define PMS_RX_PIN     12
#define PMS_TX_PIN     13
#define PMS_SET_PIN     7

#define MQ7_PIN        A0
#define MQ135_PIN      A1
#define MQ131_PIN      A2
#define MQ136_PIN      A5

// Mega pin 8 -> RST del ESP8266
#define NODEMCU_RST_PIN 8

// ============================================================
// TIEMPOS
// ============================================================

// 10 minutos
#define SEND_INTERVAL_MS 600000UL

// PMS5003 warmup
#define PMS_WARMUP_MS    30000UL

// ============================================================
// OBJETOS
// ============================================================

SoftwareSerial pmsSerial(PMS_RX_PIN, PMS_TX_PIN);

DHTStable dht;

// ============================================================
// VARIABLES
// ============================================================

unsigned long prevSendMillis = 0;

bool dhtReadSuccess = false;
bool lastPMSReadSuccess = false;

float temperatura = 0.0;
float humedad     = 0.0;

float ppm_nh3 = 0.0;
float ppm_nox = 0.0;
float ppm_co2 = 0.0;
float ppm_so2 = 0.0;

int adc_mq7   = 0;
int adc_mq135 = 0;
int adc_mq131 = 0;
int adc_mq136 = 0;

// ============================================================
// PMS DATA
// ============================================================

struct PMSData {

  uint16_t pm1_0  = 0;
  uint16_t pm2_5  = 0;
  uint16_t pm10_0 = 0;

  bool valid = false;
};

PMSData pmsData;

// ============================================================
// BUFFER CSV
// ============================================================

char dataBuffer[220];

// ============================================================
// PROTOTIPOS
// ============================================================

void entrarIdleSleep();

void pmsWakeUp();
void pmsSleep();

bool readPMSDataToGlobal();

bool readDhtWithRetry();

void calculateMQ135Gases(int adc_value);

float calculateMQ136SO2(int adc_value);

void despertarNodeMCU();

void sendDataToNodeMCU(
  float ppm_co,
  float ppm_o3,
  int mq135_adc
);

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(9600);

  Serial1.begin(9600);

  // PMS SET
  pinMode(PMS_SET_PIN, OUTPUT);

  // ESP8266 RESET
  pinMode(NODEMCU_RST_PIN, OUTPUT);

  digitalWrite(PMS_SET_PIN, HIGH);

  // ESP operando normal
  digitalWrite(NODEMCU_RST_PIN, HIGH);

  // ahorrar energía
  power_spi_disable();
  power_twi_disable();

  Serial.println();
  Serial.println(F("===================================="));
  Serial.println(F(" ARDUINO MEGA LOW POWER SENSOR HUB "));
  Serial.println(F("===================================="));

  Serial.print(F("Version: "));
  Serial.println(F(VERSION));

  Serial.println(F("Modo ultra low power activo"));

  // dormir PMS al inicio
  pmsSleep();

  prevSendMillis = millis();
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  unsigned long currentMillis = millis();

  // ==========================================================
  // NUEVO CICLO CADA 10 MIN
  // ==========================================================

  if (currentMillis - prevSendMillis >= SEND_INTERVAL_MS) {

    prevSendMillis = currentMillis;

    Serial.println();
    Serial.println(F("================================"));
    Serial.println(F(" NUEVO CICLO DE ENVIO "));
    Serial.println(F("================================"));

    // ======================================================
    // DESPERTAR PMS
    // ======================================================

    Serial.println(F("Despertando PMS5003..."));

    pmsWakeUp();

    Serial.println(F("Esperando calentamiento PMS..."));

    delay(PMS_WARMUP_MS);

    // ======================================================
    // LEER DHT11
    // ======================================================

    Serial.println(F("Leyendo DHT11..."));

    dhtReadSuccess = readDhtWithRetry();

    if (!dhtReadSuccess) {

      Serial.println(F("ERROR DHT11"));
    }

    // ======================================================
    // LEER MQ
    // ======================================================

    Serial.println(F("Leyendo sensores MQ..."));

    adc_mq7   = analogRead(MQ7_PIN);
    adc_mq135 = analogRead(MQ135_PIN);
    adc_mq131 = analogRead(MQ131_PIN);
    adc_mq136 = analogRead(MQ136_PIN);

    // ======================================================
    // CALCULAR GASES
    // ======================================================

    calculateMQ135Gases(adc_mq135);

    ppm_so2 = calculateMQ136SO2(adc_mq136);

    float ppm_co =
      adc_mq7 * (5.0 / 1023.0) * 20.0;

    float ppm_o3 =
      adc_mq131 * (5.0 / 1023.0) * 10.0;

    // ======================================================
    // LEER PMS5003
    // ======================================================

    Serial.println(F("Leyendo PMS5003..."));

    lastPMSReadSuccess =
      readPMSDataToGlobal();

    if (lastPMSReadSuccess) {

      Serial.println(F("Lectura PMS OK"));

    } else {

      Serial.println(F("ERROR PMS5003"));
    }

    // ======================================================
    // DORMIR PMS
    // ======================================================

    pmsSleep();

    Serial.println(F("PMS5003 dormido"));

    // ======================================================
    // DESPERTAR ESP8266
    // ======================================================

    despertarNodeMCU();

    // esperar boot del ESP
    delay(2000);

    // ======================================================
    // ENVIAR CSV
    // ======================================================

    sendDataToNodeMCU(
      ppm_co,
      ppm_o3,
      adc_mq135
    );

    Serial.println(F("Datos enviados al ESP8266"));

    Serial.print(F("CSV: "));
    Serial.println(dataBuffer);

    Serial.println(F("ESP8266 deberia entrar DeepSleep"));
  }

  // ==========================================================
  // IDLE SLEEP
  // ==========================================================

  entrarIdleSleep();
}

// ============================================================
// DESPERTAR NODEMCU
// ============================================================

void despertarNodeMCU() {

  Serial.println(F("Despertando NodeMCU..."));

  digitalWrite(NODEMCU_RST_PIN, LOW);

  delay(150);

  digitalWrite(NODEMCU_RST_PIN, HIGH);

  delay(500);

  Serial.println(F("NodeMCU despertado"));
}

// ============================================================
// ENVIAR CSV
// ============================================================

void sendDataToNodeMCU(
  float ppm_co,
  float ppm_o3,
  int mq135_adc
) {

  char t_str[8];
  char h_str[8];

  char co_str[8];
  char o3_str[8];

  char nh3_str[8];
  char nox_str[8];
  char co2_str[8];
  char so2_str[8];

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

  sprintf(
    dataBuffer,
    "%s,%s,%s,%s,%d,%s,%s,%s,%d,%d,%d,%s",

    t_str,
    h_str,

    co_str,
    o3_str,

    mq135_adc,

    nh3_str,
    nox_str,
    co2_str,

    pm1,
    pm25,
    pm10,

    so2_str
  );

  Serial1.println(dataBuffer);
}

// ============================================================
// PMS CONTROL
// ============================================================

void pmsWakeUp() {

  digitalWrite(PMS_SET_PIN, HIGH);

  Serial.println(F("PMS5003 despertado"));
}

void pmsSleep() {

  digitalWrite(PMS_SET_PIN, LOW);

  Serial.println(F("PMS5003 dormido"));
}

// ============================================================
// IDLE SLEEP
// ============================================================

void entrarIdleSleep() {

  set_sleep_mode(SLEEP_MODE_IDLE);

  sleep_enable();

  sleep_cpu();

  sleep_disable();
}

// ============================================================
// LEER PMS5003
// ============================================================

bool readPMSDataToGlobal() {

  pmsSerial.begin(9600);

  delay(100);

  const unsigned long timeout = 3000;

  unsigned long startTime = millis();

  uint8_t rx_buffer[32];

  int index = 0;

  bool success = false;

  while (pmsSerial.available()) {

    pmsSerial.read();
  }

  while (millis() - startTime < timeout && !success) {

    while (pmsSerial.available()) {

      uint8_t c = pmsSerial.read();

      if (index == 0 && c != 0x42) continue;

      if (index == 1 && c != 0x4D) {

        index = 0;

        continue;
      }

      if (index < 32) {

        rx_buffer[index++] = c;
      }

      if (index == 32) {

        uint16_t checksum = 0;

        for (uint8_t i = 0; i < 30; i++) {

          checksum += rx_buffer[i];
        }

        uint16_t receivedChecksum =
          (rx_buffer[30] << 8) | rx_buffer[31];

        if (checksum == receivedChecksum) {

          pmsData.pm1_0 =
            (rx_buffer[4] << 8) | rx_buffer[5];

          pmsData.pm2_5 =
            (rx_buffer[6] << 8) | rx_buffer[7];

          pmsData.pm10_0 =
            (rx_buffer[8] << 8) | rx_buffer[9];

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

// ============================================================
// DHT11
// ============================================================

bool readDhtWithRetry() {

  for (byte i = 0; i < 3; i++) {

    if (dht.read11(DHTPIN) == 0) {

      float h = dht.getHumidity();

      float t = dht.getTemperature();

      if (
        t > -40 &&
        t < 80 &&
        h >= 0 &&
        h <= 100
      ) {

        humedad     = h;
        temperatura = t;

        return true;
      }
    }

    delay(500);
  }

  return false;
}

// ============================================================
// MQ135
// ============================================================

void calculateMQ135Gases(int adc_value) {

  float voltaje =
    adc_value * (5.0 / 1023.0);

  ppm_nh3 =
    constrain(voltaje * 10.0, 0, 200);

  ppm_nox =
    constrain(voltaje * 15.0, 0, 300);

  ppm_co2 =
    constrain(
      350.0 + (voltaje * 1000.0),
      350,
      10000
    );
}

// ============================================================
// MQ136
// ============================================================

float calculateMQ136SO2(int adc_value) {

  float voltaje =
    adc_value * (5.0 / 1023.0);

  return constrain(
    voltaje * 40.0,
    0,
    200
  );
}