/**
 * @file empacadora_vertical_esp32.ino
 * @brief Firmware de control para Empacadora Vertical Automática de Frutas (VFFS) con ESP32
 * @institution Escuela Superior Politécnica del Litoral (ESPOL) - FIEC
 * @course Sistemas Embebidos - Paralelo 2
 * @authors
 *   - Allan Moises Chacha Leon (Matrícula: 202304135)
 *   - Axel Andrés Alejandro Jiménez (Matrícula: 202213161)
 * @date 17/11/2026
 *
 * @note Seguridad: No incluir credenciales reales de WiFi, ThingSpeak ni Telegram
 *       al subir este archivo a repositorios públicos en GitHub.
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HX711.h>
#include <max6675.h>

// ============================================================================
// 1. DEFINICIÓN DE PINES (ESPECIFICACIÓN SECCIÓN 6 - DISEÑO DE INTERFACES)
// ============================================================================

// Interfaz Usuario - Máquina (OLED I2C + 3 Pulsadores + Buzzer)
#define PIN_OLED_SDA      21
#define PIN_OLED_SCL      22
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define OLED_ADDR         0x3C

#define PIN_BTN_UP        32  // INPUT_PULLUP
#define PIN_BTN_DOWN      33  // INPUT_PULLUP
#define PIN_BTN_OK        25  // INPUT_PULLUP
#define PIN_BUZZER        4

// Sensores (Lógica 3.3 V)
#define PIN_IR_SENSOR     34  // E18-D80NK con adaptación de nivel a 3.3 V (solo entrada)
#define PIN_HX711_DT      16
#define PIN_HX711_SCK     17

#define PIN_SPI_SCK       18
#define PIN_SPI_MISO      19
#define PIN_MAX6675_CS1   5   // Termopar 1: Selladora horizontal SETE-30
#define PIN_MAX6675_CS2   14  // Termopar 2: Selladora vertical SETE-20

// Finales de carrera (GPIO 35, 36, 39 requieren resistencia pull-up externa de 10k a 3.3V)
#define PIN_FC_MORDAZAS   35  // Final de carrera mordazas cerradas (activo en LOW)
#define PIN_FC_COMPUERTA  36  // Final de carrera compuerta molde (activo en LOW)
#define PIN_FC_PARK       39  // Final de carrera posición park / abiertas (activo en LOW)

// Actuadores: Driver A4988 (NEMA 17 en Full-Step: 200 pasos/rev, polea Ø 7.6 cm)
#define PIN_STEP          26
#define PIN_DIR           27
#define PIN_EN            13  // Activo en LOW

// Actuadores: Módulo de 4 Relés 5 V (ajustar RELAY_ACTIVE si es activo en LOW o HIGH)
#define PIN_RELAY_WIPER   23  // Motor limpiaparabrisas 12 V (mordazas horizontales)
#define PIN_RELAY_SETE30  15  // Selladora horizontal SETE-30 (paralelo a microswitch)
#define PIN_RELAY_SETE20  2   // Selladora vertical SETE-20 (paralelo a microswitch)
#define PIN_RELAY_SEGUROS 12  // Actuadores de seguro 12 V (barra vertical / compuerta)

#define RELAY_ON          HIGH
#define RELAY_OFF         LOW

// ============================================================================
// 2. CONSTANTES DE PROCESO Y CREDENCIALES DE EJEMPLO (PLACEHOLDERS)
// ============================================================================

// 184 pasos = 22 cm con polea de 7.6 cm -> 92 pasos por tramo de 11 cm
constexpr int PASOS_TRAMO_11CM      = 92;
constexpr unsigned long STEP_US     = 1800;   // Velocidad de pulso NEMA 17

// Tiempos y umbrales de seguridad
constexpr unsigned long TIMEOUT_MORDAZAS_MS   = 4000;
constexpr unsigned long TIEMPO_IMPULSO_SELLO  = 1200;
constexpr unsigned long TIEMPO_ENFRIAMIENTO   = 1500;
constexpr unsigned long INTERVALO_THINGSPEAK  = 15000; // Mínimo 15 s API gratuita

constexpr float TEMP_MIN_SELLO_C    = 95.0f;
constexpr float TEMP_MAX_SELLO_C    = 185.0f;

// Credenciales IoT (REEMPLAZAR LOCALMENTE, NO SUBIR CLAVES REALES A GITHUB)
const char* THINGSPEAK_API_KEY      = "TU_API_KEY_THINGSPEAK";
const char* TELEGRAM_BOT_TOKEN      = "TU_BOT_TOKEN_TELEGRAM";
const char* TELEGRAM_CHAT_ID        = "TU_CHAT_ID_TELEGRAM";

// ============================================================================
// 3. OBJETOS GLOBALES Y MÁQUINA DE ESTADOS (FIGURA 3)
// ============================================================================

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
HX711 scale;
MAX6675 termoparHoriz(PIN_SPI_SCK, PIN_MAX6675_CS1, PIN_SPI_MISO);
MAX6675 termoparVert(PIN_SPI_SCK, PIN_MAX6675_CS2, PIN_SPI_MISO);

Preferences prefs;
WebServer server(80);

enum EstadoSistema {
  INICIO,
  PORTAL_AP,
  REPOSO,
  SELLO_FONDO_INICIAL,
  ARRASTRE_1,
  SELLO_VERT_1,
  ARRASTRE_2,
  SELLO_VERT_2,
  LLENADO,
  CIERRE_MORDAZAS,
  SELLO_HORIZONTAL,
  DESCARGA,
  REGISTRO,
  ALARMA
};

EstadoSistema estadoActual = INICIO;
String mensajeAlarma = "";

// Perfiles de fruta y configuración operativa
struct PerfilFruta {
  const char* nombre;
  float pesoUnitarioG;
  int limitePorFunda;
};

PerfilFruta perfiles[] = {
  {"Limon sutil",   45.0f, 5},
  {"Mandarina",     95.0f, 4},
  {"Naranja",      160.0f, 3}
};
const int NUM_PERFILES = sizeof(perfiles) / sizeof(perfiles[0]);
int perfilIdx = 0;

int frutasContadas = 0;
int totalFundasProducidas = 0;
float pesoTaraFundaG = 0.0f;
float ultimaTempHoriz = 0.0f;
float ultimaTempVert  = 0.0f;
unsigned long ultimoEnvioTS = 0;

// Credenciales WiFi almacenadas en NVS/EEPROM
String ssid1, pass1, ssid2, pass2;

// ============================================================================
// 4. FUNCIONES AUXILIARES DE SEGURIDAD, INTERFAZ Y CONECTIVIDAD
// ============================================================================

void apagarTodosLosActuadores() {
  digitalWrite(PIN_RELAY_WIPER, RELAY_OFF);
  digitalWrite(PIN_RELAY_SETE30, RELAY_OFF);
  digitalWrite(PIN_RELAY_SETE20, RELAY_OFF);
  digitalWrite(PIN_RELAY_SEGUROS, RELAY_OFF);
  digitalWrite(PIN_EN, HIGH); // Deshabilita driver A4988
}

void dispararAlarma(const String& motivo) {
  apagarTodosLosActuadores();
  mensajeAlarma = motivo;
  estadoActual = ALARMA;
  Serial.println("[ALARMA] " + motivo);
}

bool botonPresionado(uint8_t pin) {
  if (digitalRead(pin) == LOW) {
    delay(40); // Antirrebote básico
    if (digitalRead(pin) == LOW) {
      while (digitalRead(pin) == LOW) delay(10);
      return true;
    }
  }
  return false;
}

void actualizarOLED(const String& linea1, const String& linea2, const String& linea3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("EMPACADORA VFFS ESP32"));
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 16);
  display.println(linea1);
  display.setCursor(0, 32);
  display.println(linea2);
  display.setCursor(0, 48);
  display.println(linea3);
  display.display();
}

void avanzarLaminaPasos(int pasos) {
  digitalWrite(PIN_EN, LOW);   // Habilita A4988
  digitalWrite(PIN_DIR, HIGH); // Sentido de descenso de lámina
  for (int i = 0; i < pasos; i++) {
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(STEP_US);
    digitalWrite(PIN_STEP, LOW);
    delayMicroseconds(STEP_US);
  }
  digitalWrite(PIN_EN, HIGH);
}

bool conectarWiFiGuardado() {
  prefs.begin("wifi-cfg", true);
  ssid1 = prefs.getString("ssid1", "");
  pass1 = prefs.getString("pass1", "");
  ssid2 = prefs.getString("ssid2", "");
  pass2 = prefs.getString("pass2", "");
  prefs.end();

  const char* ssids[2] = {ssid1.c_str(), ssid2.c_str()};
  const char* passes[2] = {pass1.c_str(), pass2.c_str()};

  for (int r = 0; r < 2; r++) {
    if (strlen(ssids[r]) == 0) continue;
    actualizarOLED("Conectando WiFi...", String("Red ") + (r + 1) + ": " + ssids[r]);
    WiFi.begin(ssids[r], passes[r]);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
      return true;
    }
    WiFi.disconnect(true);
  }
  return false;
}

void iniciarPortalAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Selladora-Config"); // IP por defecto: 192.168.4.1

  server.on("/", HTTP_GET, []() {
    String html = F(
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Configuracion WiFi - Empacadora</title></head><body>"
      "<h2>Portal WiFi - Selladora ESP32</h2>"
      "<form action='/save' method='POST'>"
      "SSID Red 1:<br><input name='s1' required><br>"
      "Clave Red 1:<br><input name='p1' type='password'><br><br>"
      "SSID Red 2 (Respaldo):<br><input name='s2'><br>"
      "Clave Red 2:<br><input name='p2' type='password'><br><br>"
      "<button type='submit'>Guardar y Conectar</button>"
      "</form></body></html>"
    );
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    prefs.begin("wifi-cfg", false);
    prefs.putString("ssid1", server.arg("s1"));
    prefs.putString("pass1", server.arg("p1"));
    prefs.putString("ssid2", server.arg("s2"));
    prefs.putString("pass2", server.arg("p2"));
    prefs.end();

    server.send(200, "text/html", "<h3>Credenciales guardadas. Reiniciando ESP32...</h3>");
    delay(1500);
    ESP.restart();
  });

  server.begin();
  actualizarOLED("Modo AP Activo", "SSID: Selladora-Config", "IP: 192.168.4.1");
}

void enviarTelegramAlerta(const String& texto) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + "/sendMessage";
  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    String payload = "{"chat_id":"" + String(TELEGRAM_CHAT_ID) +
                     "","text":"[ALARMA EMPACADORA] " + texto + ""}";
    https.POST(payload);
    https.end();
  }
}

void enviarTelemetriaThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - ultimoEnvioTS < INTERVALO_THINGSPEAK) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String("https://api.thingspeak.com/update?api_key=") + THINGSPEAK_API_KEY +
               "&field1=" + String(totalFundasProducidas) +
               "&field2=" + String(frutasContadas) +
               "&field3=" + String(ultimaTempHoriz, 1) +
               "&field4=" + String(ultimaTempVert, 1);

  if (https.begin(client, url)) {
    https.GET();
    https.end();
    ultimoEnvioTS = millis();
  }
}

// ============================================================================
// 5. SETUP Y BUCLE PRINCIPAL (MÁQUINA DE ESTADOS FINITOS)
// ============================================================================

void setup() {
  Serial.begin(115200);

  // Configuración segura de salidas antes de energizar etapas de potencia
  pinMode(PIN_RELAY_WIPER, OUTPUT);
  pinMode(PIN_RELAY_SETE30, OUTPUT);
  pinMode(PIN_RELAY_SETE20, OUTPUT);
  pinMode(PIN_RELAY_SEGUROS, OUTPUT);
  pinMode(PIN_EN, OUTPUT);
  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  apagarTodosLosActuadores();

  // Entradas
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_BTN_OK, INPUT_PULLUP);
  pinMode(PIN_IR_SENSOR, INPUT);
  pinMode(PIN_FC_MORDAZAS, INPUT);
  pinMode(PIN_FC_COMPUERTA, INPUT);
  pinMode(PIN_FC_PARK, INPUT);

  // Inicialización OLED
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);

  // Inicialización Celda de Carga HX711 (ajustar factor de calibración tras tara)
  scale.begin(PIN_HX711_DT, PIN_HX711_SCK);
  scale.set_scale(420.0f);
  scale.tare();

  estadoActual = INICIO;
}

void loop() {
  // Supervisión térmica continua en operación
  ultimaTempHoriz = termoparHoriz.readCelsius();
  ultimaTempVert  = termoparVert.readCelsius();
  if (estadoActual != ALARMA && (ultimaTempHoriz > TEMP_MAX_SELLO_C || ultimaTempVert > TEMP_MAX_SELLO_C)) {
    dispararAlarma("Sobretemperatura en selladora");
    enviarTelegramAlerta(mensajeAlarma);
  }

  switch (estadoActual) {
    case INICIO: {
      if (conectarWiFiGuardado()) {
        estadoActual = REPOSO;
      } else {
        iniciarPortalAP();
        estadoActual = PORTAL_AP;
      }
      break;
    }

    case PORTAL_AP: {
      server.handleClient();
      break;
    }

    case REPOSO: {
      PerfilFruta& p = perfiles[perfilIdx];
      actualizarOLED(
        "Perfil: " + String(p.nombre),
        "Limite: " + String(p.limitePorFunda) + " frutas",
        "[OK] Iniciar ciclo"
      );

      if (botonPresionado(PIN_BTN_UP)) {
        perfilIdx = (perfilIdx + 1) % NUM_PERFILES;
      }
      if (botonPresionado(PIN_BTN_DOWN)) {
        perfilIdx = (perfilIdx - 1 + NUM_PERFILES) % NUM_PERFILES;
      }
      if (botonPresionado(PIN_BTN_OK)) {
        scale.tare();
        estadoActual = SELLO_FONDO_INICIAL;
      }
      break;
    }

    case SELLO_FONDO_INICIAL: {
      actualizarOLED("Iniciando lote...", "Sellando fondo 1ra", "funda (SETE-30)");
      // Cierra mordazas hasta FC y aplica impulso de sellado horizontal inicial
      unsigned long t0 = millis();
      digitalWrite(PIN_RELAY_WIPER, RELAY_ON);
      while (digitalRead(PIN_FC_MORDAZAS) != LOW) {
        if (millis() - t0 > TIMEOUT_MORDAZAS_MS) {
          dispararAlarma("Atasco mordazas en sello inicial");
          enviarTelegramAlerta(mensajeAlarma);
          return;
        }
        delay(10);
      }
      digitalWrite(PIN_RELAY_WIPER, RELAY_OFF);

      digitalWrite(PIN_RELAY_SETE30, RELAY_ON);
      delay(TIEMPO_IMPULSO_SELLO);
      digitalWrite(PIN_RELAY_SETE30, RELAY_OFF);
      delay(TIEMPO_ENFRIAMIENTO);

      // Abre mordazas a posición park
      t0 = millis();
      digitalWrite(PIN_RELAY_WIPER, RELAY_ON);
      while (digitalRead(PIN_FC_PARK) != LOW) {
        if (millis() - t0 > TIMEOUT_MORDAZAS_MS) {
          dispararAlarma("Atasco apertura mordazas");
          enviarTelegramAlerta(mensajeAlarma);
          return;
        }
        delay(10);
      }
      digitalWrite(PIN_RELAY_WIPER, RELAY_OFF);

      estadoActual = ARRASTRE_1;
      break;
    }

    case ARRASTRE_1: {
      actualizarOLED("Formando tubo...", "Arrastre tramo 1/2", "(11 cm)");
      avanzarLaminaPasos(PASOS_TRAMO_11CM);
      estadoActual = SELLO_VERT_1;
      break;
    }

    case SELLO_VERT_1: {
      actualizarOLED("Formando tubo...", "Sello vertical 1/2", "(SETE-20)");
      digitalWrite(PIN_RELAY_SEGUROS, RELAY_ON); // Actuador presiona barra vertical
      delay(250);
      digitalWrite(PIN_RELAY_SETE20, RELAY_ON);
      delay(TIEMPO_IMPULSO_SELLO);
      digitalWrite(PIN_RELAY_SETE20, RELAY_OFF);
      delay(TIEMPO_ENFRIAMIENTO);
      digitalWrite(PIN_RELAY_SEGUROS, RELAY_OFF);

      estadoActual = ARRASTRE_2;
      break;
    }

    case ARRASTRE_2: {
      actualizarOLED("Formando tubo...", "Arrastre tramo 2/2", "(22 cm totales)");
      avanzarLaminaPasos(PASOS_TRAMO_11CM);
      estadoActual = SELLO_VERT_2;
      break;
    }

    case SELLO_VERT_2: {
      actualizarOLED("Formando tubo...", "Sello vertical 2/2", "(SETE-20)");
      digitalWrite(PIN_RELAY_SEGUROS, RELAY_ON);
      delay(250);
      digitalWrite(PIN_RELAY_SETE20, RELAY_ON);
      delay(TIEMPO_IMPULSO_SELLO);
      digitalWrite(PIN_RELAY_SETE20, RELAY_OFF);
      delay(TIEMPO_ENFRIAMIENTO);
      digitalWrite(PIN_RELAY_SEGUROS, RELAY_OFF);

      frutasContadas = 0;
      pesoTaraFundaG = scale.get_units(5);
      estadoActual = LLENADO;
      break;
    }

    case LLENADO: {
      PerfilFruta& p = perfiles[perfilIdx];
      actualizarOLED(
        "Llenando funda...",
        "Conteo: " + String(frutasContadas) + " / " + String(p.limitePorFunda),
        "IR + Celda HX711"
      );

      // Detección de caída por sensor IR y estimación por incremento de peso
      if (digitalRead(PIN_IR_SENSOR) == LOW) {
        delay(180); // Espera estabilización mecánica de la caída en la funda
        float pesoActualG = scale.get_units(5) - pesoTaraFundaG;
        int estimadas = max(1, (int)round(pesoActualG / p.pesoUnitarioG));
        frutasContadas = estimadas;

        if (frutasContadas > p.limitePorFunda) {
          dispararAlarma("Exceso de frutas (" + String(frutasContadas) + ")");
          enviarTelegramAlerta(mensajeAlarma);
        } else if (frutasContadas == p.limitePorFunda) {
          estadoActual = CIERRE_MORDAZAS;
        }
      }
      break;
    }

    case CIERRE_MORDAZAS: {
      actualizarOLED("Cerrando mordazas...", "Verificando FC", "cierre horizontal");
      unsigned long t0 = millis();
      digitalWrite(PIN_RELAY_WIPER, RELAY_ON);
      while (digitalRead(PIN_FC_MORDAZAS) != LOW) {
        if (millis() - t0 > TIMEOUT_MORDAZAS_MS) {
          dispararAlarma("Atasco mecanico en mordazas");
          enviarTelegramAlerta(mensajeAlarma);
          return;
        }
        delay(10);
      }
      digitalWrite(PIN_RELAY_WIPER, RELAY_OFF);
      estadoActual = SELLO_HORIZONTAL;
      break;
    }

    case SELLO_HORIZONTAL: {
      actualizarOLED("Sellado horizontal", "SETE-30 + enfriado", "T: " + String(ultimaTempHoriz, 1) + " C");
      digitalWrite(PIN_RELAY_SETE30, RELAY_ON);
      delay(TIEMPO_IMPULSO_SELLO);
      digitalWrite(PIN_RELAY_SETE30, RELAY_OFF);
      delay(TIEMPO_ENFRIAMIENTO);

      ultimaTempHoriz = termoparHoriz.readCelsius();
      if (ultimaTempHoriz < TEMP_MIN_SELLO_C || ultimaTempHoriz > TEMP_MAX_SELLO_C) {
        dispararAlarma("Mal sellado: T fuera de rango (" + String(ultimaTempHoriz, 1) + " C)");
        enviarTelegramAlerta(mensajeAlarma);
      } else {
        estadoActual = DESCARGA;
      }
      break;
    }

    case DESCARGA: {
      actualizarOLED("Descargando funda", "Abriendo mordazas", "y compuerta");
      unsigned long t0 = millis();
      digitalWrite(PIN_RELAY_WIPER, RELAY_ON);
      while (digitalRead(PIN_FC_PARK) != LOW) {
        if (millis() - t0 > TIMEOUT_MORDAZAS_MS) {
          dispararAlarma("Atasco en apertura de descarga");
          enviarTelegramAlerta(mensajeAlarma);
          return;
        }
        delay(10);
      }
      digitalWrite(PIN_RELAY_WIPER, RELAY_OFF);

      // Acciona compuerta del molde para liberar la funda
      digitalWrite(PIN_RELAY_SEGUROS, RELAY_ON);
      delay(600);
      digitalWrite(PIN_RELAY_SEGUROS, RELAY_OFF);

      totalFundasProducidas++;
      estadoActual = REGISTRO;
      break;
    }

    case REGISTRO: {
      actualizarOLED("Registrando ciclo", "Funda #" + String(totalFundasProducidas), "Subiendo ThingSpeak");
      enviarTelemetriaThingSpeak();
      estadoActual = ARRASTRE_1; // Inicia ciclo continuo de la siguiente funda
      break;
    }

    case ALARMA: {
      apagarTodosLosActuadores();
      actualizarOLED("!! ALARMA ACTIVA !!", mensajeAlarma, "[OK] Rearmar sistema");
      digitalWrite(PIN_BUZZER, HIGH);
      delay(200);
      digitalWrite(PIN_BUZZER, LOW);
      delay(200);

      if (botonPresionado(PIN_BTN_OK)) {
        mensajeAlarma = "";
        estadoActual = REPOSO;
      }
      break;
    }
  }
}
