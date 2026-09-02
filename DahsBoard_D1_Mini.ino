#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <EEPROM.h>

// -----------------------------
// TIPOGRAFÍA DEL NÚMERO
// La smooth font solo contiene DÍGITOS, punto y guion. Cualquier texto
// con letras DEBE ir por drawGfxText() o no se dibujará nada.
// -----------------------------
#define USE_SMOOTH_FONT 1
#if USE_SMOOTH_FONT
  #include "ArialBold48.h"
#endif

const int buttonPin = D3;

// -----------------------------
// TFT + SPRITE COMPARTIDO
// -----------------------------
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite iconSprite = TFT_eSprite(&tft);

// Ajustado al icono real. Antes estaba definido DOS veces (100x92 y
// 84x72): el compilador cogía el segundo y avisaba de redefinición.
#define SPRITE_W 84
#define SPRITE_H 72
bool spriteReady = false;

struct OBDPacket { uint8_t pid; int value; };

bool initMeter = true;

const int16_t gaugeX = 120;
const int16_t gaugeY = 120;
const uint8_t gaugeRadius = 120;

// -----------------------------
// PALETA
// -----------------------------
const uint16_t NEON_CYAN   = tft.color565(0, 225, 255);
const uint16_t NEON_ORANGE = tft.color565(255, 140, 0);
const uint16_t NEON_RED    = tft.color565(255, 60, 40);
const uint16_t NEON_YELLOW = tft.color565(255, 210, 90);
const uint16_t NEON_VIOLET = tft.color565(170, 120, 255);
const uint16_t TRACK_GRAY  = tft.color565(40, 40, 46);
const uint16_t ZONE_OK     = 0x4734;              // verde menta
const uint16_t BG_COLOR    = tft.color565(12, 12, 12);
const uint16_t STALE_GRAY  = tft.color565(90, 90, 96);
const uint16_t LABEL_GRAY  = tft.color565(150, 150, 155);

// Track de fondo: marca por dónde SE PUEDE mover el arco. El tramo
// peligroso se tiñe de un rojo muy apagado, a la misma intensidad
// perceptual que el gris, para que se lea como fondo serigrafiado y
// no compita con el arco activo.
const uint16_t TRACK_DANGER = tft.color565(58, 20, 22);
const uint16_t TRACK_WARN   = tft.color565(54, 40, 18);

#define STALE_MS 6000   // con 13 PIDs rotando, cada gauge recibe dato
                        // cada ~1.3 s. 3000 daría falsos "sin datos".

// -----------------------------
// ESTADO DEL ENLACE CAN (PID 0xFF)
// -----------------------------
#define PID_ESTADO       0xFF
#define CAN_ST_TEST       0
#define CAN_ST_INIT       1
#define CAN_ST_NO_ECU     2
#define CAN_ST_OK         3
#define CAN_ST_BUS_OFF    4
#define CAN_ST_ERROR      5
#define CAN_ST_SHUTDOWN   6   // el emisor se va a dormir: coche apagado

volatile int canEstado = -1;
volatile uint32_t lastEstadoPacket = 0;

// -----------------------------
// LAYOUT VERTICAL
//
// La pantalla es CIRCULAR: cerca de los bordes el ancho disponible cae
// rápido, así que cada franja tiene que caber tanto en alto como en la
// cuerda del círculo a esa altura. Ninguna zona invade a otra: el sprite
// del icono ya no pisa la banda de estado, que era lo que la borraba.
// -----------------------------
#define LY_BANNER  (-68)
#define LY_ICON    (-20)
#define LY_NUM      (50)
#define LY_UNIT     (86)

// -----------------------------
// ICONOS
// -----------------------------
#define IC_PISTON   0
#define IC_COOLANT  1
#define IC_SPEED    2
#define IC_LOAD     3
#define IC_INTAKE   4
#define IC_TPS      5
#define IC_MAP      6
#define IC_MAF      7
#define IC_FUEL     8
#define IC_AMBIENT  9
#define IC_TIMING  10
#define IC_TRIM    11

// -----------------------------
// ESQUEMAS DE COLOR
//
// Z_RISE   : más alto = peor  (cian -> verde -> naranja -> rojo)
// Z_FALL   : más bajo = peor  (nivel de combustible)
// Z_CENTER : el centro es lo bueno, los extremos malos (trims, avance)
// Z_COOL   : cuanto más frío mejor (temperaturas de aire)
// Z_FLAT   : informativo, sin juicio (velocidad, MAF, MAP)
// Z_TEMP   : refrigerante, con la banda óptima 82-90
// Z_RPM    : tacómetro
// -----------------------------
#define Z_RISE    0
#define Z_FALL    1
#define Z_CENTER  2
#define Z_COOL    3
#define Z_FLAT    4
#define Z_TEMP    5
#define Z_RPM     6

// -----------------------------
// TABLA DE GAUGES
// Un solo renderizador genérico los dibuja todos. Añadir un gauge nuevo
// es añadir una fila aquí, no copiar 200 líneas.
// -----------------------------
#define NO_B -32768

struct GaugeDef {
    uint8_t     pid;
    const char *name;
    const char *unit;
    int16_t     vmin, vmax;
    int16_t     b1, b2;        // cortes visuales del aro
    uint8_t     icon;
    uint8_t     zone;
    uint8_t     dec;           // 1 -> el valor llega x10 (MAF)
    int16_t     warnFrom;
    int16_t     dangerFrom;
};

