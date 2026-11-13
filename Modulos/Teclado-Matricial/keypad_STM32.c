/*
 * ============================================================================
 * ARCHIVO: keypad_STM32.c
 * AUTOR: Mamani Flores Carlos (UTN FRT)
 * PROYECTO: Biblioteca de Drivers Modulares en C para STM32 (TD2 - UTN FRT)
 * VERSION: 1.1
 * FECHA: 2026
 *
 * DESCRIPCION:
 * Implementation file para el control de teclados matriciales (4x4 / 4x3)
 * mediante multiplexado asíncrono y filtrado de rebotes por MEF temporalizada
 * (tanto en la pulsación como en la liberación).
 * ============================================================================
 */

#include "keypad_STM32.h"

/*
 * ============================================================================
 * FUNCIONES PRIVADAS / HELPER DE CAPA BAJA
 * ============================================================================
 */

/*
 * Keypad_ResetRows:
 * Pone todas las filas configuradas en estado inactivo (nivel alto/HIGH).
 * Esto garantiza un punto de partida conocido antes de activar individualmente
 * la fila bajo evaluación.
 */
static void Keypad_ResetRows(const Keypad_t *keypad) {
    for (uint8_t r = 0; r < KEYPAD_ROWS; r++) {
        HAL_GPIO_WritePin(keypad->row_pins[r].port, keypad->row_pins[r].pin, GPIO_PIN_SET);
    }
}

/*
 * Keypad_ScanColumns:
 * Realiza la lectura del puerto GPIO mapeado a las columnas para la fila que se
 * encuentra actualmente activa en nivel bajo.
 *
 * Retorno:
 *  - Indice de la columna en LOW (0 a KEYPAD_COLS - 1)
 *  - (-1) si ninguna columna se encuentra presionada.
 */
static int8_t Keypad_ScanColumns(const Keypad_t *keypad) {
    for (uint8_t c = 0; c < KEYPAD_COLS; c++) {
        if (HAL_GPIO_ReadPin(keypad->col_pins[c].port, keypad->col_pins[c].pin) == GPIO_PIN_RESET) {
            return (int8_t)c;
        }
    }
    return -1;
}

/*
 * ============================================================================
 * API PUBLICA DEL DRIVER
 * ============================================================================
 */

/*
 * Keypad_Init:
 * Carga las estructuras de hardware (mapeo de pines) y la matriz de caracteres.
 * Establece los estados iniciales de la MEF, marcas de tiempo y coloca las
 * filas en estado inactivo para iniciar el barrido seguro.
 * NOTA: no configura los GPIO (modo, pull, velocidad); eso se hace en CubeMX.
 */
void Keypad_Init(Keypad_t *keypad, const KeypadPin_t rows[KEYPAD_ROWS],
                 const KeypadPin_t cols[KEYPAD_COLS], const char map[KEYPAD_ROWS][KEYPAD_COLS])
{
    if (keypad == NULL || rows == NULL || cols == NULL || map == NULL) return;

    /* Cargar mapeo de pines de filas */
    for (uint8_t r = 0; r < KEYPAD_ROWS; r++) {
        keypad->row_pins[r] = rows[r];
    }

    /* Cargar mapeo de pines de columnas */
    for (uint8_t c = 0; c < KEYPAD_COLS; c++) {
        keypad->col_pins[c] = cols[c];
    }

    /* Copiar la matriz de mapeo de teclas */
    for (uint8_t r = 0; r < KEYPAD_ROWS; r++) {
        for (uint8_t c = 0; c < KEYPAD_COLS; c++) {
            keypad->key_map[r][c] = map[r][c];
        }
    }

    /* Inicializar variables internas de control */
    keypad->current_row = 0;
    keypad->last_scan_tick = 0;
    keypad->debounce_time_ms = KEYPAD_DEBOUNCE_MS;
    keypad->last_key = '\0';
    keypad->current_key = '\0';
    keypad->fsm_state = KEYPAD_STATE_IDLE;
    keypad->last_event = KEYPAD_EVENT_NONE;

    /* Forzar todas las filas a estado inactivo (ALTO) */
    Keypad_ResetRows(keypad);
}

