#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "driver/twai.h"
#include <Preferences.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

#define TEST_MODE false   // ← true para datos simulados sin bus CAN

// -----------------------------
// DEPURACIÓN
// Serial.print() es BLOQUEANTE: con el sondeo rápido salen ~50 líneas por
// segundo y eso roba tiempo real al bus. Con DEBUG a 0 las macros se
// compilan a NADA.
// -----------------------------
#define DEBUG 0

#if DEBUG
  #define DBG(x)        Serial.print(x)
  #define DBGLN(x)      Serial.println(x)
  #define DBGF(...)     Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGLN(x)
  #define DBGF(...)
#endif

// -----------------------------
// CONFIGURACIÓN CAN (TWAI)
//
// CAN_TX DEBE estar en GPIO0-5: son los pines del dominio RTC del
// ESP32-C3, los únicos donde gpio_hold_en() garantiza que el nivel se
// mantiene durante el deep sleep. Es crítico: si TX quedara flotando y
// cayera a bajo, el transceptor mantendría el bus CAN en dominante
// permanente y bloquearía la comunicación de todo el coche.
//
// CAN_RX puede ir en cualquier pin: solo necesitaba estar en 0-5 cuando
// despertábamos por flanco, y ahora despertamos por temporizador.
//
// RECOMENDADO ADEMÁS: resistencia de 10k entre CAN_TX y 3V3. El
// gpio_hold_en protege durante el sueño, pero NO durante resets, el
// arranque ni si el firmware se cuelga.
// -----------------------------
#define CAN_TX     GPIO_NUM_5
#define CAN_RX     GPIO_NUM_4

// -----------------------------
// RITMO DE SONDEO
// La ECU contesta en 10-30 ms. Se lanza la siguiente petición en cuanto
// la anterior se resuelve, con solo un respiro mínimo.
//
// REQ_MIN_GAP_MS: si tu ECU empieza a ignorar peticiones, súbelo a 20-30.
// -----------------------------
#define REQ_MIN_GAP_MS      8
#define RESP_TIMEOUT_MS   120
#define MAX_FALLOS          3    // timeouts seguidos -> PID descartado
#define ECU_SILENT_MS    5000
#define TEST_INTERVAL_MS  100

// -----------------------------
// ESTADO DEL ENLACE (PID 0xFF)
// El PID 0xFF no existe en OBD2 real (modo 01 va de 0x00 a 0x7F), así que
// no puede colisionar con un dato y permite reutilizar la misma struct.
// -----------------------------
#define PID_ESTADO       0xFF

#define CAN_ST_TEST       0
#define CAN_ST_INIT       1
#define CAN_ST_NO_ECU     2
#define CAN_ST_OK         3
#define CAN_ST_BUS_OFF    4
#define CAN_ST_ERROR      5
#define CAN_ST_SHUTDOWN   6   // nos vamos a dormir: coche apagado

twai_message_t tx_msg;
unsigned long ultimoEnvio = 0;
unsigned long ultimaRespuesta = 0;

bool canOK = false;
bool esperandoResp = false;
unsigned long tPeticion = 0;

Preferences prefs;

// -----------------------------
// PIDs
// OJO: 0x06 es SHORT term fuel trim y 0x07 es LONG term (SAE J1979).
// Estaban etiquetados al revés.
// -----------------------------
struct PIDInfo { uint8_t pid; const char* nombre; };

PIDInfo listaPIDs[] = {
    {0x0C, "RPM"},      {0x05, "TEMP"},   {0x0D, "VEL"},
    {0x04, "CARGA"},    {0x0F, "INTAKE"}, {0x11, "TPS"},
    {0x0B, "MAP"},      {0x10, "MAF"},    {0x2F, "FUEL LVL"},
    {0x46, "AMBIENT"},  {0x0E, "TIMING"}, {0x06, "STFT1"},
    {0x07, "LTFT1"}
};

#define NUM_PIDS (sizeof(listaPIDs) / sizeof(listaPIDs[0]))

