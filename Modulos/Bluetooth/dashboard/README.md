# Dashboard Embebido STM32 · Bluetooth

Aplicación de escritorio en **Python + Tkinter** para comunicarse con una placa **STM32 Nucleo-144** a través de un módulo Bluetooth serie (**HC-05**, **HC-06**). Permite encender y apagar el LED de la placa, ver todo lo que envía el microcontrolador en una terminal y ejecutar pruebas automáticas de comunicación.

- **Autor:** Mamani Flores Carlos (UTN FRT)
- **Versión de la app:** 2.1
- **Archivos:** `bt_dashboard.py` (PC) y `main.c` + driver `bt_STM32.c/.h` (placa)

---

## 1. Qué hace

| Función | Descripción |
|---|---|
| Conexión Bluetooth / Serial | Detecta puertos COM, marca los Bluetooth con `[BT]` y se conecta en un hilo aparte (la ventana nunca se congela). |
| Reconexión automática | Si se pierde el enlace, reintenta cada 2 s. |
| Control del LED | Botones **A (LED ON)** y **B (LED OFF)** y un indicador que muestra el estado real, confirmado por el STM32. |
| Indicador de enlace | Muestra si la placa está enviando datos (`STM32 enlazado`, `Esperando datos…`, `Sin datos hace N s`). |
| Terminal | Colores por tipo de mensaje, hora, autoscroll, pausa, limpiar y guardar a `.txt`. |
| Comando libre | Campo de texto con historial (flechas ↑ ↓) y terminador configurable. |
| Panel de pruebas | Alternar LED, parpadeo, ráfagas, stress, comando inválido, etc. |
| Macros | 6 textos editables que se envían con un clic y se guardan entre sesiones. |
| Configuración persistente | Recuerda puerto, baudios, terminador, macros, etc. |

---

## 2. Requisitos

### Software (PC)

- **Python 3.9 o superior**.
- **tkinter** (viene incluido con Python en Windows y macOS; en Linux: `sudo apt install python3-tk`).
- **pyserial**:

```bash
pip install pyserial
```

### Hardware

- Placa **STM32 Nucleo-144** (el `main.c` de este proyecto está generado para una Nucleo con reloj de 180 MHz, por ejemplo NUCLEO-F439ZI).
- Módulo **HC-05** (recomendado) o **HC-06**.
- Cables Dupont: 4 para la comunicación + 1 para el pin `STATE` (ver sección 3).
- PC con Bluetooth.

> **Sobre el HM-10:** el driver de la placa también lo soporta, pero el HM-10 es **Bluetooth Low Energy (BLE)**. Normalmente Windows **no crea un puerto COM** al emparejarlo, por lo que esta app (que trabaja con puertos serie) no se puede conectar directamente a él. Para la PC usá un HC-05 o HC-06 (Bluetooth clásico); el HM-10 sirve para celulares con apps de terminal BLE.

---

## 3. Conexión del hardware

El firmware usa el **UART4** de la Nucleo para el módulo Bluetooth (los pines exactos son los que tengas asignados a UART4 en CubeMX / el `.ioc`).

| Módulo (HC-05) | Nucleo |
|---|---|
| `VCC` | `5V` (la mayoría de los módulos con placa soportan 3,6–6 V) |
| `GND` | `GND` |
| `TXD` | Pin **RX** del UART4 |
| `RXD` | Pin **TX** del UART4 |
| `STATE` | Pin etiquetado **`BT_STATE`** en CubeMX (entrada digital) |

> **Cruzar TX/RX:** el TXD del módulo va al RX de la placa y viceversa.
>
> **Niveles lógicos:** el STM32 trabaja a 3,3 V, igual que el pin RXD del HC-05; no conectes señales de 5 V a los pines del microcontrolador.
>
> **El cable `STATE` es necesario.** El firmware lo lee para saber si hay un dispositivo conectado: nivel alto = enlace activo. Si no está cableado, la placa creerá que no hay enlace, no atenderá comandos y el LED LD1 parpadeará rápido (ver sección 11). Si tu módulo no tiene pin STATE (por ejemplo, algunos HC-06 de 4 pines), borrá la etiqueta `BT_STATE` en CubeMX: el código compila igual y el driver asume que el enlace siempre está activo.

---

## 4. Emparejar el módulo con la PC (Windows)