const GaugeDef GAUGES[] = {
  // pid   nombre         unidad   min   max     b1     b2   icono       zona      dec  warn  danger
  { 0x0C, "REGIMEN",     "RPM",     0,  7000,  5000,  6000, IC_PISTON,  Z_RPM,    0,  5000,  6000 },
  { 0x05, "REFRIGERANTE","C",       0,   120,    82,    95, IC_COOLANT, Z_TEMP,   0,    90,    95 },
  { 0x0D, "VELOCIDAD",   "KM/H",    0,   180,   NO_B,  NO_B, IC_SPEED,   Z_FLAT,   0,  NO_B,  NO_B },
  { 0x04, "CARGA MOTOR", "%",       0,   100,     70,    90, IC_LOAD,    Z_RISE,   0,    70,    90 },
  { 0x0F, "ADMISION",    "C",     -20,    90,     50,    70, IC_INTAKE,  Z_COOL,   0,    50,    70 },
  { 0x11, "ACELERADOR",  "%",       0,   100,   NO_B,  NO_B, IC_TPS,     Z_FLAT,   0,  NO_B,  NO_B },
  { 0x0B, "PRESION MAP", "KPA",     0,   255,   NO_B,  NO_B, IC_MAP,     Z_FLAT,   0,  NO_B,  NO_B },
  { 0x10, "CAUDAL AIRE", "G/S",     0,  5000,   NO_B,  NO_B, IC_MAF,     Z_FLAT,   1,  NO_B,  NO_B },
  { 0x2F, "COMBUSTIBLE", "%",       0,   100,     15,    30, IC_FUEL,    Z_FALL,   0,  NO_B,  NO_B },
  { 0x46, "AMBIENTE",    "C",     -20,    60,   NO_B,  NO_B, IC_AMBIENT, Z_COOL,   0,  NO_B,  NO_B },
  { 0x0E, "AVANCE",      "GRA",   -30,    60,      0,  NO_B, IC_TIMING,  Z_CENTER, 0,  NO_B,  NO_B },
  { 0x06, "AJUSTE CORTO","%",     -30,    30,    -10,    10, IC_TRIM,    Z_CENTER, 0,  NO_B,  NO_B },
  { 0x07, "AJUSTE LARGO","%",     -30,    30,    -10,    10, IC_TRIM,    Z_CENTER, 0,  NO_B,  NO_B },
};

#define NUM_GAUGES (sizeof(GAUGES) / sizeof(GAUGES[0]))

volatile int      gaugeVal[NUM_GAUGES];
volatile uint32_t gaugeTs[NUM_GAUGES];

int currentGauge = 0;

// -----------------------------
// MÁQUINA DE ESTADOS DE INTERFAZ
// -----------------------------
#define UI_BOOT      0
#define UI_SPLASH    1
#define UI_LIVE      2
#define UI_SHUTDOWN  3   // animación de cierre
#define UI_SLEEP     4   // pantalla negra, esperando al emisor

int  uiState      = UI_BOOT;
bool splashInit   = true;
bool shutdownInit = true;
bool bannerDirty  = true;

#define SPLASH_MS 550

// -----------------------------
// FUENTES
// loadFont() hace malloc, así que la del número se carga una vez en
// setup() y se deja puesta. Todo lo que lleve letras pasa por
// drawGfxText(), que la descarga y la restaura.
// -----------------------------
void useNumberFont() {
#if !USE_SMOOTH_FONT
    tft.setFreeFont(&FreeSansBold24pt7b);
    tft.setTextSize(1);
#endif
}

void restoreNumberFont() {
#if USE_SMOOTH_FONT
    tft.loadFont(ArialBold48);
#endif
}

void drawGfxText(const char *txt, int x, int y, const GFXfont *f,
                 uint16_t col, uint16_t bg, int pad, uint8_t datum) {
#if USE_SMOOTH_FONT
    tft.unloadFont();
#endif
    tft.setFreeFont(f);
    tft.setTextSize(1);
    tft.setTextColor(col, bg);
    tft.setTextDatum(datum);
    tft.setTextPadding(pad);
    tft.drawString(txt, x, y);
    tft.setTextPadding(0);
    tft.setTextDatum(TL_DATUM);
    restoreNumberFont();
}

// -----------------------------
// UTILIDADES DE COLOR
// -----------------------------
uint16_t colorShade(uint16_t color, float factor) {
    uint8_t r = (color >> 11) & 0x1F, g = (color >> 5) & 0x3F, b = color & 0x1F;
    r = constrain((int)(r * factor), 0, 31);
    g = constrain((int)(g * factor), 0, 63);
    b = constrain((int)(b * factor), 0, 31);
    return (r << 11) | (g << 5) | b;
}

uint16_t colorBlend(uint16_t c1, uint16_t c2, float t) {
    t = constrain(t, 0.0f, 1.0f);
    uint8_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
    uint8_t r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
    return (uint16_t)((uint8_t)(r1 + (r2 - r1) * t) << 11) |
           ((uint8_t)(g1 + (g2 - g1) * t) << 5) |
            (uint8_t)(b1 + (b2 - b1) * t);
}

// -----------------------------
// COLOR SEGÚN ESQUEMA DE ZONA
// Fuente ÚNICA de verdad: la usan tanto el aro como el icono.
// -----------------------------
uint16_t zoneColor(int g, int val) {
    const GaugeDef &d = GAUGES[g];
    float span = (float)(d.vmax - d.vmin);
    float p = constrain((val - d.vmin) / span, 0.0f, 1.0f);

    switch (d.zone) {

      case Z_TEMP:
        // <55 frío | 55-82 calentando | 82-90 óptimo | 90-95 caliente
        // >95 PELIGRO: rojo pleno desde el primer grado, no una
        // transición lenta. A 95 ya hay que reaccionar.
        if (val <= 55) return NEON_CYAN;
        if (val <= 82) return colorBlend(NEON_CYAN, ZONE_OK, (val - 55) / 27.0f);
        if (val <= 90) return ZONE_OK;
        if (val <= 95) return colorBlend(ZONE_OK, NEON_RED, (val - 90) / 5.0f);
        return colorBlend(NEON_RED, tft.color565(255, 0, 0),
                          min((val - 95) / 15.0f, 1.0f));

      case Z_RPM:
        if (val <= 1200) return NEON_CYAN;
        if (val <= 3000) return colorBlend(NEON_CYAN, ZONE_OK, (val - 1200) / 1800.0f);
        if (val <= 5000) return colorBlend(ZONE_OK, NEON_ORANGE, (val - 3000) / 2000.0f);
        if (val <= 6000) return colorBlend(NEON_ORANGE, NEON_RED, (val - 5000) / 1000.0f);
        return NEON_RED;

      case Z_RISE:
        if (p < 0.5f) return colorBlend(NEON_CYAN, ZONE_OK, p * 2);
        if (p < 0.8f) return colorBlend(ZONE_OK, NEON_ORANGE, (p - 0.5f) / 0.3f);
        return colorBlend(NEON_ORANGE, NEON_RED, (p - 0.8f) / 0.2f);

      case Z_FALL:
        if (p < 0.15f) return NEON_RED;
        if (p < 0.30f) return colorBlend(NEON_RED, NEON_ORANGE, (p - 0.15f) / 0.15f);
        if (p < 0.50f) return colorBlend(NEON_ORANGE, ZONE_OK, (p - 0.30f) / 0.20f);
        return ZONE_OK;

      case Z_CENTER: {
        float mid = (d.vmin + d.vmax) / 2.0f;
        float dist = fabs(val - mid) / ((d.vmax - d.vmin) / 2.0f);
        if (dist < 0.35f) return ZONE_OK;
        if (dist < 0.70f) return colorBlend(ZONE_OK, NEON_ORANGE, (dist - 0.35f) / 0.35f);
        return colorBlend(NEON_ORANGE, NEON_RED, (dist - 0.70f) / 0.30f);
      }

      case Z_COOL:
        if (p < 0.5f) return colorBlend(NEON_CYAN, ZONE_OK, p * 2);
        return colorBlend(ZONE_OK, NEON_ORANGE, (p - 0.5f) * 2);

      default: // Z_FLAT
        return colorBlend(NEON_CYAN, NEON_VIOLET, p);
    }
}