// Lista auto-depurada: un PID que no contesta MAX_FALLOS veces seguidas
// deja de pedirse. Cada PID muerto costaba un timeout entero por vuelta,
// y eso era el origen de los parones periódicos.
uint8_t pidActual   = 0;
uint8_t pidPeticion = 0;
bool    pidActivo[NUM_PIDS];
uint8_t pidFallos[NUM_PIDS];

struct OBDPacket { uint8_t pid; int value; };

uint8_t receptorMAC[] = {0xA4, 0xCF, 0x12, 0xF0, 0xEF, 0xD8};

// -----------------------------
// ESP-NOW
// -----------------------------
#ifdef ESP8266
void OnDataSent(uint8_t *mac_addr, uint8_t status)
#else
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status)
#endif
{
    if (status != ESP_NOW_SEND_SUCCESS) DBGLN("ESP-NOW: fallo de envio");
}

void enviarESPNow(uint8_t pid, int valor) {
    OBDPacket pkt;
    pkt.pid = pid;
    pkt.value = valor;
    esp_now_send(receptorMAC, (uint8_t*)&pkt, sizeof(pkt));
}

// Periódico y no solo en los cambios: ESP-NOW no garantiza entrega, y si
// el único paquete de "ya estoy OK" se pierde, el receptor se quedaría
// con un error fantasma. Los cambios sí van al instante.
uint8_t estadoCAN = CAN_ST_INIT;
unsigned long ultimoEstado = 0;

#define ESTADO_INTERVAL_MS 1000

void enviarEstado(uint8_t nuevo, bool forzar = false) {
    bool cambio = (nuevo != estadoCAN);
    estadoCAN = nuevo;
    if (cambio || forzar || millis() - ultimoEstado >= ESTADO_INTERVAL_MS) {
        ultimoEstado = millis();
        enviarESPNow(PID_ESTADO, estadoCAN);
    }
}

// -----------------------------
// SIGUIENTE PETICIÓN
// Un turno de cada dos va a RPM: es el dato que más rápido cambia y el
// que peor tolera el retraso. Con ~20 ms de respuesta, RPM se refresca
// cada ~40 ms y la vuelta completa son ~300 ms.
// -----------------------------
void siguientePeticion() {
    static bool turnoRPM = false;
    turnoRPM = !turnoRPM;

    // Salvaguarda: si TODOS quedaran descartados (ECU muda), reactivarlos
    // para no entrar en bucle infinito buscando uno activo.
    bool hayAlguno = false;
    for (uint8_t i = 0; i < NUM_PIDS; i++) if (pidActivo[i]) { hayAlguno = true; break; }
    if (!hayAlguno)
        for (uint8_t i = 0; i < NUM_PIDS; i++) { pidActivo[i] = true; pidFallos[i] = 0; }

    if (turnoRPM && pidActivo[0]) {
        pidPeticion = 0;
    } else {
        for (uint8_t i = 0; i < NUM_PIDS; i++) {
            pidActual++;
            if (pidActual >= NUM_PIDS) pidActual = 0;
            if (pidActivo[pidActual]) break;
        }
        pidPeticion = pidActual;
    }
    tx_msg.data[2] = listaPIDs[pidPeticion].pid;
}

// -----------------------------
// INICIALIZAR CAN
// -----------------------------
bool iniciarCAN() {
    DBGLN("Inicializando bus CAN...");

    memset(&tx_msg, 0, sizeof(tx_msg));
    tx_msg.identifier = 0x7DF;
    tx_msg.extd = 0;
    tx_msg.rtr = 0;
    tx_msg.data_length_code = 8;
    tx_msg.data[0] = 0x02;
    tx_msg.data[1] = 0x01;
    tx_msg.data[2] = listaPIDs[pidPeticion].pid;

    twai_general_config_t g_config =
        TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);

    // El bus de un coche en marcha lleva cientos de tramas por segundo.
    // Con la cola por defecto (5) se desbordaba y se perdían respuestas.
    g_config.rx_queue_len = 32;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();

    // Filtro: SOLO respuestas de diagnóstico (0x7E8-0x7EF). El tráfico
    // normal del coche ni siquiera entra en la cola.
    // Formato SJA1000: los 11 bits del ID van en los bits 31..21.
    // 0x7E8 << 21 = 0xFD000000. En la máscara, un 1 = "no importa".
    twai_filter_config_t f_config;
    f_config.acceptance_code = 0xFD000000;
    f_config.acceptance_mask = 0x00FFFFFF;
    f_config.single_filter   = true;

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        DBGLN("ERROR: no se pudo instalar TWAI");
        return false;
    }
    if (twai_start() != ESP_OK) {
        DBGLN("ERROR: no se pudo arrancar TWAI");
        twai_driver_uninstall();
        return false;
    }

    DBGLN("CAN listo.");
    esperandoResp = false;
    return true;
}