1. Alimentá la placa. El LED del HC-05 debe parpadear rápido (esperando emparejamiento).
2. En Windows: **Configuración → Bluetooth y dispositivos → Agregar dispositivo → Bluetooth**.
3. Elegí **HC-05** (o HC-06) e ingresá el PIN. Por defecto es **`1234`** (o `0000`).
4. Una vez emparejado, el LED del módulo parpadea lento.
5. Windows crea **puertos COM virtuales**. Para ver cuál es el tuyo: *Administrador de dispositivos → Puertos (COM y LPT)*, o *Panel de control → Dispositivos e impresoras → (tu módulo) → Propiedades → Hardware*.

> Windows suele crear **dos** puertos COM por dispositivo Bluetooth: uno **entrante** y uno **saliente**. La app debe usar el **saliente** (`Standard Serial over Bluetooth link`). Si un puerto no funciona, probá el otro.

En **Linux**, el emparejamiento se hace con `bluetoothctl` y luego se vincula con `rfcomm bind /dev/rfcomm0 <MAC>`; el puerto será `/dev/rfcomm0`.

---

## 5. Parámetros de comunicación

| Parámetro | Valor |
|---|---|
| Velocidad (baudios) | **9600** (configurado en `huart4` del firmware y valor de fábrica del HC-05 en modo datos) |
| Datos / paridad / stop | 8 N 1 |
| Control de flujo | Ninguno |

La velocidad de la app debe coincidir con la del UART4 del firmware (`huart4.Init.BaudRate`). Si cambiás la velocidad del módulo con comandos AT, cambiala también en CubeMX y en la app.

> El `main.c` también configura `USART3` a 115200 (puerto serie virtual del ST-Link). **No** es el que se usa para el Bluetooth.

---

## 6. Cómo ejecutar la app

```bash
python bt_dashboard.py
```

### Primer uso paso a paso

1. Con el módulo emparejado y la placa encendida, abrí la app.
2. En **Puerto** elegí el COM del módulo (los Bluetooth aparecen marcados con `[BT]`). Con **⟳** o **F5** se actualiza la lista.
3. Verificá **Baudios** = 9600.
4. Presioná **Conectar**. El estado cambia a `● Conectado` en verde.
5. En la terminal deberían empezar a aparecer los mensajes de la placa.
6. Presioná **A · LED ON** (o **F1**): se enciende el LED LD3 de la placa y el indicador de la app pasa a verde cuando llega la confirmación.

---

## 7. Protocolo de comunicación

### PC → STM32

| Carácter | Acción en el firmware |
|---|---|
| `A` | Enciende el LED **LD3** y responde con el ACK de encendido |
| `B` | Apaga el LED **LD3** y responde con el ACK de apagado |
| `\r`, `\n` y cualquier otro carácter | **Se ignoran**: no cambian nada ni generan respuesta |

El firmware procesa **byte por byte**, por lo que funciona igual si la app envía `A` o `A\n`: el terminador de la app (LF, CR, CR+LF o ninguno) no afecta.

### STM32 → PC

Todos los mensajes terminan en **`\n`**; la app arma las líneas cortando por ese carácter.

| Mensaje | Cuándo se envía | Uso en la app |
|---|---|---|
| `[STM32] Comando A ejecutado: LED ON\n` | Al recibir `A` | **Confirmación (ACK):** el indicador del LED pasa a ENCENDIDO. |
| `[STM32] Comando B ejecutado: LED OFF\n` | Al recibir `B` | **Confirmación (ACK):** el indicador pasa a APAGADO. |
| `soy una nucleo y estoy conectada\n` | Cada `TELEMETRY_PERIOD_MS` (200 ms) | Se muestra en la terminal y mantiene activo el indicador de enlace. |

> **Importante:** el indicador del LED no cambia al presionar el botón sino **cuando llega el ACK**. Lo que busca la app es el texto `Comando A ejecutado` / `Comando B ejecutado`; si cambiás esos mensajes en el firmware, ajustá la expresión regular `ACK_RE` en `bt_dashboard.py`.

> **Seguridad del firmware:** si el módulo pierde el enlace (pin `BT_STATE` en bajo), la placa **apaga LD3** (estado seguro). Al reconectar, el LED queda apagado hasta que se envíe `A`.

---

## 8. Firmware de la placa (`main.c`)