// -----------------------------
// VALOR -> ÁNGULO
// Lineal salvo el refrigerante, cuya escala se deforma a propósito: el
// motor pasa casi todo el tiempo entre 82 y 90 °C, así que ese tramo se
// lleva 90° de arco para 8 grados.
// -----------------------------
const int TEMP_BP[]     = {  0,  60, 82,  90,  95, 120 };
const int TEMP_BP_ANG[] = { 30,  90, 150, 240, 270, 330 };

int valToAngle(int g, int val) {
    const GaugeDef &d = GAUGES[g];
    int v = constrain(val, d.vmin, d.vmax);

    if (d.zone == Z_TEMP) {
        for (int i = 0; i < 5; i++)
            if (v <= TEMP_BP[i + 1])
                return map(v, TEMP_BP[i], TEMP_BP[i+1], TEMP_BP_ANG[i], TEMP_BP_ANG[i+1]);
        return 330;
    }
    return map(v, d.vmin, d.vmax, 30, 330);
}

// -----------------------------
// ARCO CON CORTES DE ZONA
// El aro se PARTE en las fronteras en vez de dibujar marcas encima:
// un corte se lee como intencionado, una marca superpuesta como parche.
//
// Va ANTES de drawTrackRange, que lo usa.
// -----------------------------
#define ZONE_GAP_DEG 3

void drawArcGapped(int x, int y, int ro, int ri, int from, int to,
                   uint16_t color, uint16_t bg, const int *bounds, int nb) {
    if (to <= from) return;
    int cur = from;
    for (int i = 0; i < nb; i++) {
        int gs = bounds[i] - ZONE_GAP_DEG, ge = bounds[i] + ZONE_GAP_DEG;
        if (ge <= cur) continue;
        if (gs >= to) break;
        if (gs > cur) tft.drawArc(x, y, ro, ri, cur, gs, color, bg);
        cur = max(cur, ge);
    }
    if (to > cur) tft.drawArc(x, y, ro, ri, cur, to, color, bg);
}

// Color del TRACK en un ángulo dado. El track no es uniforme: los tramos
// de atención y peligro llevan su propio tono apagado, como la banda roja
// serigrafiada de un tacómetro real.
uint16_t trackColorAt(int g, int ang) {
    const GaugeDef &d = GAUGES[g];
    if (d.dangerFrom != NO_B && ang >= valToAngle(g, d.dangerFrom)) return TRACK_DANGER;
    if (d.warnFrom   != NO_B && ang >= valToAngle(g, d.warnFrom))   return TRACK_WARN;
    return TRACK_GRAY;
}

// Dibuja track por tramos. Se usa tanto al construir el gauge como al
// BORRAR cuando el valor baja: si al bajar se repintara todo gris, la
// banda roja desaparecería en cuanto el arco la sobrepasara una vez.
void drawTrackRange(int g, int x, int y, int ro, int ri,
                    int from, int to, const int *bounds, int nb) {
    if (to <= from) return;

    const GaugeDef &d = GAUGES[g];
    int cuts[2]; int nc = 0;
    if (d.warnFrom   != NO_B) cuts[nc++] = valToAngle(g, d.warnFrom);
    if (d.dangerFrom != NO_B) cuts[nc++] = valToAngle(g, d.dangerFrom);
    if (nc == 2 && cuts[0] > cuts[1]) { int t = cuts[0]; cuts[0] = cuts[1]; cuts[1] = t; }

    int cur = from;
    for (int i = 0; i < nc; i++) {
        if (cuts[i] <= cur || cuts[i] >= to) continue;
        drawArcGapped(x, y, ro, ri, cur, cuts[i],
                      trackColorAt(g, cur), BG_COLOR, bounds, nb);
        cur = cuts[i];
    }
    drawArcGapped(x, y, ro, ri, cur, to, trackColorAt(g, cur), BG_COLOR, bounds, nb);
}

int gaugeBounds(int g, int *out) {
    int n = 0;
    if (GAUGES[g].b1 != NO_B) out[n++] = valToAngle(g, GAUGES[g].b1);
    if (GAUGES[g].b2 != NO_B) out[n++] = valToAngle(g, GAUGES[g].b2);
    if (n == 2 && out[0] > out[1]) { int t = out[0]; out[0] = out[1]; out[1] = t; }
    return n;
}

// ==================================================================
// ICONOS VECTORIALES
// Todos reciben "TFT_eSPI &gfx" para poder dibujarse igual en pantalla
// o en el sprite. `ph` avanza cada frame para las animaciones.
// ==================================================================

void drawIconPiston(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ang) {
    float cr = s * 0.28f, rod = s * 0.80f;
    int crankCy = cy - (int)(s * 0.18f) + (int)(s * 0.72f);
    uint16_t wall = colorShade(c, 0.5f);

    float si = sin(ang), co = cos(ang);
    float pinX = cx + cr * si, pinY = crankCy - cr * co;
    float inner = rod * rod - (cr * si) * (cr * si);
    if (inner < 0) inner = 0;
    float pY = crankCy - (cr * co + sqrt(inner));

    int pw = (int)(s * 0.58f), ph = max(4, (int)(s * 0.30f));
    int ch = pw / 2 + 3;
    int ct = crankCy - (int)(cr + rod) - ph / 2 - (int)(s * 0.16f);
    gfx.drawFastVLine(cx - ch, ct, crankCy - (int)(cr * 0.2f) - ct, wall);
    gfx.drawFastVLine(cx + ch, ct, crankCy - (int)(cr * 0.2f) - ct, wall);
    gfx.drawFastHLine(cx - ch, ct, ch * 2, wall);

    gfx.drawLine(cx, (int)pY, (int)pinX, (int)pinY, c);
    gfx.drawLine(cx + 1, (int)pY, (int)pinX + 1, (int)pinY, c);
    gfx.fillRoundRect(cx - pw/2, (int)pY - ph/2, pw, ph, 3, c);
    gfx.drawCircle(cx, crankCy, (int)cr, wall);
    gfx.fillCircle((int)pinX, (int)pinY, max(2, (int)(s * 0.10f)), c);
}