void reiniciarCAN() {
    DBGLN("Reiniciando driver CAN...");
    twai_stop();
    twai_driver_uninstall();
    delay(50);
    // Si el problema era del bus y no de la ECU, los descartes anteriores
    // eran falsos: se reactivan todos.
    for (uint8_t i = 0; i < NUM_PIDS; i++) { pidActivo[i] = true; pidFallos[i] = 0; }
    canOK = iniciarCAN();
}

// -----------------------------
// VIGILAR SALUD DEL BUS
// El TWAI entra en BUS_OFF tras acumular errores y de ahí NO sale solo:
// hay que llamar a twai_initiate_recovery() explícitamente.
// -----------------------------
void vigilarBusCAN() {
    twai_status_info_t status;
    if (twai_get_status_info(&status) != ESP_OK) {
        enviarEstado(CAN_ST_ERROR);
        return;
    }

    if (status.state == TWAI_STATE_BUS_OFF) {
        DBGLN("BUS_OFF -> recuperando");
        enviarEstado(CAN_ST_BUS_OFF);
        twai_initiate_recovery();
        ultimaRespuesta = millis();
        esperandoResp = false;
        return;
    }
    if (status.state == TWAI_STATE_STOPPED) {
        enviarEstado(CAN_ST_BUS_OFF);
        twai_start();
        esperandoResp = false;
        return;
    }
    if (status.state == TWAI_STATE_RECOVERING) {
        enviarEstado(CAN_ST_BUS_OFF);
        return;
    }

    // RUNNING. El bus puede estar perfecto y no haber nadie al otro lado.
    if (millis() - ultimaRespuesta > ECU_SILENT_MS) {
        enviarEstado(CAN_ST_NO_ECU);
        if (millis() - ultimaRespuesta > ECU_SILENT_MS * 6) reiniciarCAN();
    } else {
        enviarEstado(CAN_ST_OK);
    }
}

// -----------------------------
// DECODIFICAR UN PID
// -----------------------------
int decodificarPID(uint8_t pid, const uint8_t *d) {
    switch (pid) {
        case 0x0C: return ((d[3] << 8) | d[4]) / 4;      // RPM
        case 0x05:                                        // Temp refrigerante
        case 0x0F:                                        // Temp admision
        case 0x46: return d[3] - 40;                      // Temp ambiente
        case 0x0D: return d[3];                           // Velocidad km/h
        case 0x0B: return d[3];                           // MAP kPa
        case 0x04:                                        // Carga motor %
        case 0x11:                                        // TPS %
        case 0x2F: return (d[3] * 100) / 255;             // Nivel comb. %
        case 0x10: return ((d[3] << 8) | d[4]);           // MAF en cg/s
        case 0x0E: return (d[3] / 2) - 64;                // Avance grados
        case 0x06:                                        // STFT %
        case 0x07: return ((d[3] - 128) * 100) / 128;     // LTFT %
    }
    return 0;
}

// -----------------------------
// GESTIÓN DE ENERGÍA
//
// El pin 16 del OBD2 es BAT permanente: hay 12 V aunque el coche esté
// apagado. Sin deep sleep serían ~40 mA constantes que, sumados a los
// ~25 mA propios del coche, dejarían la batería sin arranque en ~10 días.
//
// Se detecta "coche encendido" por ACTIVIDAD DEL BUS, no por tensión: con
// contacto puesto y motor parado la batería marca ~12,2 V y aparcada
// ~12,4 V, indistinguibles en la práctica.
//
// Ciclo: dormir 5 s -> escuchar 150 ms -> decidir. Retraso máximo al
// girar la llave: 5 s. Ciclo de trabajo ~3%, media ~1,4 mA.
// -----------------------------
#define SLEEP_SECS          5
#define PROBE_MS          150
#define VERIFY_MS        8000
#define IDLE_MS         30000