El firmware usa el driver propio `bt_STM32.c/.h`, que soporta **HC-05, HC-06 y HM-10**, y recibe por interrupciones. Asegurate de que ambos archivos estén en el proyecto (`Core/Src` y `Core/Inc`).

### Configuración en CubeMX

- **UART4:** modo *Asynchronous*, 9600 baudios, 8 bits, sin paridad, 1 stop bit, y **interrupción global de UART4 habilitada** (NVIC).
- **Pines de LED:** `LD1`, `LD2`, `LD3` como salida (GPIOB en las Nucleo-144).
- **Pin `BT_STATE`:** entrada digital (GPIO_Input), con ese nombre exacto en *User Label*.

### Estructura del programa

```c
#include "bt_STM32.h"

#define TELEMETRY_PERIOD_MS   200u    /* periodo del mensaje periódico        */
#define LINK_BLINK_PERIOD_MS  100u    /* parpadeo de LD1 cuando no hay enlace */

BT_Handler_t bt_mod;                  /* manejador global del driver */

/* --- en main(), antes del while(1) --- */
BT_Init(&bt_mod, &huart4, BT_TYPE_HC05, 100);   /* UART4, timeout de TX 100 ms */
BT_ConfigStatePin(&bt_mod, BT_STATE_GPIO_Port, BT_STATE_Pin);

/* --- dentro del while(1) --- */
BT_Status_t link = BT_CheckAndRecover(&bt_mod);   /* supervisa enlace y recepción */

if (link == BT_OK) {
    HAL_GPIO_WritePin(LD1_GPIO_Port, LD1_Pin, GPIO_PIN_RESET);   /* LD1 apagado */

    int16_t c;
    while ((c = BT_ReadChar(&bt_mod)) != -1) {          /* lectura no bloqueante */
        switch (c) {
            case 'A':
                HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_SET);
                BT_SendString(&bt_mod, "[STM32] Comando A ejecutado: LED ON\n");
                break;
            case 'B':
                HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
                BT_SendString(&bt_mod, "[STM32] Comando B ejecutado: LED OFF\n");
                break;
            default:
                break;                                  /* '\r', '\n' y otros: se ignoran */
        }
    }

    /* mensaje periódico */
    uint32_t now = HAL_GetTick();
    if ((now - telemetry_tick) >= TELEMETRY_PERIOD_MS) {
        telemetry_tick = now;
        BT_SendString(&bt_mod, "soy una nucleo y estoy conectada\n");
    }
} else {
    /* sin enlace: LD3 apagado y LD1 parpadeando */
    HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, GPIO_PIN_RESET);
    if ((HAL_GetTick() - error_tick) >= LINK_BLINK_PERIOD_MS) {
        error_tick = HAL_GetTick();
        HAL_GPIO_TogglePin(LD1_GPIO_Port, LD1_Pin);
    }
}
```