void drawCoolantWave(TFT_eSPI &gfx, int cx, int y, int hw, int amp,
                     int th, float sh, uint16_t c) {
    for (int dx = -hw; dx <= hw; dx += 2) {
        float wy = sin((float)dx / (float)hw * PI * 1.7f + sh) * amp;
        gfx.fillCircle(cx + dx, y + (int)wy, th, c);
    }
}

// Termómetro base, compartido por refrigerante / admisión / ambiente
void drawThermoBody(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c) {
    int sw = max(4, (int)(s * 0.26f));
    int st = cy - (int)(s * 0.92f);
    int bc = cy + (int)(s * 0.40f);
    gfx.fillRoundRect(cx - sw/2, st, sw, bc - st, sw/2, c);
    gfx.fillCircle(cx, bc, max(4, (int)(s * 0.32f)), c);
    int gx = cx + (int)(sw * 0.62f), gw = (int)(s * 0.40f);
    int gh = max(3, (int)(s * 0.13f));
    for (int i = 0; i < 3; i++)
        gfx.fillRoundRect(gx, st + (int)(s * (0.24f + i * 0.28f)), gw, gh, gh/2, c);
}

void drawIconCoolant(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph, float spd) {
    int hw = (int)(s * 1.12f), amp = max(2, (int)(s * 0.14f)), th = max(2, (int)(s * 0.085f));
    drawCoolantWave(gfx, cx, cy + (int)(s * 0.52f), hw, amp, th, ph * spd,      c);
    drawCoolantWave(gfx, cx, cy + (int)(s * 0.92f), hw, amp, th, ph * spd + PI, c);
    drawThermoBody(gfx, cx, cy, s, c);
}

void drawIconSpeed(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    uint16_t dim = colorShade(c, 0.45f);
    int r = (int)(s * 0.9f);
    for (int a = 200; a <= 340; a += 4) {
        float rad = radians(a);
        gfx.fillCircle(cx + cos(rad) * r, cy + sin(rad) * r + s*0.25f, 2, dim);
    }
    float sweep = (sin(ph * 1.4f) + 1.0f) / 2.0f;
    float na = radians(200 + sweep * 140);
    gfx.drawLine(cx, cy + s*0.25f, cx + cos(na) * r * 0.8f, cy + sin(na) * r * 0.8f + s*0.25f, c);
    gfx.drawLine(cx+1, cy + s*0.25f, cx + cos(na) * r * 0.8f + 1, cy + sin(na) * r * 0.8f + s*0.25f, c);
    gfx.fillCircle(cx, cy + s*0.25f, max(3, (int)(s * 0.13f)), c);
}

void drawIconLoad(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    uint16_t dim = colorShade(c, 0.4f);
    int w = (int)(s * 1.3f), h = (int)(s * 0.95f);
    gfx.drawRoundRect(cx - w/2, cy - h/2, w, h, 4, dim);
    gfx.drawFastHLine(cx - w/2 + 4, cy - h/2 - 4, w - 8, dim);
    float lv = (sin(ph * 1.6f) + 1.0f) / 2.0f;
    int bars = 1 + (int)(lv * 4);
    int bw = (w - 14) / 5;
    for (int i = 0; i < bars; i++)
        gfx.fillRect(cx - w/2 + 6 + i * bw, cy - h/2 + 8, bw - 3, h - 16, c);
}

void drawIconIntake(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    drawThermoBody(gfx, cx, cy, s, c);
    uint16_t dim = colorShade(c, 0.6f);
    for (int i = 0; i < 3; i++) {
        float off = fmod(ph * 8 + i * 12, 34.0f);
        int ax = cx - (int)(s * 1.15f) + (int)off;
        int ay = cy - (int)(s * 0.45f) + i * (int)(s * 0.42f);
        gfx.drawFastHLine(ax, ay, (int)(s * 0.35f), dim);
        gfx.drawLine(ax + (int)(s*0.35f), ay, ax + (int)(s*0.25f), ay - 3, dim);
        gfx.drawLine(ax + (int)(s*0.35f), ay, ax + (int)(s*0.25f), ay + 3, dim);
    }
}

void drawIconTps(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    uint16_t dim = colorShade(c, 0.45f);
    float press = (sin(ph * 1.8f) + 1.0f) / 2.0f;
    int baseY = cy + (int)(s * 0.75f);
    gfx.drawFastHLine(cx - s, baseY, (int)(s * 1.9f), dim);

    int topX = cx - (int)(s * 0.35f) + (int)(press * s * 0.5f);
    int topY = cy - (int)(s * 0.75f) + (int)(press * s * 0.5f);
    for (int k = -3; k <= 3; k++)
        gfx.drawLine(topX + k, topY, cx - (int)(s * 0.55f) + k, baseY - 3, c);
    gfx.fillCircle(cx - (int)(s * 0.55f), baseY - 3, 3, dim);
}

void drawIconMap(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    uint16_t dim = colorShade(c, 0.45f);
    int r = (int)(s * 0.85f);
    gfx.drawCircle(cx, cy, r, dim);
    gfx.drawCircle(cx, cy, r - 1, dim);
    for (int a = 210; a <= 330; a += 20) {
        float rad = radians(a);
        gfx.drawLine(cx + cos(rad) * (r - 4), cy + sin(rad) * (r - 4),
                     cx + cos(rad) * (r - 9), cy + sin(rad) * (r - 9), dim);
    }
    float sw = (sin(ph * 1.5f) + 1.0f) / 2.0f;
    float na = radians(210 + sw * 120);
    gfx.drawLine(cx, cy, cx + cos(na) * (r - 12), cy + sin(na) * (r - 12), c);
    gfx.drawLine(cx, cy + 1, cx + cos(na) * (r - 12), cy + sin(na) * (r - 12) + 1, c);
    gfx.fillCircle(cx, cy, max(3, (int)(s * 0.12f)), c);
    gfx.fillRect(cx - 3, cy + r, 6, (int)(s * 0.3f), dim);
}

