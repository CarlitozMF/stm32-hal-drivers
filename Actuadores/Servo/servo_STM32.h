/**
 * @file servo_STM32.h
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Driver para servomotores tipo SG90 / MG996R / MG946R (PWM 50 Hz).
 * @details Características (v2.0):
 *          - N servos independientes (una instancia Servo_t por servo).
 *          - Movimiento instantáneo o suave, NO bloqueante (HAL_GetTick).
 *          - Movimiento por velocidad (°/s) o por tiempo (ms).
 *          - Perfiles de aceleración (easing): lineal, entrada, salida y entrada-salida suave.
 *          - Ángulos con decimales (float).
 *          - Límites por software, inversión de sentido y trim (offset) por servo.
 *          - Attach / Detach del PWM, Stop y control directo del pulso en µs.
 *          - Callback de fin de movimiento.
 *          - Secuencias de waypoints por servo.
 *          - Grupos de servos: movimiento sincronizado (llegan todos a la vez),
 *            secuencias de poses y callback de fin de grupo.
 *
 * @note Configuración del timer (CubeMX):
 *       - Canal en modo "PWM Generation CHx", período de 20 ms (50 Hz).
 *       - Recomendado 1 tick = 1 µs (PSC = f_timer/1MHz - 1, ARR = 19999).
 *         Con eso: min_pulse ≈ 500..1000 y max_pulse ≈ 2000..2500.
 *       - Si usás otra base de tiempo, definí SERVO_TICKS_PER_US (solo afecta a SERVO_SetPulseUs).
 *
 * @note Todas las funciones de movimiento son no bloqueantes: hay que llamar
 *       SERVO_Update() (o SERVO_GROUP_Update()) en el while(1).
 *       Los callbacks se ejecutan DENTRO de Update(), nunca en una ISR.
 *
 * @par Ejemplo 1: un servo
 * @code
 *   Servo_t servo1;
 *   SERVO_Init(&servo1, &htim3, TIM_CHANNEL_1, 600, 2400);
 *   SERVO_SetEasing(&servo1, SERVO_EASE_IN_OUT);
 *   SERVO_MoveTime(&servo1, 120.0f, 800);        // 120° en 800 ms
 *   while (1) { SERVO_Update(&servo1); }
 * @endcode
 *
 * @par Ejemplo 2: brazo con 3 servos sincronizados
 * @code
 *   Servo_t base, hombro, codo;
 *   Servo_t *lista[] = { &base, &hombro, &codo };
 *   ServoGroup_t brazo;
 *
 *   SERVO_Init(&base,   &htim3, TIM_CHANNEL_1, 600, 2400);
 *   SERVO_Init(&hombro, &htim3, TIM_CHANNEL_2, 600, 2400);
 *   SERVO_Init(&codo,   &htim3, TIM_CHANNEL_3, 600, 2400);
 *   SERVO_GROUP_Init(&brazo, lista, 3);
 *
 *   // Poses (deben ser static/global: el driver guarda el puntero)
 *   static const float pose_a[3] = {  90.0f, 45.0f, 120.0f };
 *   static const float pose_b[3] = { 180.0f, SERVO_GROUP_KEEP, 60.0f }; // hombro no se mueve
 *   static const ServoGroupStep_t rutina[] = {
 *       { pose_a, 1000, 500 },   // ir a pose_a en 1 s y esperar 500 ms
 *       { pose_b, 1500,   0 },
 *   };
 *   SERVO_GROUP_StartSequence(&brazo, rutina, 2, 1);  // 1 = repetir
 *
 *   while (1) { SERVO_GROUP_Update(&brazo); }
 * @endcode
 *
 * @version 2.0
 * @date 2026
 */

#ifndef SERVO_STM32_H
#define SERVO_STM32_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stddef.h>

/* Exported constants --------------------------------------------------------*/

/** @brief Ángulo mínimo absoluto, en grados. */
#define SERVO_ANGLE_MIN   0
/** @brief Ángulo máximo absoluto, en grados. */
#define SERVO_ANGLE_MAX   180

/**
 * @brief Ticks del timer por microsegundo (usado por SERVO_SetPulseUs).
 * @note Con 1 tick = 1 µs vale 1. Se puede redefinir antes de incluir este header.
 */
#ifndef SERVO_TICKS_PER_US
#define SERVO_TICKS_PER_US  1U
#endif