Además, en la sección `USER CODE 4` del `main.c` van dos callbacks que derivan las interrupciones del UART al driver (son obligatorios):

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == bt_mod.huart) { BT_RxISR_Callback(&bt_mod); }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == bt_mod.huart) { BT_ErrorISR_Callback(&bt_mod); }
}
```

### Indicaciones de los LEDs de la placa

| LED | Significado |
|---|---|
| **LD3** | Controlado por la app: encendido con `A`, apagado con `B` |
| **LD1** | Apagado = enlace Bluetooth activo · **parpadeando rápido (100 ms) = sin enlace** |

### Usar otro módulo

En `BT_Init(...)` el tercer parámetro indica el tipo de módulo (`BT_TYPE_HC05` en este proyecto). Para HC-06 o HM-10 usá la constante correspondiente que figura en `bt_STM32.h`.

---

## 9. Guía de la interfaz

### Barra de conexión
- **Puerto / ⟳:** lista de puertos; se actualiza al desplegarla.
- **Baudios:** velocidad de la comunicación.
- **Conectar / Desconectar / Cancelar:** cambia según el estado.
- **Reconexión automática:** reintenta cada 2 s si se pierde el enlace.
- **Estado:** `Desconectado` (rojo), `Conectando…` (amarillo), `Conectado` (verde), `Reconectando…` (naranja).

### Terminal
- **Colores:** `TX ▶` celeste (lo que envía la PC), `RX ◀` verde (lo que llega), `SYS` amarillo (eventos de la app), `ERR` rojo (errores).
- **Autoscroll / Hora / Ocultar telemetría / Pausa:** opciones de visualización. En pausa, los mensajes se retienen y se muestran al reanudar. *Ocultar telemetría* filtra solo las líneas con formato `ADC_VAL:<n> | TICK:<ms>`.
- **Limpiar / Guardar:** borra la vista o exporta todo el registro a `.txt`.
- **Línea de comandos:** escribí un texto y presioná **Enter** o **Enviar**. Con ↑ ↓ recorrés el historial.
- **Terminador:** `Ninguno`, `LF (\n)`, `CR (\r)` o `CR+LF (\r\n)`. Se agrega a todo lo que se envía; con este firmware no afecta.

### Control del STM32
- **Indicador del LED:** gris (desconocido), verde (encendido), rojo oscuro (apagado). Se actualiza solo con el ACK del firmware.
- **Indicador de enlace:** verde si llegó algún dato en los últimos 2 s.
- **A · LED ON (F1)** y **B · LED OFF (F2).**

### Pruebas
Los parámetros **Repeticiones** (N) e **Intervalo (ms)** aplican a las pruebas con secuencia.

| Botón | Qué hace | Qué esperar con este firmware |
|---|---|---|
| ⇄ Alternar LED (F3) | Envía lo contrario del estado actual (si es desconocido, envía `A`). | Un ACK. |
| ✦ Parpadeo ×N | Envía `A`,`B`,`A`,`B`… N veces cada par. | El LED LD3 parpadea y llega un ACK por cada comando. |
| A ×N / B ×N | Ráfaga de N comandos iguales. | N ACKs idénticos. |
| ⚡ Stress A/B 5 s | Alterna `A` y `B` durante ~5 s al intervalo elegido. | LD3 parpadea rápido; ver nota de velocidad abajo. |
| ABAB (cadena) | Envía los 4 caracteres juntos, en un solo paquete. | 4 ACKs seguidos (A, B, A, B): confirma que la placa procesa varios bytes por ciclo. |
| ? Comando inválido (X) | Envía `X`. | **Sin respuesta**: el firmware ignora cualquier carácter que no sea `A` o `B`; el LED no cambia. |
| ↵ Solo terminador | Envía únicamente el terminador seleccionado. | Sin respuesta (se ignora). |
| ■ DETENER PRUEBA (Esc) | Cancela la secuencia en curso. | — |

Las secuencias se cancelan solas si se pierde la conexión. Los botones de prueba solo se habilitan con conexión activa.

> **Velocidad máxima:** a 9600 baudios cada ACK (~37 bytes) tarda unos 38 ms en transmitirse, y además la placa envía el mensaje periódico. Con intervalos menores a ~100 ms los ACKs empiezan a acumularse y llegar con retraso. Para las pruebas de ráfaga y stress se recomienda un intervalo de **100 ms o más**.

### Macros
Seis ranuras `M1`–`M6` con texto editable; el botón **Enviar** manda el texto más el terminador. Se guardan entre sesiones. Con este firmware, solo los caracteres `A` y `B` tienen efecto (por ejemplo, una macro `ABBA` enciende, apaga, apaga y enciende).

### Atajos de teclado

| Tecla | Acción |
|---|---|
| F1 / F2 | Enviar `A` / `B` |
| F3 | Alternar LED |
| F5 | Actualizar lista de puertos |
| Esc | Detener prueba |
| Ctrl+L | Limpiar terminal |
| Ctrl+S | Guardar registro |
| Enter | Enviar comando libre |
| ↑ / ↓ | Historial de comandos |

### Barra de estado
Muestra el puerto y la velocidad, los bytes recibidos (RX) y enviados (TX), y el tiempo conectado.

---

## 10. Ejemplo completo de uso

1. Cargá el firmware en la Nucleo y verificá el cableado del módulo (incluido el cable `STATE`).
2. Al encender, **LD1 parpadea rápido**: la placa espera que alguien se conecte.
3. Emparejá el módulo con la PC (PIN `1234`).
4. Ejecutá `python bt_dashboard.py`.
5. Elegí el puerto `[BT]`, baudios `9600`, **Conectar**. Al establecerse el enlace, **LD1 se apaga**.
6. En la terminal verás varias veces por segundo `RX ◀ soy una nucleo y estoy conectada`, y el enlace en `● STM32 enlazado`.
7. Presioná **F1**:
   ```
   TX ▶ A\n
   RX ◀ [STM32] Comando A ejecutado: LED ON
   ```
   LD3 se enciende y el indicador de la app se pone en verde.
8. Presioná **F2** y verificá que LD3 se apaga y el indicador pasa a rojo oscuro.
9. En **Pruebas** poné Repeticiones = `5` e Intervalo = `300` y presioná **✦ Parpadeo ×N**: LD3 parpadea 5 veces y cada ACK aparece en la terminal.
10. Presioná **? Comando inválido (X)**: no debería aparecer ninguna respuesta ni cambiar el LED (el firmware lo ignora).
11. Presioná **Desconectar**: la placa detecta que se perdió el enlace, apaga LD3 y LD1 vuelve a parpadear.
12. Presioná **Guardar** para exportar el registro de la sesión.

---

## 11. Archivo de configuración

La app guarda sus preferencias en:

- Windows: `C:\Users\<usuario>\.bt_dashboard.json`
- Linux / macOS: `~/.bt_dashboard.json`

Incluye puerto, baudios, terminador, opciones de la terminal, parámetros de prueba, macros y tamaño de ventana. Para restablecer todo a valores de fábrica, cerrá la app y borrá ese archivo.

---

## 12. Solución de problemas

| Síntoma | Causa probable | Solución |
|---|---|---|
| `Falta la librería pyserial` | No está instalada. | `pip install pyserial` |
| No aparece el puerto del módulo | No está emparejado. | Emparejalo desde Windows y presioná ⟳ / F5. |
| `No se pudo abrir el puerto` | Puerto ocupado o incorrecto. | Cerrá otros programas (monitor serie, PuTTY, CubeIDE). Probá el otro COM del módulo (entrante/saliente). |
| **LD1 parpadea rápido aunque estás conectado** | El pin `STATE` no llega a la placa (cable suelto o mal pin) o `BT_STATE` apunta a otro pin. | Revisá el cable `STATE` → pin `BT_STATE`. Con el firmware actual, sin enlace la placa **no atiende comandos**. |
| Conecta pero no llega nada | TX/RX sin cruzar, UART distinto o baudios distintos. | Revisá cableado, que sea UART4 y que ambos lados estén a 9600. |
| Caracteres raros en la terminal | Baudios distintos entre app y STM32. | Igualá velocidades en ambos lados. |
| El LED responde pero el indicador queda en "desconocido" | El ACK no coincide con lo que busca la app. | El texto debe contener `Comando A ejecutado` / `Comando B ejecutado`, o ajustá `ACK_RE`. |
| Los mensajes aparecen pegados en una línea | Faltan los `\n` al final. | Terminá cada `BT_SendString` con `\n`. |
| La terminal se llena de "soy una nucleo…" | El mensaje periódico es cada 200 ms. | Usá **Pausa**, o subí `TELEMETRY_PERIOD_MS` en el firmware (por ejemplo 1000). |
| Las pruebas de ráfaga/stress llegan atrasadas | El intervalo es menor que el tiempo de transmisión de los ACK a 9600 baudios. | Usá un intervalo de 100 ms o más. |
| Se corta la conexión y reconecta | Mala alimentación o distancia. | Alimentá bien el módulo y acercalo a la PC. |
| `Sin datos hace N s` | El firmware dejó de enviar. | Revisá que la placa no se haya colgado o reiniciado; si el enlace se perdió, LD1 estará parpadeando. |
| El HM-10 no aparece como puerto COM | Es BLE, no Bluetooth clásico. | Usá HC-05/HC-06 con esta app. |

---

## 13. Estructura interna (para quien quiera modificar el código)

- **`SerialWorker`**: hilo que abre el puerto, lee, arma líneas por `\n` y las envía a la GUI por una cola. Tkinter nunca se toca desde este hilo.
- **`Dashboard`**: ventana principal. Toda la lógica de interfaz, estados de conexión, envío, pruebas, terminal y configuración.
- **Constantes** al inicio del archivo: `STALE_AFTER_S` (tiempo para marcar "sin datos"), `RECONNECT_DELAY_MS`, `MAX_TERM_LINES`, `N_MACROS`, etc.
- **Expresiones regulares** `TELE_RE` (líneas `ADC_VAL`) y `ACK_RE` (confirmación del LED): si cambiás los mensajes del firmware, ajustalas ahí.