void drawIconMaf(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    uint16_t dim = colorShade(c, 0.5f);
    int w = (int)(s * 1.5f), h = (int)(s * 1.0f);
    gfx.drawRoundRect(cx - w/2, cy - h/2, w, h, 5, dim);
    for (int i = 0; i < 3; i++) {
        int y = cy - (int)(s * 0.3f) + i * (int)(s * 0.3f);
        float off = fmod(ph * 10 + i * 9, (float)(w - 10));
        int xs = cx - w/2 + 5 + (int)off;
        int xe = min(xs + (int)(s * 0.5f), cx + w/2 - 5);
        if (xe > xs) gfx.drawFastHLine(xs, y, xe - xs, c);
        if (xe > xs + 4) {
            gfx.drawLine(xe, y, xe - 4, y - 3, c);
            gfx.drawLine(xe, y, xe - 4, y + 3, c);
        }
    }
}

void drawIconFuel(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    uint16_t dim = colorShade(c, 0.45f);
    int w = (int)(s * 0.85f), h = (int)(s * 1.5f);
    int bx = cx - (int)(s * 0.3f);
    gfx.drawRoundRect(bx - w/2, cy - h/2, w, h, 3, dim);
    gfx.fillRect(bx - w/2 + 3, cy - h/2 + 4, w - 6, (int)(s * 0.42f), c);
    gfx.drawLine(bx + w/2, cy - h/2 + 6, bx + w/2 + (int)(s*0.35f), cy - h/2 + 6, dim);
    gfx.drawLine(bx + w/2 + (int)(s*0.35f), cy - h/2 + 6,
                 bx + w/2 + (int)(s*0.35f), cy + (int)(s * 0.35f), dim);
    gfx.fillRect(bx + w/2 + (int)(s*0.2f), cy - h/2 - 2, (int)(s*0.3f), 5, dim);
}

void drawIconAmbient(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    drawThermoBody(gfx, cx + (int)(s * 0.35f), cy, (int)(s * 0.85f), c);
    uint16_t sun = colorShade(c, 1.2f);
    int sx = cx - (int)(s * 0.75f), sy = cy - (int)(s * 0.35f);
    int sr = max(4, (int)(s * 0.26f));
    gfx.fillCircle(sx, sy, sr, sun);
    float rot = fmod(ph * 4, 45.0f);
    for (int i = 0; i < 8; i++) {
        float a = radians(i * 45 + rot);
        gfx.drawLine(sx + cos(a) * (sr + 3), sy + sin(a) * (sr + 3),
                     sx + cos(a) * (sr + 8), sy + sin(a) * (sr + 8), sun);
    }
}

void drawIconTiming(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    uint16_t dim = colorShade(c, 0.45f);
    int w = (int)(s * 0.42f);
    gfx.fillRect(cx - w/2, cy - (int)(s * 0.95f), w, (int)(s * 0.55f), dim);
    gfx.fillRoundRect(cx - w/2 - 3, cy - (int)(s * 0.4f), w + 6, (int)(s * 0.5f), 2, c);
    gfx.fillRect(cx - w/2, cy + (int)(s * 0.1f), w, (int)(s * 0.45f), dim);
    gfx.drawFastVLine(cx, cy + (int)(s * 0.55f), (int)(s * 0.25f), dim);
    gfx.drawFastHLine(cx - 6, cy + (int)(s * 0.8f), 12, dim);

    if (fmod(ph, 1.2f) < 0.35f) {
        for (int i = -1; i <= 1; i += 2) {
            gfx.drawLine(cx, cy + (int)(s * 0.62f), cx + i * 6, cy + (int)(s * 0.72f), NEON_YELLOW);
            gfx.drawLine(cx + i * 6, cy + (int)(s * 0.72f), cx + i * 3, cy + (int)(s * 0.8f), NEON_YELLOW);
        }
    }
}

void drawIconTrim(TFT_eSPI &gfx, int cx, int cy, int s, uint16_t c, float ph) {
    int dr = (int)(s * 0.42f);
    int dy = cy + (int)(s * 0.15f);
    gfx.fillCircle(cx, dy, dr, c);
    gfx.fillTriangle(cx - dr, dy - 2, cx + dr, dy - 2, cx, dy - (int)(s * 0.95f), c);

    uint16_t sg = colorShade(c, 0.35f);
    bool plus = fmod(ph, 2.4f) < 1.2f;
    int l = (int)(s * 0.22f);
    gfx.fillRect(cx - l, dy, l * 2, 4, sg);
    if (plus) gfx.fillRect(cx - 2, dy + 2 - l, 4, l * 2, sg);
}

void drawGaugeIcon(TFT_eSPI &gfx, int icon, int cx, int cy, int s,
                   uint16_t c, float ph) {
    switch (icon) {
        case IC_PISTON:  drawIconPiston(gfx, cx, cy, s, c, ph); break;
        case IC_COOLANT: drawIconCoolant(gfx, cx, cy, s, c, ph, 1.0f); break;
        case IC_SPEED:   drawIconSpeed(gfx, cx, cy, s, c, ph); break;
        case IC_LOAD:    drawIconLoad(gfx, cx, cy, s, c, ph); break;
        case IC_INTAKE:  drawIconIntake(gfx, cx, cy, s, c, ph); break;
        case IC_TPS:     drawIconTps(gfx, cx, cy, s, c, ph); break;
        case IC_MAP:     drawIconMap(gfx, cx, cy, s, c, ph); break;
        case IC_MAF:     drawIconMaf(gfx, cx, cy, s, c, ph); break;
        case IC_FUEL:    drawIconFuel(gfx, cx, cy, s, c, ph); break;
        case IC_AMBIENT: drawIconAmbient(gfx, cx, cy, s, c, ph); break;
        case IC_TIMING:  drawIconTiming(gfx, cx, cy, s, c, ph); break;
        case IC_TRIM:    drawIconTrim(gfx, cx, cy, s, c, ph); break;
    }
}

void flashPressFeedback(int x, int y, int r) {
    tft.drawArc(x, y, r - 1, r - 5, 0, 360, TFT_WHITE, BG_COLOR);
}

String formatValue(int g, int val) {
    if (GAUGES[g].dec == 1) {
        // El valor llega en centésimas de unidad (MAF en cg/s)
        int e = val / 100, f = abs((val / 10) % 10);
        return String(e) + "." + String(f);
    }
    return String(val);
}