/**
 * @brief Valor especial para un arreglo de ángulos de grupo:
 *        "no mover este servo" (conserva su posición/movimiento actual).
 */
#define SERVO_GROUP_KEEP   (-1000.0f)

/* Exported types ------------------------------------------------------------*/

/** @brief Perfil de velocidad de un movimiento. */
typedef enum {
    SERVO_EASE_LINEAR = 0,   /**< Velocidad constante. */
    SERVO_EASE_IN,           /**< Arranca suave y termina a velocidad plena. */
    SERVO_EASE_OUT,          /**< Arranca a velocidad plena y frena suave. */
    SERVO_EASE_IN_OUT        /**< Arranca y frena suave (el más natural). */
} ServoEasing_t;

/** @brief Estado interno de movimiento (servo o grupo). */
typedef enum {
    SERVO_STATE_IDLE = 0,    /**< Quieto. */
    SERVO_STATE_MOVING,      /**< Ejecutando un movimiento. */
    SERVO_STATE_HOLD         /**< Pausa entre pasos de una secuencia. */
} ServoState_t;

/**
 * @brief Paso de una secuencia de un servo individual.
 */
typedef struct {
    float    angle;          /**< Ángulo destino en grados. */
    uint32_t duration_ms;    /**< Tiempo para llegar (0 = salto instantáneo). */
    uint32_t hold_ms;        /**< Pausa al llegar, antes del siguiente paso. */
} ServoWaypoint_t;

/** @brief Tipo de la instancia de servo (definida más abajo). */
typedef struct Servo_s Servo_t;

/**
 * @brief Callback de fin de movimiento de un servo.
 * @param servo Servo que terminó.
 * @param ctx   Puntero de usuario registrado en SERVO_SetCallback().
 */
typedef void (*ServoCallback_t)(Servo_t *servo, void *ctx);

/**
 * @brief Instancia de un servo.
 * @note No modificar los campos a mano: usar siempre la API del driver.
 */
struct Servo_s {
    /* --- Hardware --- */
    TIM_HandleTypeDef *htim;      /**< Handle del timer (ej: &htim3). */
    uint32_t channel;             /**< Canal PWM (ej: TIM_CHANNEL_1). */

    /* --- Calibración y ajustes --- */
    uint32_t min_pulse;           /**< CCR para 0° (físico). */
    uint32_t max_pulse;           /**< CCR para 180° (físico). */
    float    limit_min;           /**< Límite inferior por software (°). */
    float    limit_max;           /**< Límite superior por software (°). */
    float    trim;                /**< Offset fino del cero (°), se suma al ángulo físico. */
    uint8_t  inverted;            /**< 1 = sentido invertido (ángulo = 180 - ángulo). */
    ServoEasing_t easing;         /**< Perfil de movimiento. */

    /* --- Estado del movimiento --- */
    float    current_angle;       /**< Ángulo actual estimado (°). */
    float    start_angle;         /**< Ángulo al iniciar el movimiento actual. */
    float    target_angle;        /**< Ángulo destino. */
    uint32_t start_time_ms;       /**< Instante de inicio del movimiento. */
    uint32_t duration_ms;         /**< Duración total del movimiento. */
    ServoState_t state;           /**< Estado (idle / moving / hold). */
    uint8_t  attached;            /**< 1 = PWM activo. */

    /* --- Callback --- */
    ServoCallback_t on_done;      /**< Callback de fin de movimiento (o NULL). */
    void    *cb_ctx;              /**< Contexto de usuario del callback. */

    /* --- Secuencia --- */
    const ServoWaypoint_t *seq;   /**< Secuencia en ejecución. */
    uint16_t seq_len;             /**< Cantidad de pasos. */
    uint16_t seq_index;           /**< Paso actual. */
    uint32_t hold_start_ms;       /**< Inicio de la pausa actual. */
    uint8_t  seq_loop;            /**< 1 = repetir indefinidamente. */
    uint8_t  seq_active;          /**< 1 = secuencia en curso. */
};

/** @brief Tipo de la instancia de grupo (definida más abajo). */
typedef struct ServoGroup_s ServoGroup_t;

/**
 * @brief Callback de fin de movimiento de un grupo.
 * @param group Grupo que terminó.
 * @param ctx   Puntero de usuario registrado en SERVO_GROUP_SetCallback().
 */
typedef void (*ServoGroupCallback_t)(ServoGroup_t *group, void *ctx);