unsigned long tArranque = 0;
bool verificado = false;

void dormirAhora() {
    gpio_set_direction(CAN_TX, GPIO_MODE_OUTPUT);
    gpio_set_level(CAN_TX, 1);
    gpio_hold_en(CAN_TX);
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SECS * 1000000ULL);
    esp_deep_sleep_start();
}

void irADormir() {
    DBGLN("Sin actividad -> deep sleep");
#if DEBUG
    Serial.flush();
#endif

    // Avisar de APAGADO, no de "sin ECU": son cosas distintas. El receptor
    // necesita saber que es un cierre ordenado para lanzar su secuencia de
    // apagado en vez de quedarse esperando.
    //
    // Se manda tres veces: ESP-NOW no garantiza entrega y este mensaje no
    // se repetirá, porque nos dormimos justo después. Perderlo dejaría al
    // D1 Mini colgado en "ESPERANDO ECU" indefinidamente.
    for (int i = 0; i < 3; i++) {
        enviarESPNow(PID_ESTADO, CAN_ST_SHUTDOWN);
        delay(40);
    }

    twai_stop();
    twai_driver_uninstall();
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);

    dormirAhora();
}

// -----------------------------
// SONDEO TRAS DESPERTAR
// En modo SOLO ESCUCHA: no transmite nada, así que no puede molestar al
// coche. Sin WiFi levantado, para que el pico de consumo sea mínimo.
// -----------------------------
bool hayActividadEnBus() {
    twai_general_config_t g =
        TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_LISTEN_ONLY);
    g.rx_queue_len = 8;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    if (twai_start() != ESP_OK) { twai_driver_uninstall(); return false; }

    bool activo = false;
    unsigned long t0 = millis();
    while (millis() - t0 < PROBE_MS) {
        twai_message_t rx;
        if (twai_receive(&rx, pdMS_TO_TICKS(20)) == ESP_OK) { activo = true; break; }
    }

    twai_stop();
    twai_driver_uninstall();
    return activo;
}

// -----------------------------
// SETUP
// -----------------------------
void setup() {
    Serial.begin(115200);
    DBGLN("Arranque");

    // Liberar los pines retenidos durante el sueño
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(CAN_TX);

    // Si venimos de deep sleep, comprobar si merece la pena encenderlo
    // todo ANTES de levantar la radio: WiFi + ESP-NOW cuestan ~100 mA, y
    // hacerlo cada 5 s con el coche aparcado arruinaría el ahorro.
    if (!TEST_MODE &&
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {

        if (!hayActividadEnBus()) {
            DBGLN("Bus mudo -> volver a dormir");
#if DEBUG
            Serial.flush();
#endif
            // No se avisa: ESP-NOW ni está levantado, y el receptor ya
            // recibió el SHUTDOWN cuando nos dormimos la primera vez.
            dormirAhora();
        }
        DBGLN("Bus activo -> arrancando sesion");
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) { DBGLN("Error ESP-NOW"); return; }
    esp_now_register_send_cb(OnDataSent);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receptorMAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) { DBGLN("Error peer"); return; }

    for (uint8_t i = 0; i < NUM_PIDS; i++) { pidActivo[i] = true; pidFallos[i] = 0; }
    pidActual = 0;
    pidPeticion = 0;

    if (!TEST_MODE) {
        enviarEstado(CAN_ST_INIT, true);
        canOK = iniciarCAN();
        if (!canOK) enviarEstado(CAN_ST_ERROR, true);
    } else {
        enviarEstado(CAN_ST_TEST, true);
    }

    tArranque = millis();
    // A media distancia del umbral: deja margen para dormir si la ECU no
    // contesta, pero SIN cruzar ECU_SILENT_MS al arrancar, que hacía
    // aparecer "SIN CONTACTO" en cada encendido aunque todo fuera bien.
    ultimaRespuesta = millis() - 2000;
    verificado = false;
}