// -----------------------------
// BANDA DE ESTADO
// Solo aparece cuando hay algo que decir: un "todo OK" permanente sería
// ruido visual y el usuario dejaría de mirarlo.
//
// "ESPERANDO ECU" en vez de "SIN CONTACTO": el módulo está vivo y
// escuchando, simplemente la ECU aún no contesta. Decir "sin contacto"
// cuando el coche está en marcha y solo falta la primera respuesta
// resultaba confuso.
// -----------------------------
const char* estadoTexto(int st) {
    switch (st) {
        case CAN_ST_INIT:    return "CONECTANDO";
        case CAN_ST_NO_ECU:  return "ESPERANDO ECU";
        case CAN_ST_BUS_OFF: return "ERROR CAN";
        case CAN_ST_ERROR:   return "FALLO MODULO";
        case CAN_ST_TEST:    return "MODO TEST";
    }
    return NULL;   // OK y SHUTDOWN no usan banda
}

uint16_t estadoColor(int st) {
    switch (st) {
        case CAN_ST_BUS_OFF:
        case CAN_ST_ERROR:   return NEON_RED;
        case CAN_ST_TEST:    return NEON_CYAN;
    }
    return LABEL_GRAY;
}

void drawStatusBanner(int x, int y, int r) {
    static int lastShown = -2;
    bool sinEmisor = (lastEstadoPacket == 0) || (millis() - lastEstadoPacket > 4000);
    int st = sinEmisor ? -1 : canEstado;

    if (st == lastShown && !bannerDirty) return;
    lastShown = st;
    bannerDirty = false;

    int by = y + LY_BANNER;
    tft.fillRect(x - 60, by - 10, 120, 20, BG_COLOR);

    const char *txt; uint16_t col;
    if (sinEmisor) { txt = "SIN SENAL"; col = NEON_ORANGE; }
    else {
        txt = estadoTexto(st);
        col = estadoColor(st);
        if (txt == NULL) return;
    }
    drawGfxText(txt, x, by, &FreeSans9pt7b, col, BG_COLOR, 120, MC_DATUM);
}

// ==================================================================
// ANIMACIÓN DE ARRANQUE
// Barrido completo del aro. Además de dar entrada, es un autotest
// visual: si un píxel o el SPI fallan, se ve al instante.
// ==================================================================
bool runBootAnimation(int x, int y, int r) {
    static bool init = true;
    static uint32_t t0 = 0;
    static int lastA = 30;
    const uint32_t UP = 750, HOLD = 200, DOWN = 500;
    int ro = r - 4, ri = ro - 14;

    if (init) {
        init = false; t0 = millis(); lastA = 30;
        tft.fillScreen(BG_COLOR);
        drawGfxText("CAR GAUGES", x, y, &FreeSansBold12pt7b,
                    tft.color565(170,170,178), BG_COLOR, 0, MC_DATUM);
    }

    uint32_t el = millis() - t0;
    float p;
    if (el < UP)                    p = el / (float)UP;
    else if (el < UP + HOLD)        p = 1.0f;
    else if (el < UP + HOLD + DOWN) p = 1.0f - (el - UP - HOLD) / (float)DOWN;
    else { init = true; return true; }

    int ang = 30 + (int)(p * 300);
    uint16_t c = colorBlend(NEON_CYAN, NEON_ORANGE, p);
    if (ang > lastA)      tft.drawArc(x, y, ro, ri, lastA, ang, c, BG_COLOR);
    else if (ang < lastA) tft.drawArc(x, y, ro, ri, ang, lastA, BG_COLOR, BG_COLOR);
    lastA = ang;
    return false;
}

// ==================================================================
// PANTALLA DE TRANSICIÓN
// Sin escalado: el zoom se dibujaba directamente en pantalla (borrar +
// repintar en cada paso) y ese hueco era el parpadeo.
// El "3 / 13" es necesario: con trece pantallas el nombre solo no basta.
// ==================================================================
bool runSplash(int g, int x, int y, int r) {
    static uint32_t t0 = 0;

    if (splashInit) {
        splashInit = false;
        t0 = millis();
        tft.fillScreen(BG_COLOR);

        drawGaugeIcon(tft, GAUGES[g].icon, x, y - 30, 42,
                      zoneColor(g, gaugeVal[g]), 0.6f);

        drawGfxText(GAUGES[g].name, x, y + 48, &FreeSansBold12pt7b,
                    TFT_WHITE, BG_COLOR, 220, MC_DATUM);

        char pos[10];
        snprintf(pos, sizeof(pos), "%d / %d", g + 1, (int)NUM_GAUGES);
        drawGfxText(pos, x, y + 76, &FreeSans9pt7b, LABEL_GRAY, BG_COLOR, 80, MC_DATUM);
    }

    return (millis() - t0 >= SPLASH_MS);
}

// ==================================================================
// SECUENCIA DE APAGADO
//
// El emisor avisa antes de dormirse, así que el receptor puede cerrar de
// forma ordenada en vez de quedarse con datos congelados o mostrando un
// error que no lo es. El arco se retrae hasta desaparecer.
// ==================================================================
bool runShutdown(int x, int y, int r) {
    static uint32_t t0 = 0;
    static int lastA = 330;
    const uint32_t SWEEP = 700, HOLD = 1200;
    int ro = r - 4, ri = ro - 14;

    if (shutdownInit) {
        shutdownInit = false;
        t0 = millis();
        lastA = 330;
        tft.fillScreen(BG_COLOR);
        tft.drawArc(x, y, ro, ri, 30, 330, tft.color565(60, 60, 68), BG_COLOR);
        drawGfxText("MOTOR APAGADO", x, y - 10, &FreeSansBold12pt7b,
                    LABEL_GRAY, BG_COLOR, 220, MC_DATUM);
        drawGfxText("hasta luego", x, y + 22, &FreeSans9pt7b,
                    tft.color565(90, 90, 98), BG_COLOR, 180, MC_DATUM);
    }

    uint32_t el = millis() - t0;

    if (el < SWEEP) {
        int ang = 330 - (int)((el / (float)SWEEP) * 300);
        if (ang < lastA) {
            tft.drawArc(x, y, ro, ri, ang, lastA, BG_COLOR, BG_COLOR);
            lastA = ang;
        }
        return false;
    }
    return (el >= SWEEP + HOLD);
}

// ==================================================================
// RENDERIZADOR GENÉRICO
// Un solo cuerpo para los 13 gauges. Toda la variación viene de la
// tabla GAUGES, así que un arreglo aquí beneficia a todos a la vez.
// ==================================================================
#define REDLINE_ON   6000
#define REDLINE_OFF  5800
#define REDLINE_FLASH_MS 110