/**
 * @brief Paso de una secuencia de poses de un grupo.
 */
typedef struct {
    const float *angles;     /**< Un ángulo por servo del grupo (o SERVO_GROUP_KEEP). Debe permanecer válido. */
    uint32_t duration_ms;    /**< Tiempo para llegar a la pose (todos llegan juntos). */
    uint32_t hold_ms;        /**< Pausa al llegar, antes del siguiente paso. */
} ServoGroupStep_t;

/**
 * @brief Instancia de un grupo de servos.
 * @note No modificar los campos a mano: usar siempre la API del driver.
 */
struct ServoGroup_s {
    Servo_t **servos;             /**< Arreglo de punteros a los servos. */
    uint8_t   count;              /**< Cantidad de servos. */

    ServoGroupCallback_t on_done; /**< Callback de fin (o NULL). */
    void     *cb_ctx;             /**< Contexto de usuario del callback. */
    uint8_t   active;             /**< 1 = hay un movimiento de grupo pendiente de notificar. */

    const ServoGroupStep_t *seq;  /**< Secuencia de poses en ejecución. */
    uint16_t  seq_len;            /**< Cantidad de pasos. */
    uint16_t  seq_index;          /**< Paso actual. */
    uint8_t   seq_loop;           /**< 1 = repetir. */
    ServoState_t seq_state;       /**< Estado de la secuencia. */
    uint32_t  hold_start_ms;      /**< Inicio de la pausa actual. */
};

/* ===========================================================================
 *                          API: SERVO INDIVIDUAL
 * =========================================================================*/

/**
 * @brief Inicializa el servo, arranca el PWM y lo posiciona en 0°.
 * @param servo Instancia (no NULL).
 * @param htim Handle del timer configurado en PWM.
 * @param channel Canal del timer (TIM_CHANNEL_x).
 * @param min CCR para 0°.
 * @param max CCR para 180°.
 * @note Valores por defecto: límites 0..180, trim 0, sin inversión, easing lineal.
 */
void SERVO_Init(Servo_t *servo, TIM_HandleTypeDef *htim, uint32_t channel, uint32_t min, uint32_t max);

/**
 * @brief Igual que Init, pero con ángulo inicial (evita el salto a 0°).
 * @param servo Instancia (no NULL).
 * @param htim Handle del timer configurado en PWM.
 * @param channel Canal del timer (TIM_CHANNEL_x).
 * @param min CCR para 0°.
 * @param max CCR para 180°.
 * @param initial_angle Ángulo inicial en grados (se limita a 0..180).
 */
void SERVO_InitAt(Servo_t *servo, TIM_HandleTypeDef *htim, uint32_t channel, uint32_t min, uint32_t max, float initial_angle);

/**
 * @brief Posicionamiento instantáneo (ángulo entero).
 * @param servo Instancia del servo.
 * @param angle Ángulo destino.
 * @note Cancela movimiento y secuencia en curso. Respeta los límites por software.
 *       Si el servo estaba detached, lo vuelve a activar.
 */
void SERVO_SetAngle(Servo_t *servo, int angle);

/**
 * @brief Posicionamiento instantáneo con decimales. Ver SERVO_SetAngle().
 * @param servo Instancia del servo.
 * @param angle Ángulo destino en grados.
 */
void SERVO_SetAngleF(Servo_t *servo, float angle);

/**
 * @brief Movimiento suave a velocidad dada (ángulo entero).
 * @param servo Instancia del servo.
 * @param target_angle Destino en grados.
 * @param speed_dps Velocidad en °/s. Si es <= 0, el movimiento es instantáneo.
 * @note Con easing distinto de LINEAL, speed_dps es la velocidad PROMEDIO
 *       (el pico llega a 1.5x en EASE_IN_OUT y 2x en EASE_IN / EASE_OUT).
 */
void SERVO_SetSpeedAngle(Servo_t *servo, int target_angle, float speed_dps);

/**
 * @brief Movimiento suave a velocidad dada, con decimales. Ver SERVO_SetSpeedAngle().
 * @param servo Instancia del servo.
 * @param target_angle Destino en grados.
 * @param speed_dps Velocidad en °/s.
 */
void SERVO_SetSpeedAngleF(Servo_t *servo, float target_angle, float speed_dps);

