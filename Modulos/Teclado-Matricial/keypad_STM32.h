/**
 * @file keypad_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Driver modular no bloqueante para teclados matriciales (4x4 / 4x3) en STM32 HAL.
 * @details Implementa barrido multiplexado asíncrono y filtrado de rebotes (pulsación y
 *          liberación) por MEF mediante HAL_GetTick(). Se incluye "main.h" para heredar
 *          la familia HAL correcta del proyecto, de modo que el driver sirva para
 *          cualquier micro de ST.
 * @note El driver NO configura los GPIO: eso lo hace CubeMX / el usuario.
 *       - Filas: salida. Se recomienda Open Drain para evitar cortocircuitos
 *         entre filas cuando se presionan dos teclas de la misma columna.
 *       - Columnas: entrada con Pull-Up.
 * @version 1.1
 * @date 2026
 */

#ifndef KEYPAD_STM32_H_
#define KEYPAD_STM32_H_

#include "main.h"

/** @brief Número de filas del teclado (redefinible antes de incluir este header) */
#ifndef KEYPAD_ROWS
#define KEYPAD_ROWS 4
#endif

/** @brief Número de columnas del teclado (redefinible; ej: 3 para un 4x3) */
#ifndef KEYPAD_COLS
#define KEYPAD_COLS 4
#endif

/** @brief Tiempo por defecto para el filtrado de rebotes (debounce) en milisegundos */
#ifndef KEYPAD_DEBOUNCE_MS
#define KEYPAD_DEBOUNCE_MS 30
#endif

/**
 * @brief Estados de la máquina de estados (MEF) interna del teclado
 */
typedef enum {
    KEYPAD_STATE_IDLE = 0,          /**< Reposo y barrido continuo de filas */
    KEYPAD_STATE_DEBOUNCE,          /**< Espera de estabilización tras detectar pulsación */
    KEYPAD_STATE_PRESSED,           /**< Tecla confirmada; espera a la liberación */
    KEYPAD_STATE_RELEASE_DEBOUNCE   /**< Espera de estabilización tras detectar liberación */
} KeypadState_t;

/**
 * @brief Eventos generados por la lectura del teclado
 */
typedef enum {
    KEYPAD_EVENT_NONE = 0,  /**< Sin evento detectado */
    KEYPAD_EVENT_PRESSED,   /**< Evento de tecla presionada */
    KEYPAD_EVENT_RELEASED   /**< Evento de tecla liberada */
} KeypadEvent_t;

/**
 * @brief Estructura de mapeo de puerto y pin GPIO para filas y columnas
 */
typedef struct {
    GPIO_TypeDef* port;  /**< Puerto GPIO (ej: GPIOA, GPIOB) */
    uint16_t pin;        /**< Pin GPIO (ej: GPIO_PIN_0) */
} KeypadPin_t;

/**
 * @brief Estructura principal de control del teclado matricial
 */
typedef struct {
    KeypadPin_t row_pins[KEYPAD_ROWS];        /**< Mapeo de pines de filas */
    KeypadPin_t col_pins[KEYPAD_COLS];        /**< Mapeo de pines de columnas */
    char key_map[KEYPAD_ROWS][KEYPAD_COLS];   /**< Matriz de mapeo de caracteres */
    uint8_t current_row;                      /**< Índice de la fila activa en el barrido */
    uint32_t last_scan_tick;                  /**< Marca de tiempo (tick) para el debounce */
    uint32_t debounce_time_ms;                /**< Tiempo configurado de debounce en ms */
    char current_key;                         /**< Tecla detectada en evaluación */
    char last_key;                            /**< Última tecla confirmada como presionada */
    KeypadState_t fsm_state;                  /**< Estado actual de la MEF */
    KeypadEvent_t last_event;                 /**< Último evento generado */
} Keypad_t;

/**
 * @brief Inicializa la estructura de control del teclado (mapeo de pines, mapa de
 *        teclas y MEF). No configura los GPIO; solo deja las filas en estado inactivo (HIGH).
 *
 * @param keypad Puntero a la estructura de control Keypad_t.
 * @param rows Arreglo de KeypadPin_t con las filas.
 * @param cols Arreglo de KeypadPin_t con las columnas.
 * @param map Matriz de caracteres [KEYPAD_ROWS][KEYPAD_COLS].
 * @note Si algún puntero es NULL, la función retorna sin modificar nada.
 */
void Keypad_Init(Keypad_t *keypad, const KeypadPin_t rows[KEYPAD_ROWS],
                 const KeypadPin_t cols[KEYPAD_COLS], const char map[KEYPAD_ROWS][KEYPAD_COLS]);

/**
 * @brief Actualiza la MEF de barrido y filtrado de rebotes. Debe ejecutarse
 *        periódicamente en el super-loop.
 *
 * @param keypad Puntero a la estructura de control Keypad_t.
 * @return char Tecla presionada si se valida un nuevo evento, o '\0' si no hay nueva tecla.
 */
char Keypad_Update(Keypad_t *keypad);

/**
 * @brief Obtiene la última tecla confirmada que fue presionada.
 *
 * @param keypad Puntero a la estructura de control Keypad_t.
 * @return char Última tecla registrada, o '\0' si no hubo ninguna.
 */
char Keypad_GetKey(const Keypad_t *keypad);

/**
 * @brief Obtiene el último evento generado (presionada / liberada).
 *
 * @param keypad Puntero a la estructura de control Keypad_t.
 * @return KeypadEvent_t Último evento, o KEYPAD_EVENT_NONE si keypad es NULL.
 */
KeypadEvent_t Keypad_GetEvent(const Keypad_t *keypad);

#endif /* KEYPAD_STM32_H_ */
