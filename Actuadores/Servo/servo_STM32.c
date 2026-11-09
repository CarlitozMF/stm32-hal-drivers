/**
 * @file servo_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación del driver de servos (v2.0).
 * @details Modelo de movimiento: todo movimiento se define por
 *          (ángulo inicial, ángulo destino, duración, instante de inicio).
 *          En cada Update() se calcula la posición según el tiempo
 *          transcurrido y el perfil (easing). Al terminar, la posición se
 *          fuerza exactamente al destino, sin errores acumulados.
 * @version 2.0
 * @date 2026
 */

#include <servo_STM32.h>
#include <string.h>

/* Valores menores o iguales a este umbral se interpretan como SERVO_GROUP_KEEP */
#define KEEP_THRESHOLD   (-999.0f)

/* ===========================================================================
 * FUNCIONES AUXILIARES PRIVADAS
 * =========================================================================*/

/* Valor absoluto para flotantes */
static float absf(float x)
{
    return (x < 0.0f) ? -x : x;
}

/* Recorta un flotante entre un límite inferior y superior */
static float clamp_f(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Satura un ángulo lógico a los límites configurados por software en el servo */
static float clamp_logical(const Servo_t *s, float angle)
{
    return clamp_f(angle, s->limit_min, s->limit_max);
}

/* Perfil de aceleración (easing): convierte el progreso temporal t [0,1] en avance espacial [0,1] */
static float ease(ServoEasing_t type, float t)
{
    switch (type) {
    case SERVO_EASE_IN:      return t * t;                           /* Aceleración cuadrática */
    case SERVO_EASE_OUT:     return t * (2.0f - t);                  /* Desaceleración cuadrática */
    case SERVO_EASE_IN_OUT:  return t * t * (3.0f - 2.0f * t);        /* Curva S suave (Hermite) */
    case SERVO_EASE_LINEAR:
    default:                 return t;                               /* Velocidad constante */
    }
}

/* Convierte un ángulo lógico (°), con trim e inversión, al valor del registro de comparación (CCR) */
static uint32_t angle_to_ccr(const Servo_t *s, float logical)
{
    /* Aplicar sentido de giro invertido si está habilitado */
    float p = s->inverted ? ((float)SERVO_ANGLE_MAX - logical) : logical;

    /* Sumar offset fino (trim) y saturar al rango físico del servo (0° a 180°) */
    p += s->trim;
    p = clamp_f(p, (float)SERVO_ANGLE_MIN, (float)SERVO_ANGLE_MAX);

    /* Escalamiento lineal al valor de CCR con redondeo exacto */
    float range = (float)s->max_pulse - (float)s->min_pulse;
    return (uint32_t)((float)s->min_pulse + (range / (float)SERVO_ANGLE_MAX) * p + 0.5f);
}

/* Escribe el registro CCR correspondiente en el hardware del timer */
static void write_ccr(Servo_t *s)
{
    __HAL_TIM_SET_COMPARE(s->htim, s->channel, angle_to_ccr(s, s->current_angle));
}

/* Aplica el ángulo actual al hardware solo si el PWM está habilitado */
static void apply_current_angle(Servo_t *s)
{
    if (s->attached) write_ccr(s);
}

/* Reactiva la generación PWM si el servo se encontraba en estado 'detached' */
static void ensure_attached(Servo_t *s)
{
    if (!s->attached) SERVO_Attach(s);
}

/* Detiene cualquier movimiento o secuencia activa reteniendo el ángulo actual */
static void cancel_motion(Servo_t *s)
{
    s->state       = SERVO_STATE_IDLE;
    s->seq_active  = 0;
    s->start_angle = s->current_angle;
    s->target_angle = s->current_angle;
    s->duration_ms = 0;
}

/* Configura las variables para iniciar un movimiento progresivo no bloqueante */
static void begin_move(Servo_t *s, float target, uint32_t duration_ms, uint32_t t0)
{
    ensure_attached(s);

    s->start_angle   = s->current_angle;
    s->target_angle  = target;
    s->duration_ms   = duration_ms;
    s->start_time_ms = t0;
    s->state         = SERVO_STATE_MOVING;

    /* Si la duración es 0, salta directamente y la notificación se difiere a Update() */
    if (duration_ms == 0) {
        s->current_angle = target;
        apply_current_angle(s);
    }
}

/* Lanza la función callback registrada por el usuario si está definida */
static void fire_callback(Servo_t *s)
{
    if (s->on_done != NULL) s->on_done(s, s->cb_ctx);
}

/* Carga y arranca el siguiente waypoint de la secuencia activa del servo */
static void start_waypoint(Servo_t *s)
{
    const ServoWaypoint_t *wp = &s->seq[s->seq_index];
    begin_move(s, clamp_logical(s, wp->angle), wp->duration_ms, HAL_GetTick());
}

/* Avanza al siguiente índice dentro de la secuencia de waypoints o finaliza el bucle */
static void sequence_next(Servo_t *s)
{
    s->seq_index++;
    if (s->seq_index >= s->seq_len) {
        if (s->seq_loop) {
            s->seq_index = 0;
        } else {
            s->seq_active = 0;
            s->state = SERVO_STATE_IDLE;
            fire_callback(s);
            return;
        }
    }
    start_waypoint(s);
}

/* Evento ejecutado al alcanzar la duración objetivo de un movimiento */
static void on_move_finished(Servo_t *s, uint32_t now)
{
    s->state = SERVO_STATE_IDLE;

    if (s->seq_active) {
        if (s->seq[s->seq_index].hold_ms > 0) {
            s->state = SERVO_STATE_HOLD;
            s->hold_start_ms = now;
        } else {
            sequence_next(s);
        }
    } else {
        fire_callback(s);
    }
}

/* ===========================================================================
 * IMPLEMENTACIÓN DE LA API PÚBLICA: SERVO INDIVIDUAL
 * =========================================================================*/

/* Inicializa el servo forzando una posición inicial específica para evitar brincos bruscos */
void SERVO_InitAt(Servo_t *servo, TIM_HandleTypeDef *htim, uint32_t channel,
                  uint32_t min, uint32_t max, float initial_angle)
{
    if (servo == NULL || htim == NULL) return;

    memset(servo, 0, sizeof(Servo_t));

    servo->htim      = htim;
    servo->channel   = channel;
    servo->min_pulse = min;
    servo->max_pulse = max;

    servo->limit_min = (float)SERVO_ANGLE_MIN;
    servo->limit_max = (float)SERVO_ANGLE_MAX;
    servo->trim      = 0.0f;
    servo->inverted  = 0;
    servo->easing    = SERVO_EASE_LINEAR;
    servo->state     = SERVO_STATE_IDLE;

    initial_angle = clamp_f(initial_angle, servo->limit_min, servo->limit_max);
    servo->current_angle = initial_angle;
    servo->start_angle   = initial_angle;
    servo->target_angle  = initial_angle;

    /* SERVO_Attach escribe el registro CCR inicial y arranca la salida PWM */
    SERVO_Attach(servo);
}

/* Inicializa el servo con valor inicial predeterminado de 0° */
void SERVO_Init(Servo_t *servo, TIM_HandleTypeDef *htim, uint32_t channel,
                uint32_t min, uint32_t max)
{
    SERVO_InitAt(servo, htim, channel, min, max, 0.0f);
}

/* Asigna un ángulo en formato flotante e invalida cualquier movimiento progresivo previo */
void SERVO_SetAngleF(Servo_t *servo, float angle)
{
    if (servo == NULL) return;

    angle = clamp_logical(servo, angle);

    cancel_motion(servo);
    servo->current_angle = angle;
    servo->start_angle   = angle;
    servo->target_angle  = angle;

    ensure_attached(servo);
    apply_current_angle(servo);
}

/* Asigna un ángulo entero */
void SERVO_SetAngle(Servo_t *servo, int angle)
{
    SERVO_SetAngleF(servo, (float)angle);
}

/* Inicia un movimiento programado por tiempo absoluto en milisegundos */
void SERVO_MoveTime(Servo_t *servo, float angle, uint32_t duration_ms)
{
    if (servo == NULL) return;

    servo->seq_active = 0;
    begin_move(servo, clamp_logical(servo, angle), duration_ms, HAL_GetTick());
}

/* Inicia un movimiento determinando el tiempo a partir de una velocidad deseada (°/s) */
void SERVO_SetSpeedAngleF(Servo_t *servo, float target_angle, float speed_dps)
{
    if (servo == NULL) return;

    float target = clamp_logical(servo, target_angle);

    if (speed_dps <= 0.0f) {
        SERVO_SetAngleF(servo, target);
        return;
    }

    float delta = absf(target - servo->current_angle);
    uint32_t duration = (uint32_t)(delta * 1000.0f / speed_dps + 0.5f);

    servo->seq_active = 0;
    begin_move(servo, target, duration, HAL_GetTick());
}

/* Versión entera de movimiento por velocidad */
void SERVO_SetSpeedAngle(Servo_t *servo, int target_angle, float speed_dps)
{
    SERVO_SetSpeedAngleF(servo, (float)target_angle, speed_dps);
}

/* Bucle de actualización no bloqueante: interpola la posición y gestiona temporizaciones */
void SERVO_Update(Servo_t *servo)
{
    if (servo == NULL) return;

    if (servo->state == SERVO_STATE_MOVING) {
        uint32_t now     = HAL_GetTick();
        uint32_t elapsed = now - servo->start_time_ms;   /* Cálculo seguro contra desbordamiento de SysTick */

        if (elapsed >= servo->duration_ms) {
            /* Duración alcanzada: fijar posición destino exacta */
            servo->current_angle = servo->target_angle;
            apply_current_angle(servo);
            on_move_finished(servo, now);
        } else {
            /* Interpolación de posición con el perfil de velocidad seleccionado */
            float t = (float)elapsed / (float)servo->duration_ms;
            float progress = ease(servo->easing, t);
            servo->current_angle = servo->start_angle +
                                   (servo->target_angle - servo->start_angle) * progress;
            apply_current_angle(servo);
        }
    }
    else if (servo->state == SERVO_STATE_HOLD) {
        uint32_t now = HAL_GetTick();
        if ((now - servo->hold_start_ms) >= servo->seq[servo->seq_index].hold_ms) {
            sequence_next(servo);
        }
    }
}

/* Congela la posición del servo en el instante actual */
void SERVO_Stop(Servo_t *servo)
{
    if (servo == NULL) return;
    cancel_motion(servo);
}

/* Habilita la señal PWM del canal */
void SERVO_Attach(Servo_t *servo)
{
    if (servo == NULL || servo->attached) return;

    write_ccr(servo);                                   /* Escribir CCR previo para evitar glitches */
    HAL_TIM_PWM_Start(servo->htim, servo->channel);
    servo->attached = 1;
}

/* Detiene la generación PWM liberando el par del servomotor */
void SERVO_Detach(Servo_t *servo)
{
    if (servo == NULL) return;

    cancel_motion(servo);

    if (servo->attached) {
        HAL_TIM_PWM_Stop(servo->htim, servo->channel);
        servo->attached = 0;
    }
}

/* Retorna el estado de habilitación del PWM */
uint8_t SERVO_IsAttached(const Servo_t *servo)
{
    return (servo != NULL) ? servo->attached : 0;
}

/* Asigna directamente un pulso en microsegundos (útil para etapas de prueba o calibración) */
void SERVO_SetPulseUs(Servo_t *servo, uint32_t pulse_us)
{
    if (servo == NULL) return;

    cancel_motion(servo);
    ensure_attached(servo);

    uint32_t ccr = pulse_us * SERVO_TICKS_PER_US;
    __HAL_TIM_SET_COMPARE(servo->htim, servo->channel, ccr);

    /* Recalcular estimación del ángulo equivalente para preservar consistencia interna */
    float range = (float)servo->max_pulse - (float)servo->min_pulse;
    if (range != 0.0f) {
        float p = ((float)ccr - (float)servo->min_pulse) * (float)SERVO_ANGLE_MAX / range;
        p -= servo->trim;
        if (servo->inverted) p = (float)SERVO_ANGLE_MAX - p;
        p = clamp_f(p, (float)SERVO_ANGLE_MIN, (float)SERVO_ANGLE_MAX);

        servo->current_angle = p;
        servo->start_angle   = p;
        servo->target_angle  = p;
    }
}

/* Configura límites de software para restringir la zona de movimiento libre */
void SERVO_SetLimits(Servo_t *servo, float min_angle, float max_angle)
{
    if (servo == NULL) return;

    min_angle = clamp_f(min_angle, (float)SERVO_ANGLE_MIN, (float)SERVO_ANGLE_MAX);
    max_angle = clamp_f(max_angle, (float)SERVO_ANGLE_MIN, (float)SERVO_ANGLE_MAX);

    if (min_angle > max_angle) {
        float tmp = min_angle;
        min_angle = max_angle;
        max_angle = tmp;
    }
    servo->limit_min = min_angle;
    servo->limit_max = max_angle;
}

/* Asigna un offset fino sobre la posición cero */
void SERVO_SetTrim(Servo_t *servo, float trim_deg)
{
    if (servo == NULL) return;
    servo->trim = trim_deg;
    apply_current_angle(servo);
}

/* Configura la inversión lógica del sentido de giro */
void SERVO_SetInverted(Servo_t *servo, uint8_t inverted)
{
    if (servo == NULL) return;
    servo->inverted = inverted ? 1 : 0;
    apply_current_angle(servo);
}

/* Establece el perfil de aceleración/desaceleración para los movimientos posteriores */
void SERVO_SetEasing(Servo_t *servo, ServoEasing_t easing)
{
    if (servo == NULL) return;
    servo->easing = easing;
}

/* Registra la función callback para eventos de fin de trayectoria */
void SERVO_SetCallback(Servo_t *servo, ServoCallback_t cb, void *ctx)
{
    if (servo == NULL) return;
    servo->on_done = cb;
    servo->cb_ctx  = ctx;
}

/* Arranca una secuencia de waypoints en un servo individual */
void SERVO_StartSequence(Servo_t *servo, const ServoWaypoint_t *wps,
                        uint16_t count, uint8_t loop)
{
    if (servo == NULL || wps == NULL || count == 0) return;

    servo->seq        = wps;
    servo->seq_len    = count;
    servo->seq_index  = 0;
    servo->seq_loop   = loop ? 1 : 0;
    servo->seq_active = 1;

    start_waypoint(servo);
}

/* Consulta si el servo está en medio de una trayectoria activa */
uint8_t SERVO_IsMoving(const Servo_t *servo)
{
    if (servo == NULL) return 0;
    return (servo->state != SERVO_STATE_IDLE) ? 1 : 0;
}

/* Consulta si hay una secuencia en ejecución */
uint8_t SERVO_IsSequenceActive(const Servo_t *servo)
{
    if (servo == NULL) return 0;
    return servo->seq_active;
}

/* Devuelve el ángulo lógico actual */
float SERVO_GetAngle(const Servo_t *servo)
{
    if (servo == NULL) return 0.0f;
    return servo->current_angle;
}

/* ===========================================================================
 * IMPLEMENTACIÓN DE LA API PÚBLICA: GRUPO DE SERVOS
 * =========================================================================*/

/* Aplica un comando de movimiento síncrono asignando una misma marca de tiempo inicial a todos los miembros */
static void group_move_sync(ServoGroup_t *g, const float *angles, uint32_t duration_ms)
{
    uint32_t t0 = HAL_GetTick();   /* Marca de tiempo unificada para garantizar sincronismo absoluto */

    for (uint8_t i = 0; i < g->count; i++) {
        Servo_t *s = g->servos[i];
        if (s == NULL || angles[i] < KEEP_THRESHOLD) continue;

        s->seq_active = 0;
        begin_move(s, clamp_logical(s, angles[i]), duration_ms, t0);
    }
}

/* Evalúa si al menos uno de los servos del grupo está ejecutando un movimiento */
static uint8_t group_any_servo_moving(const ServoGroup_t *g)
{
    for (uint8_t i = 0; i < g->count; i++) {
        if (SERVO_IsMoving(g->servos[i])) return 1;
    }
    return 0;
}

/* Lanza el callback de notificación para eventos del grupo */
static void group_fire_callback(ServoGroup_t *g)
{
    if (g->on_done != NULL) g->on_done(g, g->cb_ctx);
}

/* Carga la siguiente pose de la secuencia de grupo */
static void group_start_step(ServoGroup_t *g)
{
    const ServoGroupStep_t *st = &g->seq[g->seq_index];
    group_move_sync(g, st->angles, st->duration_ms);
    g->seq_state = SERVO_STATE_MOVING;
}

/* Transita al siguiente paso dentro de la secuencia de grupo */
static void group_sequence_next(ServoGroup_t *g)
{
    g->seq_index++;
    if (g->seq_index >= g->seq_len) {
        if (g->seq_loop) {
            g->seq_index = 0;
        } else {
            g->seq_state = SERVO_STATE_IDLE;
            g->active = 0;
            group_fire_callback(g);
            return;
        }
    }
    group_start_step(g);
}

/* Inicializa la estructura del grupo con un arreglo de punteros a instancias Servo_t */
void SERVO_GROUP_Init(ServoGroup_t *group, Servo_t **servos, uint8_t count)
{
    if (group == NULL) return;

    memset(group, 0, sizeof(ServoGroup_t));
    group->servos = servos;
    group->count  = (servos != NULL) ? count : 0;
}

/* Motor de actualización periódica para el grupo: refresca servos y gestiona secuencias síncronas */
void SERVO_GROUP_Update(ServoGroup_t *group)
{
    if (group == NULL) return;

    for (uint8_t i = 0; i < group->count; i++) {
        SERVO_Update(group->servos[i]);
    }

    uint8_t any_moving = group_any_servo_moving(group);

    switch (group->seq_state) {
    case SERVO_STATE_MOVING:
        if (!any_moving) {
            if (group->seq[group->seq_index].hold_ms > 0) {
                group->seq_state = SERVO_STATE_HOLD;
                group->hold_start_ms = HAL_GetTick();
            } else {
                group_sequence_next(group);
            }
        }
        break;

    case SERVO_STATE_HOLD:
        if ((HAL_GetTick() - group->hold_start_ms) >= group->seq[group->seq_index].hold_ms) {
            group_sequence_next(group);
        }
        break;

    case SERVO_STATE_IDLE:
    default:
        if (group->active && !any_moving) {
            group->active = 0;
            group_fire_callback(group);
        }
        break;
    }
}

/* Posiciona de manera instantánea todos los miembros del grupo */
void SERVO_GROUP_SetAngles(ServoGroup_t *group, const float *angles)
{
    if (group == NULL || angles == NULL) return;

    group->seq_state = SERVO_STATE_IDLE;
    group->active = 0;

    for (uint8_t i = 0; i < group->count; i++) {
        if (group->servos[i] == NULL || angles[i] < KEEP_THRESHOLD) continue;
        SERVO_SetAngleF(group->servos[i], angles[i]);
    }
}

/* Coordina el movimiento del grupo garantizando que todos los servos lleguen al unísono tras 'duration_ms' */
void SERVO_GROUP_MoveSync(ServoGroup_t *group, const float *angles, uint32_t duration_ms)
{
    if (group == NULL || angles == NULL) return;

    group->seq_state = SERVO_STATE_IDLE;
    group_move_sync(group, angles, duration_ms);
    group->active = 1;
}

/* Calcula dinámicamente el tiempo necesario a partir del servo que recorre mayor distancia angular */
void SERVO_GROUP_MoveSyncSpeed(ServoGroup_t *group, const float *angles, float speed_dps)
{
    if (group == NULL || angles == NULL) return;

    if (speed_dps <= 0.0f) {
        SERVO_GROUP_SetAngles(group, angles);
        return;
    }

    float max_delta = 0.0f;
    for (uint8_t i = 0; i < group->count; i++) {
        Servo_t *s = group->servos[i];
        if (s == NULL || angles[i] < KEEP_THRESHOLD) continue;

        float d = absf(clamp_logical(s, angles[i]) - s->current_angle);
        if (d > max_delta) max_delta = d;
    }

    uint32_t duration = (uint32_t)(max_delta * 1000.0f / speed_dps + 0.5f);
    SERVO_GROUP_MoveSync(group, angles, duration);
}

/* Arranca la ejecución de una secuencia de poses síncronas */
void SERVO_GROUP_StartSequence(ServoGroup_t *group, const ServoGroupStep_t *steps,
                                uint16_t count, uint8_t loop)
{
    if (group == NULL || steps == NULL || count == 0) return;

    group->seq       = steps;
    group->seq_len   = count;
    group->seq_index = 0;
    group->seq_loop  = loop ? 1 : 0;
    group->active    = 0;

    group_start_step(group);
}

/* Detiene de forma inmediata la trayectoria de todos los servos pertenecientes al grupo */
void SERVO_GROUP_Stop(ServoGroup_t *group)
{
    if (group == NULL) return;

    group->seq_state = SERVO_STATE_IDLE;
    group->active = 0;

    for (uint8_t i = 0; i < group->count; i++) {
        SERVO_Stop(group->servos[i]);
    }
}

/* Habilita las salidas PWM de todos los miembros del grupo */
void SERVO_GROUP_AttachAll(ServoGroup_t *group)
{
    if (group == NULL) return;
    for (uint8_t i = 0; i < group->count; i++) {
        SERVO_Attach(group->servos[i]);
    }
}

/* Deshabilita la generación PWM para liberar del par a todos los servos del grupo */
void SERVO_GROUP_DetachAll(ServoGroup_t *group)
{
    if (group == NULL) return;

    group->seq_state = SERVO_STATE_IDLE;
    group->active = 0;

    for (uint8_t i = 0; i < group->count; i++) {
        SERVO_Detach(group->servos[i]);
    }
}

/* Verifica si existe movimiento en curso dentro del grupo */
uint8_t SERVO_GROUP_IsMoving(const ServoGroup_t *group)
{
    if (group == NULL) return 0;
    if (group->seq_state != SERVO_STATE_IDLE) return 1;
    return group_any_servo_moving(group);
}

/* Registra la función callback de grupo */
void SERVO_GROUP_SetCallback(ServoGroup_t *group, ServoGroupCallback_t cb, void *ctx)
{
    if (group == NULL) return;
    group->on_done = cb;
    group->cb_ctx  = ctx;
}