/**
 * @brief Movimiento suave que dura exactamente `duration_ms`.
 * @param servo Instancia del servo.
 * @param angle Destino en grados.
 * @param duration_ms Duración. Si es 0, salta al destino y notifica en el próximo Update().
 */
void SERVO_MoveTime(Servo_t *servo, float angle, uint32_t duration_ms);

/**
 * @brief Avanza el movimiento y las secuencias del servo. Llamar en el while(1).
 * @param servo Instancia del servo.
 */
void SERVO_Update(Servo_t *servo);

/**
 * @brief Congela el servo en su posición actual.
 * @param servo Instancia del servo.
 * @note Cancela movimiento y secuencia. No dispara el callback. El PWM sigue activo.
 */
void SERVO_Stop(Servo_t *servo);

/**
 * @brief Apaga el PWM del servo (sin torque, sin jitter, menor consumo).
 * @param servo Instancia del servo.
 * @note Cancela movimiento y secuencia. El servo ya no sostiene su posición.
 *       Cualquier comando de movimiento posterior lo reactiva automáticamente.
 */
void SERVO_Detach(Servo_t *servo);

/**
 * @brief Reactiva el PWM en la última posición conocida.
 * @param servo Instancia del servo.
 * @note Si el servo fue movido a mano estando detached, saltará a la posición
 *       que el driver recuerda.
 */
void SERVO_Attach(Servo_t *servo);

/**
 * @brief Consulta si el PWM está activo.
 * @param servo Instancia del servo.
 * @return 1 si el PWM está activo, 0 en caso contrario.
 */
uint8_t SERVO_IsAttached(const Servo_t *servo);

/**
 * @brief Control directo del ancho de pulso en µs. Útil para calibrar min/max.
 * @param servo Instancia del servo.
 * @param pulse_us Ancho de pulso deseado en microsegundos.
 * @note Ignora calibración, límites, trim e inversión. Cancela movimiento y
 *       secuencia. Estima current_angle a partir del pulso.
 */
void SERVO_SetPulseUs(Servo_t *servo, uint32_t pulse_us);

/**
 * @brief Define los límites de recorrido por software (ej: 20..160).
 * @param servo Instancia del servo.
 * @param min_angle Ángulo mínimo permitido.
 * @param max_angle Ángulo máximo permitido.
 * @note Los movimientos posteriores se limitan a este rango. No mueve el servo.
 */
void SERVO_SetLimits(Servo_t *servo, float min_angle, float max_angle);

/**
 * @brief Ajuste fino del cero, en grados (puede ser negativo).
 * @param servo Instancia del servo.
 * @param trim_deg Offset en grados.
 * @note Se aplica de inmediato sobre la posición actual.
 */
void SERVO_SetTrim(Servo_t *servo, float trim_deg);

/**
 * @brief Invierte el sentido de giro (0° <-> 180°).
 * @param servo Instancia del servo.
 * @param inverted 1 = invertido, 0 = normal.
 * @note Se aplica de inmediato: el servo puede saltar. Conviene llamarlo
 *       justo después de Init.
 */
void SERVO_SetInverted(Servo_t *servo, uint8_t inverted);

/**
 * @brief Selecciona el perfil de movimiento para los siguientes movimientos.
 * @param servo Instancia del servo.
 * @param easing Tipo de easing (LINEAL, EASE_IN, EASE_OUT, EASE_IN_OUT).
 */
void SERVO_SetEasing(Servo_t *servo, ServoEasing_t easing);

/**
 * @brief Registra el callback de fin de movimiento.
 * @param servo Instancia del servo.
 * @param cb Función a llamar (NULL para desactivar).
 * @param ctx Puntero de usuario que se pasa al callback.
 * @note Se llama al terminar un movimiento individual o una secuencia completa
 *       (no en cada paso de la secuencia). No se llama con Stop/Detach/SetAngle.
 */
void SERVO_SetCallback(Servo_t *servo, ServoCallback_t cb, void *ctx);

/**
 * @brief Inicia una secuencia de waypoints en el servo.
 * @param servo Instancia del servo.
 * @param wps Arreglo de pasos (debe permanecer válido: usar static/global).
 * @param count Cantidad de pasos.
 * @param loop 1 = repetir indefinidamente, 0 = una sola vez.
 */
void SERVO_StartSequence(Servo_t *servo, const ServoWaypoint_t *wps, uint16_t count, uint8_t loop);