/*
 * Keypad_Update:
 * Maquina de Estados Finita (MEF) no bloqueante.
 *
 * Flujo de operación:
 * 1. IDLE: Restablece filas, activa la fila actual en LOW y consulta columnas.
 *    Si detecta una columna en LOW, registra la tecla y pasa a DEBOUNCE.
 *    Si no, avanza a la siguiente fila.
 * 2. DEBOUNCE: Pasado KEYPAD_DEBOUNCE_MS, vuelve a medir la columna. Si la tecla
 *    se mantiene, la valida, genera EVENT_PRESSED y pasa a PRESSED. Si no, vuelve
 *    a IDLE (ruido).
 * 3. PRESSED: Espera a que todas las columnas retornen a HIGH. Al detectarlo
 *    pasa a RELEASE_DEBOUNCE.
 * 4. RELEASE_DEBOUNCE: Si la tecla vuelve a verse presionada (rebote), regresa
 *    a PRESSED. Si se mantiene suelta durante KEYPAD_DEBOUNCE_MS, genera
 *    EVENT_RELEASED y vuelve a IDLE.
 */
char Keypad_Update(Keypad_t *keypad) {
    if (keypad == NULL) return '\0';

    uint32_t current_tick = HAL_GetTick();
    char detected_key = '\0';
    int8_t col_pressed;

    switch (keypad->fsm_state) {

        case KEYPAD_STATE_IDLE:
            /* Restablecer estado de filas antes de activar la siguiente */
            Keypad_ResetRows(keypad);

            /* Activar en BAJO únicamente la fila bajo evaluación */
            HAL_GPIO_WritePin(keypad->row_pins[keypad->current_row].port,
                              keypad->row_pins[keypad->current_row].pin,
                              GPIO_PIN_RESET);

            col_pressed = Keypad_ScanColumns(keypad);

            if (col_pressed != -1) {
                /* Detección inicial: guardar tecla, tomar timestamp y pasar a debounce */
                keypad->current_key = keypad->key_map[keypad->current_row][col_pressed];
                keypad->last_scan_tick = current_tick;
                keypad->fsm_state = KEYPAD_STATE_DEBOUNCE;
            } else {
                /* Avanzar a la siguiente fila de forma cíclica */
                keypad->current_row = (uint8_t)((keypad->current_row + 1) % KEYPAD_ROWS);
            }
            break;

        case KEYPAD_STATE_DEBOUNCE:
            /* Evaluación de estabilidad temporal no bloqueante con HAL_GetTick() */
            if ((current_tick - keypad->last_scan_tick) >= keypad->debounce_time_ms) {
                col_pressed = Keypad_ScanColumns(keypad);

                /* Confirmar si la tecla registrada sigue activa tras el periodo de rebote */
                if (col_pressed != -1 &&
                    keypad->key_map[keypad->current_row][col_pressed] == keypad->current_key)
                {
                    keypad->last_key = keypad->current_key;
                    keypad->last_event = KEYPAD_EVENT_PRESSED;
                    detected_key = keypad->current_key;
                    keypad->fsm_state = KEYPAD_STATE_PRESSED;
                } else {
                    /* Falso positivo / Ruido mecánico -> Retornar a IDLE */
                    keypad->fsm_state = KEYPAD_STATE_IDLE;
                }
            }
            break;

        case KEYPAD_STATE_PRESSED:
            /* Esperar la liberación de la tecla (columnas en ALTO) */
            if (Keypad_ScanColumns(keypad) == -1) {
                keypad->last_scan_tick = current_tick;
                keypad->fsm_state = KEYPAD_STATE_RELEASE_DEBOUNCE;
            }
            break;

        case KEYPAD_STATE_RELEASE_DEBOUNCE:
            if (Keypad_ScanColumns(keypad) != -1) {
                /* La tecla volvió a verse presionada: fue un rebote de liberación */
                keypad->fsm_state = KEYPAD_STATE_PRESSED;
            } else if ((current_tick - keypad->last_scan_tick) >= keypad->debounce_time_ms) {
                /* Liberación estable durante todo el periodo de debounce */
                keypad->last_event = KEYPAD_EVENT_RELEASED;
                keypad->fsm_state = KEYPAD_STATE_IDLE;
                Keypad_ResetRows(keypad);
            }
            break;

        default:
            keypad->fsm_state = KEYPAD_STATE_IDLE;
            break;
    }

    return detected_key;
}

/*
 * Keypad_GetKey:
 * Devuelve el último carácter confirmado como presionado.
 */
char Keypad_GetKey(const Keypad_t *keypad) {
    if (keypad == NULL) return '\0';
    return keypad->last_key;
}

/*
 * Keypad_GetEvent:
 * Devuelve el último evento generado (PRESSED / RELEASED / NONE).
 */
KeypadEvent_t Keypad_GetEvent(const Keypad_t *keypad) {
    if (keypad == NULL) return KEYPAD_EVENT_NONE;
    return keypad->last_event;
}