// -----------------------------
// LOOP
// -----------------------------
void loop() {

    // ---- MODO TEST ----
    if (TEST_MODE) {
        static unsigned long ultimoTest = 0;
        static bool turnoRPM = true;
        static float fase = 0;
        static int tempFake = 20;
        static int dirTemp = 1;

        if (millis() - ultimoTest < TEST_INTERVAL_MS) return;
        ultimoTest = millis();
        enviarEstado(CAN_ST_TEST);

        if (turnoRPM) {
            fase += 0.06f;
            if (fase > TWO_PI) fase -= TWO_PI;
            // Ralentí ~850 rpm + acelerones que pasan de 6000, para ver
            // entrar el modo corte del receptor
            float env = (sin(fase) + 1.0f) / 2.0f;
            env = env * env;
            int rpmFake = 850 + (int)(env * 5600) + random(-25, 26);
            enviarESPNow(0x0C, constrain(rpmFake, 0, 7000));
        } else {
            static uint8_t divisor = 0;
            if (++divisor >= 10) {         // sube despacio, como un
                divisor = 0;               // calentamiento real
                tempFake += dirTemp;
                if (tempFake >= 105) dirTemp = -1;
                if (tempFake <= 20)  dirTemp = 1;
            }
            // En crudo OBD (valor + 40): el receptor aplica el -40
            enviarESPNow(0x05, tempFake);   // °C directos, como decodificarPID
        }

        turnoRPM = !turnoRPM;
        return;
    }

    // ---- MODO REAL ----
    if (!canOK) {
        static unsigned long ultimoIntento = 0;
        enviarEstado(CAN_ST_ERROR);
        if (millis() - ultimoIntento > 3000) {
            ultimoIntento = millis();
            canOK = iniciarCAN();
        }
        return;
    }

    vigilarBusCAN();

    // ---- VIGILANCIA DE ACTIVIDAD (deep sleep) ----
    // Tras despertar, margen corto: si el bus se activó por un ciclo de
    // diagnóstico interno pero nadie responde a OBD, volvemos a dormir.
    // Ya verificado, margen largo: un hueco de segundos no debe apagar
    // el sistema en marcha.
    if (!verificado) {
        if (millis() - ultimaRespuesta < 2000) {
            verificado = true;
            DBGLN("ECU verificada, sesion activa");
        } else if (millis() - tArranque > VERIFY_MS) {
            irADormir();
        }
    } else if (millis() - ultimaRespuesta > IDLE_MS) {
        irADormir();
    }

    // ---- Petición ----
    if (!esperandoResp && millis() - ultimoEnvio >= REQ_MIN_GAP_MS) {
        ultimoEnvio = millis();
        esp_err_t res = twai_transmit(&tx_msg, pdMS_TO_TICKS(5));
        if (res == ESP_OK) {
            esperandoResp = true;
            tPeticion = millis();
        } else {
            DBGF("twai_transmit fallo: %d\n", res);
        }
    }

    // Timeout: la ECU no soporta ese PID, o se perdió la respuesta.
    if (esperandoResp && millis() - tPeticion > RESP_TIMEOUT_MS) {
        esperandoResp = false;
        if (++pidFallos[pidPeticion] >= MAX_FALLOS) {
            pidActivo[pidPeticion] = false;
            DBGF("PID 0x%02X no soportado, descartado\n", listaPIDs[pidPeticion].pid);
        }
        siguientePeticion();
    }

    // ---- Recepción ----
    twai_message_t rx_msg;
    while (twai_receive(&rx_msg, 0) == ESP_OK) {
        if (rx_msg.data_length_code >= 4 &&
            rx_msg.data[1] == 0x41 &&
            rx_msg.data[2] == listaPIDs[pidPeticion].pid)
        {
            int valor = decodificarPID(listaPIDs[pidPeticion].pid, rx_msg.data);
            ultimaRespuesta = millis();
            pidFallos[pidPeticion] = 0;
            enviarESPNow(listaPIDs[pidPeticion].pid, valor);
            esperandoResp = false;
            siguientePeticion();
        }
    }
}