void drawGauge(int g, int x, int y, int r) {
    static uint32_t lastUpdate = 0;
    if (millis() - lastUpdate < 20) return;
    lastUpdate = millis();

    static bool  firstInit    = true;
    static bool  labelDirty   = true;
    static int   lastVal      = -32000;
    static int   lastShownInt = -32000;
    static uint16_t lastArcColor = 0;
    static int   lastAngle    = 30;
    static uint32_t lastIconFrame = 0, lastNumFrame = 0;
    static float iconPhase    = 0;
    static bool  wasStale = false, staleShown = false;
    static bool  redline = false;
    static uint32_t lastFlash = 0;
    static bool  flashOn = false;

    const GaugeDef &d = GAUGES[g];

    int iconCx = x, iconCy = y + LY_ICON;
    int iconSize = 27;
    int textY = y + LY_NUM;
    int unitY = y + LY_UNIT;

    int ro = r - 4, ri = ro - 14;
    int bounds[2]; int nb = gaugeBounds(g, bounds);

    if (initMeter) {
        initMeter = false;
        firstInit = true; labelDirty = true;
        lastVal = -32000; lastShownInt = -32000;
        lastArcColor = 0; lastAngle = 30;
        iconPhase = 0;
        wasStale = false; staleShown = false; redline = false;
        tft.fillScreen(BG_COLOR);
    }

    bool stale = (gaugeTs[g] == 0) || (millis() - gaugeTs[g] > STALE_MS);
    int val = gaugeVal[g];

    // ---- MODO CORTE (solo RPM) ----
    // A esas vueltas no da tiempo a leer un arco: hace falta algo que se
    // vea por el rabillo del ojo. Parpadea contra rojo oscuro, no negro,
    // porque el negro puro da un estroboscópico donde el número se pierde.
    if (d.zone == Z_RPM) {
        bool want = !stale && (redline ? (val > REDLINE_OFF) : (val > REDLINE_ON));
        if (want) {
            if (!redline) { redline = true; lastFlash = 0; flashOn = false; }
            if (millis() - lastFlash > REDLINE_FLASH_MS) {
                lastFlash = millis(); flashOn = !flashOn;
                uint16_t bg = flashOn ? NEON_RED : tft.color565(45, 0, 0);
                tft.fillScreen(bg); yield();

                useNumberFont();
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_WHITE, bg);
                tft.setTextPadding(0);
                tft.drawString(String(val), x, y - 12);
                tft.setTextDatum(TL_DATUM);

                drawGfxText("RPM", x, y + 34, &FreeSans9pt7b, TFT_WHITE, bg, 0, MC_DATUM);

                int cw = 26, cy0 = y + 64;
                for (int k = 0; k < 2; k++) {
                    int oy = cy0 + k * 14;
                    for (int t = 0; t < 3; t++) {
                        tft.drawLine(x - cw, oy + cw/2 + t, x, oy + t, TFT_WHITE);
                        tft.drawLine(x, oy + t, x + cw, oy + cw/2 + t, TFT_WHITE);
                    }
                }
            }
            wasStale = stale;
            return;
        }
        if (redline) {
            // Los fillScreen del parpadeo borraron todo: reconstruir
            redline = false; firstInit = true; labelDirty = true;
            bannerDirty = true;
            lastArcColor = 0; lastAngle = 30; lastShownInt = -32000;
            tft.fillScreen(BG_COLOR);
        }
    }

    // ---- Aro ----
    if (firstInit) {
        drawTrackRange(g, x, y, ro, ri, 30, 330, bounds, nb);
        firstInit = false;
        lastArcColor = 0;
    }

    int va = valToAngle(g, val);

    if (va != lastAngle || stale != wasStale || val != lastVal) {
        uint16_t ac = stale ? STALE_GRAY : zoneColor(g, val);

        if (ac != lastArcColor) {
            drawArcGapped(x, y, ro, ri, 30, va, ac, BG_COLOR, bounds, nb);
            // El resto vuelve a su color de TRACK, no a gris plano: si no,
            // la banda roja se borraría en cuanto el arco la sobrepasara.
            if (va < 330) drawTrackRange(g, x, y, ro, ri, va, 330, bounds, nb);
            lastArcColor = ac;
        } else if (va != lastAngle) {
            int from = min(va, lastAngle), to = max(va, lastAngle);
            if (va > lastAngle) drawArcGapped(x, y, ro, ri, from, to, ac, BG_COLOR, bounds, nb);
            else                drawTrackRange(g, x, y, ro, ri, from, to, bounds, nb);
        }
        lastAngle = va; lastVal = val;
        yield();
    }

    // ---- Icono animado ----
    if (millis() - lastIconFrame > 100) {
        lastIconFrame = millis();

        // La velocidad de animación sigue al valor: la animación ES el dato
        float p = constrain((val - d.vmin) / (float)(d.vmax - d.vmin), 0.0f, 1.0f);
        iconPhase += stale ? 0.04f : (0.10f + p * 0.55f);
        if (iconPhase > 1000.0f) iconPhase -= 1000.0f;

        uint16_t ic = stale ? STALE_GRAY : zoneColor(g, val);

        // Pulso de alerta en sobrecalentamiento. Progresivo, no un
        // parpadeo brusco, que cansa y se lee como fallo de pantalla.
        if (!stale && d.zone == Z_TEMP && val > 95) {
            float pulse = (sin(iconPhase * 2.4f) + 1.0f) / 2.0f;
            ic = colorBlend(ic, colorShade(ic, 1.7f), pulse);
        }

        if (spriteReady) {
            iconSprite.fillSprite(BG_COLOR);
            drawGaugeIcon(iconSprite, d.icon, SPRITE_W/2, SPRITE_H/2, iconSize, ic, iconPhase);
            iconSprite.pushSprite(iconCx - SPRITE_W/2, iconCy - SPRITE_H/2);
        } else {
            tft.fillRect(iconCx - SPRITE_W/2, iconCy - SPRITE_H/2, SPRITE_W, SPRITE_H, BG_COLOR);
            drawGaugeIcon(tft, d.icon, iconCx, iconCy, iconSize, ic, iconPhase);
        }
    }

    // ---- Etiqueta de unidad: estática, una sola vez ----
    if (labelDirty) {
        labelDirty = false;
        uint16_t uc = stale ? STALE_GRAY : LABEL_GRAY;
        tft.fillRect(x - 40, unitY - 13, 80, 26, BG_COLOR);

        // Las fuentes GFX solo cubren ASCII 0x20-0x7E: el símbolo de grado
        // NO existe, así que se dibuja a mano como un anillo.
        if (strcmp(d.unit, "C") == 0) {
            tft.drawCircle(x - 9, unitY - 5, 3, uc);
            tft.drawCircle(x - 9, unitY - 5, 2, uc);
            drawGfxText("C", x - 2, unitY, &FreeSans9pt7b, uc, BG_COLOR, 0, ML_DATUM);
        } else {
            drawGfxText(d.unit, x, unitY, &FreeSans9pt7b, uc, BG_COLOR, 76, MC_DATUM);
        }
    }

    // ---- Número ----
    bool dirty = (val != lastShownInt) || (stale != staleShown);

    if (dirty && millis() - lastNumFrame > 66) {
        lastNumFrame = millis();
        useNumberFont();
        tft.setTextDatum(MC_DATUM);
        // El antialiasing necesita el color de fondo: sin él los bordes
        // suavizados se mezclan con basura y se ve peor que el bitmap.
        tft.setTextColor(stale ? STALE_GRAY : TFT_WHITE, BG_COLOR);
        tft.setTextPadding(120);
        tft.drawString(stale ? String("--") : formatValue(g, val), x, textY);
        tft.setTextPadding(0);
        tft.setTextDatum(TL_DATUM);

        if (stale != staleShown) labelDirty = true;
        lastShownInt = val; staleShown = stale;
    }

    wasStale = stale;
}

