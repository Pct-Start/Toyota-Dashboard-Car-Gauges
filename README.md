<div align="center">

# 🚗 Car Gauges

**Cuadro de instrumentos auxiliar OBD-II con pantalla circular y enlace inalámbrico**

[![Platform](https://img.shields.io/badge/platform-ESP32--C3%20%7C%20ESP8266-blue?style=flat-square)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/framework-Arduino-00979D?style=flat-square&logo=arduino)](https://www.arduino.cc/)
[![Protocol](https://img.shields.io/badge/protocol-ISO%2015765--4%20CAN-orange?style=flat-square)](https://en.wikipedia.org/wiki/OBD-II_PIDs)
[![Link](https://img.shields.io/badge/link-ESP--NOW-9cf?style=flat-square)](https://www.espressif.com/en/solutions/low-power-solutions/esp-now)
[![Display](https://img.shields.io/badge/display-GC9A01%20240×240-purple?style=flat-square)](https://github.com/Bodmer/TFT_eSPI)

Lee parámetros en tiempo real de la ECU por el puerto de diagnóstico y los
muestra en una pantalla redonda montada en el habitáculo.
**13 indicadores** · **Iconos vectoriales animados** · **Sueño profundo**

</div>

---

## 📋 Tabla de contenidos

| | | |
|---|---|---|
| [🏗️ Arquitectura](#️-arquitectura) | [🔌 Pinout](#-pinout) | [📡 Protocolo](#-protocolo-esp-now) |
| [🛠️ Hardware](#️-hardware) | [⚙️ Instalación](#️-instalación) | [📊 Parámetros](#-parámetros-monitorizados) |
| [⚠️ Seguridad eléctrica](#️-seguridad-eléctrica) | [🎨 Interfaz](#-interfaz-de-usuario) | [🔋 Energía](#-gestión-de-energía) |
| [🔧 Configuración](#-configuración) | [🩺 Diagnóstico](#-diagnóstico-de-problemas) | [🗺️ Roadmap](#️-roadmap) |

---

## 🎯 Vehículo objetivo

> Desarrollado y probado sobre un **Toyota Yaris II (XP90, 2009) 1.33 Dual VVT-i**
> — motor **1NR-FE**, protocolo **ISO 15765-4 CAN** a **500 kbit/s** con
> cabeceras de 11 bits.
>
> El firmware es genérico: funciona en cualquier vehículo compatible con
> OBD-II sobre CAN. Los PID no soportados se descartan automáticamente.

---

## 🏗️ Arquitectura

Dos módulos independientes conectados por radio. La separación permite montar la
pantalla donde resulte visible sin tirar cables hasta el conector de diagnóstico.

```
        ╔═══════════════════════════╗                ╔═══════════════════════════╗
        ║      MÓDULO EMISOR        ║                ║     MÓDULO RECEPTOR       ║
        ║                           ║    ESP-NOW     ║                           ║
   CAN  ║   ESP32-C3 SuperMini      ║   2.4 GHz      ║   Wemos D1 Mini           ║
  ══════╬═► + transceptor CAN       ║ ═ ═ ═ ═ ═ ═ ═► ║   + GC9A01 240×240        ║
  12 V  ║   + regulador buck        ║   8 bytes      ║   + pulsador              ║
  ══════╬═►                         ║                ║                           ║
        ║   📍 Conector OBD-II      ║                ║   📍 Salpicadero          ║
        ╚═══════════════════════════╝                ╚═══════════════════════════╝
          DahsBoard_ESP32C3.ino                        DahsBoard_D1_Mini.ino
```

<table>
<tr><th width="50%">📤 Emisor</th><th width="50%">📥 Receptor</th></tr>
<tr valign="top"><td>

**Sondeo adaptativo**
Lanza la petición siguiente en cuanto se resuelve la anterior, sin ciclo fijo.
Un turno de cada dos se reserva al régimen del motor.

**Lista auto-depurada**
Un PID que falla 3 veces seguidas deja de pedirse. Elimina los huecos muertos
sin conocer de antemano qué expone la ECU.

**Supervisión del bus**
Detecta y recupera el estado `BUS_OFF` del controlador TWAI, que no se
resuelve por sí solo.

**Gestión de energía**
Sueño profundo con despertar periódico y sondeo del bus.

</td><td>

**Renderizado genérico**
Un solo motor de dibujo para los 13 indicadores. Toda la variación se declara
en la tabla `GAUGES`.

**Redibujado incremental**
Solo se repinta lo que cambia. El icono se compone en un sprite fuera de
pantalla para evitar parpadeo.

**Navegación persistente**
Pulsador con escritura diferida a EEPROM.

**Estados de sesión**
Arranque, transición, operación, apagado y reposo.

</td></tr>
</table>

---

## 🛠️ Hardware

### Lista de materiales

| Componente | Modelo de referencia | Notas |
|:---|:---|:---|
| 🧠 MCU emisor | ESP32-C3 SuperMini | Controlador TWAI integrado |
| 🔗 Transceptor CAN | SN65HVD230 / TJA1051T/3 | 3,3 V · el HVD230 permite standby |
| ⚡ Regulador | MP1584 o equivalente | ⛔ **Evitar LM2596** (consumo en vacío alto) |
| 🧠 MCU receptor | Wemos D1 Mini (ESP8266) | |
| 🖥️ Pantalla | GC9A01 240×240 IPS redonda | SPI |
| 🔘 Pulsador | Táctil momentáneo | A masa, pull-up interno |
| 🛡️ Protección | TVS SMBJ24A + fusible 500 mA | Ver [seguridad eléctrica](#️-seguridad-eléctrica) |

---

## 🔌 Pinout

### 📥 Receptor — Wemos D1 Mini

Configuración de la pantalla en el `User_Setup.h` de **TFT_eSPI**:

```cpp
#define GC9A01_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// Pines Wemos D1 mini
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   0
#define TFT_MISO -1

#define LOAD_GFXFF        // fuentes vectoriales FreeSans
#define SMOOTH_FONT       // fuentes .vlw con suavizado
```

Equivalencia con la serigrafía de la placa:

| Señal | GPIO | Etiqueta D1 Mini | Pin GC9A01 |
|:---|:---:|:---:|:---|
| `TFT_MOSI` | 13 | **D7** | `SDA` / `DIN` |
| `TFT_SCLK` | 14 | **D5** | `SCL` / `CLK` |
| `TFT_CS` | 15 | **D8** | `CS` |
| `TFT_DC` | 2 | **D4** | `DC` |
| `TFT_RST` | 0 | **D3** | `RST` |
| `TFT_MISO` | — | *(sin usar)* | — |
| — | — | `3V3` | `VCC` |
| — | — | `G` | `GND` |
| — | — | `3V3` | `BLK` *(retroiluminado)* |

#### ⚠️ Conflicto de pines a resolver

> El sketch declara `const int buttonPin = D3;` y `User_Setup.h` declara
> `#define TFT_RST 0`. **Ambos son el mismo pin físico** (GPIO0 = D3).
>
> Además GPIO0 es un pin de arranque: si está a nivel bajo durante el reset, el
> ESP8266 entra en modo de programación en lugar de ejecutar el sketch.
>
> **Solución recomendada** — mover el pulsador a un pin libre:
>
> ```cpp
> const int buttonPin = D1;   // GPIO5, libre y sin función de arranque
> ```
>
> **Alternativa** — liberar D3 dejando el reset de la pantalla al del MCU:
> poner `#define TFT_RST -1` y unir el pin `RST` de la pantalla al `RST` del
> D1 Mini.

Pines libres tras el cambio: `D0` (GPIO16), `D2` (GPIO4), `D6` (GPIO12).

---

### 📤 Emisor — ESP32-C3 SuperMini

| Señal | GPIO | Destino | Notas |
|:---|:---:|:---|:---|
| `CAN_TX` | **5** | `TXD` del transceptor | ⚠️ Debe estar en GPIO0–5 |
| `CAN_RX` | **4** | `RXD` del transceptor | Cualquier pin válido |
| `3V3` | — | `VCC` transceptor + salida del buck | |
| `GND` | — | Masa común | |

> **Por qué CAN_TX debe estar en GPIO0–5:** son los únicos pines del dominio
> RTC, los únicos donde `gpio_hold_en()` garantiza que el nivel se mantiene
> durante el sueño profundo. Si el pin quedara flotando y cayera a nivel bajo,
> el transceptor mantendría el bus en estado dominante permanente y bloquearía
> la comunicación de **todas** las centralitas del vehículo.

---

### 🚙 Conector OBD-II (SAE J1962)

```
        ┌───────────────────────────────────┐
         \  1  2  3  4  5  6  7  8         /
          \  9 10 11 12 13 14 15 16       /
           └─────────────────────────────┘
```

| Pin | Señal | Destino |
|:---:|:---|:---|
| **6** | CAN High | `CANH` del transceptor |
| **14** | CAN Low | `CANL` del transceptor |
| **16** | +12 V permanente | Entrada del regulador *(vía fusible y TVS)* |
| **4 / 5** | Masa | `GND` |

---

## ⚠️ Seguridad eléctrica

El emisor se alimenta del pin 16, que tiene tensión de batería **permanente**.
Esto implica tres medidas no negociables.

<table>
<tr><td width="33%">

### 🛡️ Sobretensión

La desconexión de la batería con el alternador cargando (*load dump*) puede
llevar la línea a **40–80 V** durante milisegundos.

**TVS de 24 V** en paralelo a la entrada del regulador.

</td><td width="33%">

### 🔥 Fusible propio

El circuito del vehículo protege el mazo, no tu módulo.

**Fusible de 500 mA** en serie con el positivo.

</td><td width="33%">

### 📌 Pull-up en CAN_TX

`gpio_hold_en()` protege durante el sueño, pero **no** durante resets, arranque
ni bloqueos.

**Resistencia de 10 kΩ** entre CAN_TX y 3,3 V.

</td></tr>
</table>

> 💡 **Consumo en reposo:** los LED de la placa reguladora y del SuperMini
> consumen 2–3 mA cada uno de forma permanente, más que el propio
> microcontrolador dormido. **Se recomienda desoldarlos.**

---

## ⚙️ Instalación

### Dependencias

| Biblioteca | Módulo | Origen |
|:---|:---:|:---|
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | 📥 | Gestor de bibliotecas |
| `ESP8266WiFi` · `espnow` | 📥 | Core ESP8266 |
| `WiFi` · `esp_now` | 📤 | Core ESP32 |
| `driver/twai` | 📤 | Core ESP32 |

### Pasos

```bash
# 1 · Cargar el receptor y anotar su MAC (aparece por serie al arrancar)
#     DahsBoard_D1_Mini.ino  →  Wemos D1 Mini

# 2 · Escribir esa MAC en el sketch del emisor
```

```cpp
uint8_t receptorMAC[] = {0x48, 0x3F, 0xDA, 0x00, 0x6B, 0x84};  // ← tu MAC
```

```bash
# 3 · Cargar el emisor
#     DahsBoard_ESP32C3.ino  →  ESP32-C3 SuperMini
```

> 🧪 Para validar el receptor sin acceso al vehículo, poner `TEST_MODE` a `true`
> en el emisor: genera régimen y temperatura simulados con formas de onda
> parecidas a las reales, incluyendo subidas que disparan la alerta de corte.

<details>
<summary><b>🔤 Generar la tipografía suavizada (opcional)</b></summary>

<br>

El número principal usa una fuente `.vlw` incrustada como `ArialBold48.h`. Se
genera una sola vez con *Create_Smooth_Font*, que acompaña a TFT_eSPI y
requiere [Processing](https://processing.org/):

1. Abrir `TFT_eSPI/Tools/Create_Smooth_Font/Create_font/Create_font.pde`
2. Fijar `fontName` a una fuente instalada y `fontSize = 48`
3. Restringir el rango en `unicodeBlocks` a los caracteres necesarios
4. Ejecutar → genera el `.vlw` y su `.h` en `FontFiles/`
5. Copiar el `.h` junto al sketch

> ⚠️ El juego de caracteres **debe incluir el punto `.`**, además de los dígitos
> `0-9` y el guion `-`. El punto es necesario para el indicador de caudal de
> aire, que se muestra con un decimal.

Restringir el juego no es cosmético: el archivo se carga **completo en RAM**, y
en el ESP8266 la diferencia entre 5 KB y 45 KB determina si el sprite del icono
cabe o no.

Para prescindir de ella, poner `USE_SMOOTH_FONT` a `0`: el sketch recurre a
`FreeSansBold24pt7b`, incluida en la biblioteca.

</details>

---

## 📡 Protocolo ESP-NOW

Ambos extremos comparten una estructura de 8 bytes:

```cpp
struct OBDPacket {
    uint8_t pid;    // identificador del parámetro
    int     value;  // valor decodificado, en unidades finales
};
```

El valor `0xFF` en `pid` está reservado para mensajes de estado, aprovechando
que los PID de modo 01 solo llegan hasta `0x7F` y no puede haber colisión.

### Códigos de estado

| Código | Constante | Significado | En pantalla |
|:---:|:---|:---|:---|
| `0` | `CAN_ST_TEST` | Datos simulados | 🔵 `MODO TEST` |
| `1` | `CAN_ST_INIT` | Inicializando controlador | ⚪ `CONECTANDO` |
| `2` | `CAN_ST_NO_ECU` | Bus operativo, ECU sin responder | ⚪ `ESPERANDO ECU` |
| `3` | `CAN_ST_OK` | Funcionamiento normal | *(sin aviso)* |
| `4` | `CAN_ST_BUS_OFF` | Bus en error, recuperándose | 🔴 `ERROR CAN` |
| `5` | `CAN_ST_ERROR` | Fallo del controlador | 🔴 `FALLO MODULO` |
| `6` | `CAN_ST_SHUTDOWN` | Cierre ordenado antes de dormir | 🎬 Secuencia de apagado |
| — | *(inferido)* | Sin paquetes durante 4 s | 🟠 `SIN SEÑAL` |

> El estado se retransmite **cada segundo aunque no cambie**. ESP-NOW no
> garantiza entrega, y un único paquete perdido dejaría al receptor mostrando
> un error inexistente; la repetición hace el sistema autocorrectivo.
>
> `SIN SEÑAL` no puede transmitirse: es una inferencia del receptor por
> ausencia de mensajes.

---

## 📊 Parámetros monitorizados

| # | Parámetro | PID | Unidad | Rango | Fórmula SAE J1979 |
|:---:|:---|:---:|:---:|:---:|:---|
| 1 | 🔧 Régimen | `0x0C` | rpm | 0 – 7000 | `((A·256)+B)/4` |
| 2 | 🌡️ Refrigerante | `0x05` | °C | 0 – 120 | `A−40` |
| 3 | 🏁 Velocidad | `0x0D` | km/h | 0 – 180 | `A` |
| 4 | 📈 Carga del motor | `0x04` | % | 0 – 100 | `A·100/255` |
| 5 | 🌬️ Temp. admisión | `0x0F` | °C | −20 – 90 | `A−40` |
| 6 | 🦶 Acelerador | `0x11` | % | 0 – 100 | `A·100/255` |
| 7 | 📊 Presión colector | `0x0B` | kPa | 0 – 255 | `A` |
| 8 | 💨 Caudal de aire | `0x10` | g/s | 0 – 50 | `((A·256)+B)/100` |
| 9 | ⛽ Nivel combustible | `0x2F` | % | 0 – 100 | `A·100/255` |
| 10 | ☀️ Temp. ambiente | `0x46` | °C | −20 – 60 | `A−40` |
| 11 | ⚡ Avance encendido | `0x0E` | ° | −30 – 60 | `(A/2)−64` |
| 12 | 💧 Ajuste corto mezcla | `0x06` | % | −30 – 30 | `((A−128)·100)/128` |
| 13 | 💧 Ajuste largo mezcla | `0x07` | % | −30 – 30 | `((A−128)·100)/128` |

> 📌 **`0x06` es el ajuste de CORTO plazo y `0x07` el de LARGO** según SAE J1979.
> Es un error frecuente invertirlos: comparten fórmula, así que los valores
> salen bien pero la etiqueta miente. El corto plazo oscila constantemente; el
> largo es la corrección aprendida.

<details>
<summary><b>❓ ¿Qué PID soporta realmente mi coche?</b></summary>

<br>

El conjunto soportado **varía según motor, mercado, año y versión de software
de la ECU**, y no puede consultarse de forma fiable en documentación de
terceros. La lista autoritativa se obtiene interrogando a la propia centralita
con los PID `0x00`, `0x20` y `0x40`, que devuelven máscaras de bits.

En la práctica no hace falta: **el emisor descarta automáticamente** los que no
responden durante los primeros segundos de cada sesión. Compilando con
`DEBUG 1` se ven en la traza.

Los indicadores correspondientes quedarán mostrando `--` de forma permanente.
En este vehículo, los candidatos habituales son el nivel de combustible
(`0x2F`) y la temperatura ambiente (`0x46`).

</details>

---

## 🎨 Interfaz de usuario

### Navegación

| Acción | Resultado |
|:---|:---|
| 👆 Pulsación breve | Indicador **siguiente** |
| 👇 Pulsación mantenida (>650 ms) | Indicador **anterior** |

La selección se guarda en EEPROM **tres segundos** después del último cambio.
El retardo evita trece escrituras de flash al recorrer todo el carrusel.

### Secuencia de pantallas

```
  ARRANQUE  ──►  TRANSICIÓN  ──►  OPERACIÓN  ──►  APAGADO  ──►  REPOSO
  barrido        icono +          arco +          arco que      pantalla
  del arco       nombre +         valor +         se retrae     en negro
  (autotest)     posición         icono                              │
                     ▲                                               │
                     └───────────── al despertar el emisor ──────────┘
```

<table>
<tr><td width="50%">

**🎬 Arranque**
Barrido completo con degradado cian → naranja. Además de dar entrada, funciona
como **autodiagnóstico visual** del bus SPI y del panel.

**🔄 Transición**
Icono, nombre y posición en la secuencia durante 550 ms. Con trece pantallas la
posición no es decorativa: sin ella se pierde la referencia.

</td><td width="50%">

**📊 Operación**
Arco exterior, icono animado, valor y unidad. La banda superior **solo aparece
cuando hay algo que comunicar**: un «todo correcto» permanente es ruido visual
que se acaba ignorando.

**🌙 Apagado**
Al recibir el aviso del emisor, el arco se retrae y aparece el mensaje de
cierre.

</td></tr>
</table>

### Codificación cromática

El color del arco y el del icono proceden **siempre de la misma función**, de
modo que no pueden contradecirse. Cada parámetro declara uno de siete esquemas
según qué significa «bien» en su contexto:

| Esquema | Criterio | Ejemplo |
|:---|:---|:---|
| `Z_RISE` | Más alto, peor | Carga del motor |
| `Z_FALL` | Más bajo, peor | Nivel de combustible |
| `Z_CENTER` | El centro es lo deseable | Ajustes de mezcla |
| `Z_COOL` | Más frío, mejor | Temperaturas de aire |
| `Z_FLAT` | Informativo, sin juicio | Velocidad, MAP |
| `Z_TEMP` | Refrigerante, banda óptima 82–90 °C | — |
| `Z_RPM` | Tacómetro | — |

El **fondo del arco tampoco es uniforme**: los tramos de atención y peligro
llevan un tono apagado propio, como la banda roja serigrafiada de un tacómetro.
Así se sabe cuánto margen queda aunque la aguja esté lejos.

<details>
<summary><b>📐 Escalas no lineales</b></summary>

<br>

El indicador de refrigerante reparte el arco de forma **deliberadamente
desproporcionada**:

| Tramo | Arco | Resolución |
|:---|:---:|:---|
| 0 – 60 °C | 60° | 1,0 °/°C |
| 60 – 82 °C | 60° | 2,7 °/°C |
| **82 – 90 °C** | **90°** | **11,3 °/°C** ← zona óptima |
| 90 – 95 °C | 30° | 6,0 °/°C |
| 95 – 120 °C | 60° | 2,4 °/°C |

El motor pasa la mayor parte del tiempo en esa franja estrecha, y una escala
proporcional haría indistinguibles 84 y 88 °C. La deformación convierte el
indicador en algo que informa **durante la conducción real** en lugar de solo
en los primeros minutos.

</details>

### 🚨 Alerta de corte

Por encima de **6000 rpm** la pantalla completa parpadea en rojo con el régimen
sobreimpreso y una indicación de cambio de marcha.

- El parpadeo alterna entre **rojo intenso y rojo muy oscuro**, no negro: el
  contraste con negro puro resulta estroboscópico y hace ilegible el número.
- La salida se produce a **5800 rpm**. La histéresis evita que el aviso entre y
  salga a ráfagas cuando el régimen oscila en el umbral, algo más molesto que
  no tener aviso.

---

## 🔋 Gestión de energía

Sin gestión de consumo, el emisor demandaría unos **40 mA continuos**. Sumados
a los 20–30 mA de consumo parásito propio del vehículo, la batería quedaría sin
capacidad de arranque en torno a **los diez días** de inactividad.

### Estrategia de detección

> El firmware detecta si el vehículo está en marcha por **actividad del bus
> CAN**, no por tensión de batería. Con contacto puesto y motor parado la
> batería marca ~12,2 V, y aparcada ~12,4 V: indistinguibles en la práctica. El
> bus, en cambio, **enmudece por completo** al quitar el contacto.

```
   ┌──────────────┐   5 s    ┌──────────────┐  150 ms   ┌─────────────┐
   │ SUEÑO        │ ───────► │ ESCUCHA      │ ────────► │ ¿actividad? │
   │ PROFUNDO     │          │ (solo RX)    │           └──────┬──────┘
   │ ~0,1 mA      │ ◄─────────────── no ───────────────────────┤
   └──────────────┘                                        sí  │
                                                               ▼
                                                     ┌──────────────────┐
                                                     │ SESIÓN COMPLETA  │
                                                     │ WiFi + CAN + TX  │
                                                     └──────────────────┘
```

La escucha se hace **antes** de inicializar WiFi: levantar la radio cuesta unos
100 mA, y hacerlo cada cinco segundos con el vehículo aparcado anularía el
ahorro. Durante una sesión activa, **30 segundos** sin respuestas válidas
provocan el cierre ordenado.

### Consumo estimado

| Fase | Corriente | Duración | Ciclo |
|:---|:---:|:---:|:---:|
| 😴 Sueño profundo | ~0,1 mA + fugas del regulador | 5 s | 97 % |
| 👂 Sondeo del bus | ~40 mA | 0,15 s | 3 % |
| **Media resultante** | **~1,4 mA** | | |

Treinta días de inactividad supondrían alrededor de **1 Ah**, un porcentaje
despreciable de una batería de automóvil.

> ⚠️ Cifras **teóricas**. El consumo real depende sobre todo del regulador y del
> transceptor, no del firmware. **Debe medirse con un amperímetro en serie:** si
> el reposo supera 1–2 mA, la causa está en el hardware (LED, regulador de
> consumo en vacío elevado) y ningún ajuste de software lo compensará.

---

## 🔧 Configuración

<table>
<tr><th colspan="3">📤 Emisor — <code>DahsBoard_ESP32C3.ino</code></th></tr>
<tr><th>Constante</th><th>Valor</th><th>Descripción</th></tr>
<tr><td><code>TEST_MODE</code></td><td><code>false</code></td><td>Datos simulados sin usar el bus</td></tr>
<tr><td><code>DEBUG</code></td><td><code>0</code></td><td>Traza por puerto serie</td></tr>
<tr><td><code>REQ_MIN_GAP_MS</code></td><td><code>8</code></td><td>Separación mínima entre peticiones</td></tr>
<tr><td><code>RESP_TIMEOUT_MS</code></td><td><code>120</code></td><td>Espera máxima de respuesta</td></tr>
<tr><td><code>MAX_FALLOS</code></td><td><code>3</code></td><td>Timeouts antes de descartar un PID</td></tr>
<tr><td><code>SLEEP_SECS</code></td><td><code>5</code></td><td>Periodo de comprobación en reposo</td></tr>
<tr><td><code>PROBE_MS</code></td><td><code>150</code></td><td>Duración de la escucha del bus</td></tr>
<tr><td><code>IDLE_MS</code></td><td><code>30000</code></td><td>Inactividad antes del cierre</td></tr>
</table>

> Con `DEBUG 1` las macros de traza se compilan normalmente; con `0`
> **desaparecen por completo**, sin ocupar espacio ni tiempo. La distinción
> importa: cada línea por serie bloquea 2–4 ms, y al ritmo de sondeo actual eso
> llegaría a dominar el tiempo de proceso.

<table>
<tr><th colspan="3">📥 Receptor — <code>DahsBoard_D1_Mini.ino</code></th></tr>
<tr><th>Constante</th><th>Valor</th><th>Descripción</th></tr>
<tr><td><code>USE_SMOOTH_FONT</code></td><td><code>1</code></td><td>Tipografía suavizada frente a vectorial</td></tr>
<tr><td><code>STALE_MS</code></td><td><code>6000</code></td><td>Antigüedad máxima antes de mostrar <code>--</code></td></tr>
<tr><td><code>SPLASH_MS</code></td><td><code>550</code></td><td>Duración de la pantalla de transición</td></tr>
<tr><td><code>REDLINE_ON</code></td><td><code>6000</code></td><td>Entrada en alerta de corte</td></tr>
<tr><td><code>REDLINE_OFF</code></td><td><code>5800</code></td><td>Salida de la alerta</td></tr>
<tr><td><code>LY_BANNER</code> … <code>LY_UNIT</code></td><td>—</td><td>Posiciones verticales del diseño</td></tr>
</table>

### ➕ Añadir un indicador

Basta con agregar una fila a la tabla `GAUGES`. El motor de renderizado es
común, de modo que **no hay código nuevo que escribir** salvo que se necesite
un icono inédito.

```cpp
//  pid   nombre         unidad   min   max     b1     b2   icono       zona      dec  warn  danger
{ 0x0C, "REGIMEN",     "RPM",     0,  7000,  5000,  6000, IC_PISTON,  Z_RPM,    0,  5000,  6000 },
```

| Campo | Significado |
|:---|:---|
| `b1` / `b2` | Valores donde el aro se **parte** con un hueco visual |
| `warnFrom` / `dangerFrom` | Inicio de las bandas de fondo ámbar y roja |
| `dec` | `1` → el valor llega escalado y se muestra con un decimal |

---

## 🩺 Diagnóstico de problemas

<details>
<summary><b>El indicador muestra <code>--</code> de forma permanente</b></summary>

<br>

El PID no está soportado por la ECU y el emisor lo ha descartado. Compilar con
`DEBUG 1` para ver en la traza cuáles se descartan.

</details>

<details>
<summary><b>No llega ningún dato</b></summary>

<br>

Verificar que la MAC del receptor está correctamente escrita en el emisor.
Ambos módulos deben operar en el mismo canal WiFi.

</details>

<details>
<summary><b>El módulo no despierta al dar contacto</b></summary>

<br>

Medir la salida del regulador con el módulo dormido: debe entregar **3,3 V
estables**. Muchos convertidores económicos se vuelven inestables con carga
casi nula y colapsan la salida, dejando al microcontrolador sin alimentación
para su temporizador.

**Solución:** resistencia de 1 kΩ a masa como carga mínima, o sustituir el
regulador por uno de bajo consumo en vacío.

</details>

<details>
<summary><b>Lecturas con retraso o pausas periódicas</b></summary>

<br>

Revisar que el filtro de aceptación TWAI y el tamaño de la cola de recepción no
se hayan modificado:

```cpp
g_config.rx_queue_len = 32;              // la de por defecto (5) se desborda
f_config.acceptance_code = 0xFD000000;   // solo 0x7E8-0x7EF
f_config.acceptance_mask = 0x00FFFFFF;
```

Con la configuración por defecto, el tráfico normal del vehículo desborda la
cola y se pierden respuestas de diagnóstico.

</details>

<details>
<summary><b>El texto no aparece en pantalla</b></summary>

<br>

La fuente suavizada **solo contiene los caracteres declarados al generarla**.
Cualquier texto con letras debe dibujarse mediante `drawGfxText()`, que conmuta
temporalmente a una fuente vectorial.

</details>

<details>
<summary><b>Nada por el monitor serie (ESP32-C3)</b></summary>

<br>

El puerto USB del SuperMini es el **nativo del microcontrolador**, no un chip
conversor. Requiere habilitar **USB CDC On Boot** en el IDE y **recompilar**.

Además, el puerto desaparece del sistema al entrar en sueño profundo, lo que
deja colgado el monitor.

</details>

<details>
<summary><b>El D1 Mini no arranca o entra en modo programación</b></summary>

<br>

Ver el [conflicto de pines](#️-conflicto-de-pines-a-resolver): el pulsador y el
reset de la pantalla comparten GPIO0, que además es un pin de arranque. Si está
a nivel bajo durante el reset, el ESP8266 entra en modo de programación.

</details>

---

## 🗺️ Roadmap

- [ ] **Enlace bidireccional** — que el receptor comunique qué indicador está
      mostrando para que el emisor priorice ese PID. Reduciría la latencia del
      parámetro visible de ~300 ms a ~40 ms.
- [ ] **Omisión de indicadores no soportados** — que el emisor comunique la
      lista de PID descartados y el receptor los excluya de la navegación, en
      lugar de obligar a pasar por pantallas vacías.
- [ ] **Indicador de tensión de batería** — no requiere el bus CAN, solo un
      divisor resistivo. Detecta un alternador que no carga o una batería en
      mal estado.
- [ ] **Registro de máximos por sesión** — régimen y temperatura máximos.
- [ ] **Control del retroiluminado** — apagar el panel durante el reposo en
      lugar de limitarse a pintarlo en negro.
- [ ] **Modo standby del transceptor** — el SN65HVD230 dispone de un pin de
      control que reduce su consumo de ~15 mA a menos de 0,5 mA manteniendo la
      recepción. Es el mayor consumidor individual durante el reposo.

---

<div align="center">

### 📜 Créditos

Biblioteca de pantalla: **[TFT_eSPI](https://github.com/Bodmer/TFT_eSPI)** de Bodmer
Decodificación conforme a **SAE J1979 / ISO 15031-5**

<sub>⚠️ Este dispositivo se conecta al bus de diagnóstico de un vehículo.<br>
Una instalación incorrecta puede afectar al funcionamiento de sus sistemas electrónicos.</sub>

</div>