/**
 * @brief Consulta si el servo se está moviendo.
 * @param servo Instancia del servo.
 * @return 1 si hay un movimiento, pausa o secuencia en curso; 0 en caso contrario.
 */
uint8_t SERVO_IsMoving(const Servo_t *servo);

/**
 * @brief Consulta si hay una secuencia activa en el servo.
 * @param servo Instancia del servo.
 * @return 1 si hay una secuencia en curso, 0 en caso contrario.
 */
uint8_t SERVO_IsSequenceActive(const Servo_t *servo);

/**
 * @brief Obtiene el ángulo actual estimado en grados.
 * @param servo Instancia del servo.
 * @return Ángulo actual en grados.
 */
float SERVO_GetAngle(const Servo_t *servo);

/* ===========================================================================
 *                          API: GRUPO DE SERVOS
 * =========================================================================*/

/**
 * @brief Inicializa un grupo con servos ya inicializados.
 * @param group Instancia de grupo.
 * @param servos Arreglo de punteros a Servo_t (debe permanecer válido).
 * @param count Cantidad de servos.
 */
void SERVO_GROUP_Init(ServoGroup_t *group, Servo_t **servos, uint8_t count);

/**
 * @brief Actualiza todos los servos del grupo, sus secuencias y el callback.
 * @param group Instancia del grupo.
 * @note Llamar en el while(1) (reemplaza a llamar Update de cada servo).
 */
void SERVO_GROUP_Update(ServoGroup_t *group);

/**
 * @brief Posiciona todos los servos al instante.
 * @param group Instancia del grupo.
 * @param angles Un ángulo por servo (o SERVO_GROUP_KEEP para no moverlo).
 */
void SERVO_GROUP_SetAngles(ServoGroup_t *group, const float *angles);

/**
 * @brief Mueve todos los servos a la vez; todos llegan juntos tras `duration_ms`.
 * @param group Instancia del grupo.
 * @param angles Un ángulo por servo (o SERVO_GROUP_KEEP).
 * @param duration_ms Duración común del movimiento.
 */
void SERVO_GROUP_MoveSync(ServoGroup_t *group, const float *angles, uint32_t duration_ms);

/**
 * @brief Movimiento sincronizado definido por velocidad.
 * @param group Instancia del grupo.
 * @param angles Un ángulo por servo (o SERVO_GROUP_KEEP).
 * @param speed_dps Velocidad del servo que más recorre; los demás se escalan
 *                  para llegar al mismo tiempo. Si es <= 0, movimiento instantáneo.
 */
void SERVO_GROUP_MoveSyncSpeed(ServoGroup_t *group, const float *angles, float speed_dps);

/**
 * @brief Inicia una secuencia de poses sincronizadas.
 * @param group Instancia del grupo.
 * @param steps Arreglo de pasos (debe permanecer válido).
 * @param count Cantidad de pasos.
 * @param loop 1 = repetir indefinidamente.
 */
void SERVO_GROUP_StartSequence(ServoGroup_t *group, const ServoGroupStep_t *steps, uint16_t count, uint8_t loop);

/**
 * @brief Congela todos los servos y cancela la secuencia del grupo.
 * @param group Instancia del grupo.
 */
void SERVO_GROUP_Stop(ServoGroup_t *group);

/**
 * @brief Activa el PWM de todos los servos del grupo.
 * @param group Instancia del grupo.
 */
void SERVO_GROUP_AttachAll(ServoGroup_t *group);

/**
 * @brief Apaga el PWM de todos los servos y cancela la secuencia del grupo.
 * @param group Instancia del grupo.
 */
void SERVO_GROUP_DetachAll(ServoGroup_t *group);

/**
 * @brief Consulta si el grupo está ejecutando algún movimiento.
 * @param group Instancia del grupo.
 * @return 1 si algún servo se mueve o hay secuencia de grupo en curso, 0 en caso contrario.
 */
uint8_t SERVO_GROUP_IsMoving(const ServoGroup_t *group);

/**
 * @brief Registra el callback de fin de grupo.
 * @param group Instancia del grupo.
 * @param cb Función a llamar.
 * @param ctx Contexto de usuario.
 * @note Se llama cuando termina un movimiento de grupo (MoveSync / MoveSyncSpeed)
 *       o la secuencia completa. Los callbacks individuales de cada servo
 *       también se siguen llamando.
 */
void SERVO_GROUP_SetCallback(ServoGroup_t *group, ServoGroupCallback_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SERVO_STM32_H */