// -----------------------------
// CALLBACK ESP-NOW
// -----------------------------
void onDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
    if (len != sizeof(OBDPacket)) return;
    OBDPacket pkt;
    memcpy(&pkt, data, sizeof(pkt));

    if (pkt.pid == PID_ESTADO) {
        canEstado = pkt.value;
        lastEstadoPacket = millis();
        return;
    }

    // El emisor manda la temperatura en crudo OBD (valor + 40)
    for (uint8_t i = 0; i < NUM_GAUGES; i++) {
        if (GAUGES[i].pid == pkt.pid) {
            gaugeVal[i] = pkt.value;   // el emisor ya envía unidades finales
            gaugeTs[i]  = millis();
        }
    }
}

// -----------------------------
// SETUP
// -----------------------------
void setup() {
    Serial.begin(115200);
    EEPROM.begin(4);
    pinMode(buttonPin, INPUT_PULLUP);

    currentGauge = EEPROM.read(0);
    if (currentGauge >= (int)NUM_GAUGES) currentGauge = 0;

    for (uint8_t i = 0; i < NUM_GAUGES; i++) {
        gaugeVal[i] = GAUGES[i].vmin;
        gaugeTs[i]  = 0;
    }

    tft.init();
    tft.setRotation(0);
    tft.fillScreen(BG_COLOR);

    // Sprite primero: necesita ~6 KB CONTIGUOS. Si la fuente se cargara
    // antes podría fragmentar el heap y dejarlo sin sitio.
    iconSprite.setColorDepth(8);
    spriteReady = (iconSprite.createSprite(SPRITE_W, SPRITE_H) != nullptr);
    restoreNumberFont();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_now_init() != 0) return;
    esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
    esp_now_register_recv_cb(onDataRecv);

    uiState = UI_BOOT;
}

// -----------------------------
// NAVEGACIÓN
// -----------------------------
uint32_t saveDue = 0;

void gotoGauge(int g) {
    while (g < 0) g += NUM_GAUGES;
    currentGauge = g % NUM_GAUGES;

    // Guardado DIFERIDO: recorrer trece pantallas seguidas serían trece
    // escrituras de flash. Solo se guarda al quedarse quieto en una.
    saveDue = millis() + 3000;

    uiState = UI_SPLASH;
    splashInit = true;
}

// -----------------------------
// LOOP
// -----------------------------
void loop() {
    // Botón: "siguiente" al SOLTAR, "anterior" al mantener. Así una
    // pulsación no puede disparar las dos cosas.
    static bool lastBtn = HIGH;
    static uint32_t pressT = 0;
    static bool longFired = false;

    bool btn = digitalRead(buttonPin);

    if (lastBtn == HIGH && btn == LOW) {
        pressT = millis();
        longFired = false;
        if (uiState == UI_LIVE) flashPressFeedback(gaugeX, gaugeY, gaugeRadius);
    }

    if (btn == LOW && !longFired && pressT && millis() - pressT > 650) {
        longFired = true;
        gotoGauge(currentGauge - 1);
    }

    if (lastBtn == LOW && btn == HIGH) {
        if (!longFired && millis() - pressT > 30) {
            if (uiState == UI_BOOT)  { uiState = UI_SPLASH; splashInit = true; }
            else if (uiState == UI_SLEEP) { /* dormido: el botón no hace nada */ }
            else                     gotoGauge(currentGauge + 1);
        }
        pressT = 0;
    }
    lastBtn = btn;

    if (saveDue && millis() > saveDue) {
        saveDue = 0;
        EEPROM.write(0, currentGauge);
        EEPROM.commit();
    }

    // -----------------------------
    // TRANSICIONES DE APAGADO / DESPERTAR
    // El emisor avisa explícitamente antes de dormirse, así que el cierre
    // es ordenado en vez de degenerar en "sin señal".
    // -----------------------------
    if (canEstado == CAN_ST_SHUTDOWN &&
        uiState != UI_SHUTDOWN && uiState != UI_SLEEP) {
        uiState = UI_SHUTDOWN;
        shutdownInit = true;
    }

    if (uiState == UI_SLEEP && canEstado >= 0 && canEstado != CAN_ST_SHUTDOWN) {
        uiState = UI_SPLASH;
        splashInit = true;
    }

    switch (uiState) {
        case UI_BOOT:
            if (runBootAnimation(gaugeX, gaugeY, gaugeRadius)) {
                uiState = UI_SPLASH; splashInit = true;
            }
            break;

        case UI_SPLASH:
            if (runSplash(currentGauge, gaugeX, gaugeY, gaugeRadius)) {
                uiState = UI_LIVE;
                initMeter = true;
                bannerDirty = true;
            }
            break;

        case UI_LIVE:
            drawGauge(currentGauge, gaugeX, gaugeY, gaugeRadius);
            drawStatusBanner(gaugeX, gaugeY, gaugeRadius);
            break;

        case UI_SHUTDOWN:
            if (runShutdown(gaugeX, gaugeY, gaugeRadius)) {
                tft.fillScreen(TFT_BLACK);
                uiState = UI_SLEEP;
            }
            break;

        case UI_SLEEP:
            // Pantalla negra. Se sigue escuchando: en cuanto el emisor
            // despierte y mande un estado, se reanuda solo.
            break;
    }

    yield();
}