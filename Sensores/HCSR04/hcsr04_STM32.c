/**
 ******************************************************************************
 * @file    hcsr04_STM32.c
 * @brief   Implementación del driver no bloqueante para N sensores HC-SR04.
 ******************************************************************************
 * @note    La función HAL_TIM_IC_CaptureCallback() NO está en este archivo:
 *          la aplicación (main.c) la implementa y enruta cada canal/timer al
 *          handle del sensor correspondiente mediante
 *          HCSR04_InputCapture_Callback(). Esto permite usar N sensores.
 *
 * @note    Funcionamiento general:
 *          1. HCSR04_Trigger()  -> arma la captura (flanco de subida) y emite
 *                                  el pulso TRIG de 10 us.
 *          2. ISR (flanco subida) -> guarda el contador y cambia a flanco de bajada.
 *          3. ISR (flanco bajada) -> guarda el contador, calcula la duración y
 *                                    detiene la captura.
 *          4. HCSR04_ReadDistance_cm() -> convierte la duración a centímetros.
 ******************************************************************************
 */

#include "hcsr04_STM32.h"

/* Macros / constantes privadas ----------------------------------------------*/

/**
 * Límite de iteraciones del retardo por timer. Es sólo una protección para no
 * quedar bloqueado si el contador del timer estuviera detenido por error.
 */
#define HCSR04_DELAY_GUARD_LOOPS    50000UL

/* ===========================================================================
 * FUNCIONES PRIVADAS
 * ========================================================================= */

/**
 * @brief  Devuelve el flag de interrupción de captura (TIM_IT_CCx) del canal.
 * @param  channel  TIM_CHANNEL_1..4.
 * @return Flag TIM_IT_CC1..CC4 correspondiente.
 */
static uint32_t HCSR04_GetITFlag(uint32_t channel)
{
    switch (channel) {
        case TIM_CHANNEL_1: return TIM_IT_CC1;
        case TIM_CHANNEL_2: return TIM_IT_CC2;
        case TIM_CHANNEL_3: return TIM_IT_CC3;
        default:            return TIM_IT_CC4;
    }
}

/**
 * @brief  Calcula los ticks transcurridos entre dos lecturas del contador,
 *         contemplando un desbordamiento (wrap) del timer.
 * @param  start  Lectura inicial del contador.
 * @param  end    Lectura final del contador.
 * @param  arr    Valor de auto-reload (ARR) del timer.
 * @return Ticks transcurridos.
 * @details El período del contador es ARR + 1. Se usa aritmética de 64 bits
 *          para que funcione también con timers de 32 bits (ARR = 0xFFFFFFFF),
 *          donde ARR + 1 desbordaría un uint32_t.
 */
static uint32_t HCSR04_ElapsedTicks(uint32_t start, uint32_t end, uint32_t arr)
{
    if (end >= start) {
        return end - start;
    }
    /* El contador dio la vuelta: (período - start) + end */
    return (uint32_t)((((uint64_t)arr + 1U) - start) + end);
}

/**
 * @brief  Espera activa de una cantidad de ticks usando el propio timer.
 * @param  htim   Handle del timer (debe estar contando).
 * @param  ticks  Ticks a esperar.
 * @details Reemplaza al uso de DWT: DWT no existe en Cortex-M0/M0+, por lo que
 *          depender de él impediría portar el driver a F0/G0/L0/C0.
 */
static void HCSR04_WaitTicks(TIM_HandleTypeDef* htim, uint32_t ticks)
{
    const uint32_t arr   = __HAL_TIM_GET_AUTORELOAD(htim);
    const uint32_t start = __HAL_TIM_GET_COUNTER(htim);
    uint32_t guard = HCSR04_DELAY_GUARD_LOOPS;

    while ((HCSR04_ElapsedTicks(start, __HAL_TIM_GET_COUNTER(htim), arr) < ticks)
           && (guard-- > 0U)) {
        /* Espera activa */
    }
}

/**
 * @brief  Agrega una muestra válida al historial circular del filtro.
 * @param  hcsr    Handle del sensor.
 * @param  sample  Distancia válida en cm.
 */
static void HCSR04_FilterPush(HCSR04_Handle* hcsr, float sample)
{
    hcsr->filter_buf[hcsr->filter_idx] = sample;
    hcsr->filter_idx = (uint8_t)((hcsr->filter_idx + 1U) % HCSR04_FILTER_SIZE);

    if (hcsr->filter_count < HCSR04_FILTER_SIZE) {
        hcsr->filter_count++;
    }
}

/**
 * @brief  Calcula la mediana de las muestras almacenadas.
 * @param  hcsr  Handle del sensor.
 * @return Mediana en cm.
 * @details Copia el historial a un arreglo temporal y lo ordena por inserción
 *          (eficiente para N <= 9). El orden circular del historial no importa
 *          para la mediana, y las posiciones válidas son siempre 0..count-1.
 */
static float HCSR04_FilterMedian(const HCSR04_Handle* hcsr)
{
    float   tmp[HCSR04_FILTER_SIZE];
    uint8_t n = hcsr->filter_count;
    uint8_t i;
    uint8_t j;

    for (i = 0U; i < n; i++) {
        tmp[i] = hcsr->filter_buf[i];
    }

    /* Ordenamiento por inserción */
    for (i = 1U; i < n; i++) {
        float key = tmp[i];
        j = i;
        while ((j > 0U) && (tmp[j - 1U] > key)) {
            tmp[j] = tmp[j - 1U];
            j--;
        }
        tmp[j] = key;
    }

    return tmp[n / 2U];
}

/**
 * @brief  Registra una lectura inválida (sin eco o fuera de rango).
 * @param  hcsr  Handle del sensor.
 * @return HCSR04_ERR_TIMEOUT.
 * @details La muestra no entra al historial. Si se acumulan FILTER_SIZE
 *          inválidas consecutivas, el historial se considera obsoleto y se
 *          borra, para no mezclar distancias de antes de que el objeto se fuera.
 */
static float HCSR04_RejectSample(HCSR04_Handle* hcsr)
{
    hcsr->invalid_count++;
    if (hcsr->invalid_count >= HCSR04_FILTER_SIZE) {
        HCSR04_ClearFilter(hcsr);
    }
    return HCSR04_ERR_TIMEOUT;
}

/* ===========================================================================
 * 1. INICIALIZACIÓN Y CONFIGURACIÓN
 * ========================================================================= */

void HCSR04_Init(HCSR04_Handle* hcsr, TIM_HandleTypeDef* htim, uint32_t channel,
                 GPIO_TypeDef* trig_port, uint16_t trig_pin)
{
    /* Guarda la configuración de hardware en el handle */
    hcsr->htim      = htim;
    hcsr->channel   = channel;
    hcsr->trig_port = trig_port;
    hcsr->trig_pin  = trig_pin;
    hcsr->tick_hz   = HCSR04_DEFAULT_TICK_HZ;

    /* Estado inicial: sin medición en curso */
    hcsr->edge_state     = HCSR04_STATE_IDLE;
    hcsr->rising_time    = 0U;
    hcsr->falling_time   = 0U;
    hcsr->pulse_duration = 0U;
    hcsr->trigger_tick   = 0U;

    /* Historial del filtro vacío */
    HCSR04_ClearFilter(hcsr);

    /* El pin TRIG debe reposar en nivel bajo */
    HAL_GPIO_WritePin(hcsr->trig_port, hcsr->trig_pin, GPIO_PIN_RESET);
}

void HCSR04_SetTickFrequency(HCSR04_Handle* hcsr, uint32_t tick_hz)
{
    if (tick_hz != 0U) {
        hcsr->tick_hz = tick_hz;
    }
}

void HCSR04_ClearFilter(HCSR04_Handle* hcsr)
{
    hcsr->filter_idx    = 0U;
    hcsr->filter_count  = 0U;
    hcsr->invalid_count = 0U;
}

/* ===========================================================================
 * 2. DISPARO (TRIGGER)
 * ========================================================================= */

void HCSR04_Trigger(HCSR04_Handle* hcsr)
{
    /* 1. Cancela cualquier captura previa. Es necesario porque si una medición
     *    anterior quedó sin eco, el canal sigue en estado BUSY y
     *    HAL_TIM_IC_Start_IT() fallaría silenciosamente. Si el canal no estaba
     *    activo, esta llamada no tiene efecto. */
    HAL_TIM_IC_Stop_IT(hcsr->htim, hcsr->channel);

    /* 2. Prepara la máquina de estados para el nuevo ciclo */
    hcsr->edge_state   = HCSR04_STATE_WAIT_RISING;
    hcsr->trigger_tick = HAL_GetTick();

    /* 3. Limpia una posible interrupción pendiente y fuerza flanco de subida */
    __HAL_TIM_CLEAR_IT(hcsr->htim, HCSR04_GetITFlag(hcsr->channel));
    __HAL_TIM_SET_CAPTUREPOLARITY(hcsr->htim, hcsr->channel,
                                  TIM_INPUTCHANNELPOLARITY_RISING);

    /* 4. Activa la captura por interrupción. Esto también arranca el contador.
     *    NO se reinicia el contador a 0: otro sensor podría estar midiendo en
     *    el mismo timer y se corrompería su resultado. Como las duraciones se
     *    calculan por diferencia, el contador puede correr libremente. */
    HAL_TIM_IC_Start_IT(hcsr->htim, hcsr->channel);

    /* 5. Pulso TRIG de 10 us temporizado con el propio timer.
     *    Ticks = ceil(10us * tick_hz / 1e6) + 1; el +1 compensa que la primera
     *    lectura puede caer a mitad de un tick, garantizando >= 10 us. */
    uint32_t ticks = (uint32_t)((((uint64_t)HCSR04_TRIG_PULSE_US * hcsr->tick_hz)
                                 + 999999ULL) / 1000000ULL) + 1U;

    HAL_GPIO_WritePin(hcsr->trig_port, hcsr->trig_pin, GPIO_PIN_SET);
    HCSR04_WaitTicks(hcsr->htim, ticks);
    HAL_GPIO_WritePin(hcsr->trig_port, hcsr->trig_pin, GPIO_PIN_RESET);
}

/* ===========================================================================
 * 3. LÓGICA DE CAPTURA (llamada desde HAL_TIM_IC_CaptureCallback en main.c)
 * ========================================================================= */

void HCSR04_InputCapture_Callback(HCSR04_Handle* hcsr)
{
    switch (hcsr->edge_state) {

    case HCSR04_STATE_WAIT_RISING:
        /* Flanco de subida: comienza el pulso ECHO */
        hcsr->rising_time = HAL_TIM_ReadCapturedValue(hcsr->htim, hcsr->channel);

        /* Cambia la polaridad para capturar el flanco de bajada */
        __HAL_TIM_SET_CAPTUREPOLARITY(hcsr->htim, hcsr->channel,
                                      TIM_INPUTCHANNELPOLARITY_FALLING);
        hcsr->edge_state = HCSR04_STATE_WAIT_FALLING;
        break;

    case HCSR04_STATE_WAIT_FALLING:
        /* Flanco de bajada: termina el pulso ECHO */
        hcsr->falling_time = HAL_TIM_ReadCapturedValue(hcsr->htim, hcsr->channel);

        /* Duración en ticks, con manejo de desbordamiento del contador */
        hcsr->pulse_duration = HCSR04_ElapsedTicks(hcsr->rising_time,
                                                   hcsr->falling_time,
                                                   __HAL_TIM_GET_AUTORELOAD(hcsr->htim));

        /* Detiene la captura de este canal (no afecta a los demás canales) */
        HAL_TIM_IC_Stop_IT(hcsr->htim, hcsr->channel);

        /* Deja la polaridad lista para el próximo ciclo */
        __HAL_TIM_SET_CAPTUREPOLARITY(hcsr->htim, hcsr->channel,
                                      TIM_INPUTCHANNELPOLARITY_RISING);

        /* Medición lista para ser leída desde el lazo principal */
        hcsr->edge_state = HCSR04_STATE_READY;
        break;

    default:
        /* Evento espurio fuera de una medición: se ignora */
        break;
    }
}

/* ===========================================================================
 * 4. LECTURA DE DISTANCIA
 * ========================================================================= */

float HCSR04_ReadDistance_cm(HCSR04_Handle* hcsr)
{
    /* Sin medición en curso ni terminada */
    if (hcsr->edge_state == HCSR04_STATE_IDLE) {
        return HCSR04_ERR_NOT_READY;
    }

    /* Medición en curso: verifica si venció el tiempo de espera del eco */
    if (hcsr->edge_state != HCSR04_STATE_READY) {
        if ((HAL_GetTick() - hcsr->trigger_tick) < HCSR04_TIMEOUT_MS) {
            return HCSR04_ERR_NOT_READY;
        }

        /* Timeout: detiene la captura (esto también deshabilita su interrupción) */
        HAL_TIM_IC_Stop_IT(hcsr->htim, hcsr->channel);

        /* Si la ISR alcanzó a completar la medición justo antes del Stop,
         * se aprovecha el resultado; si no, se declara timeout. */
        if (hcsr->edge_state != HCSR04_STATE_READY) {
            __HAL_TIM_SET_CAPTUREPOLARITY(hcsr->htim, hcsr->channel,
                                          TIM_INPUTCHANNELPOLARITY_RISING);
            hcsr->edge_state = HCSR04_STATE_IDLE;
            return HCSR04_RejectSample(hcsr);
        }
    }

    /* Conversión: t = ticks / f_tick ; d = (t * v_sonido) / 2 (ida y vuelta) */
    float time_s   = (float)hcsr->pulse_duration / (float)hcsr->tick_hz;
    float distance = (time_s * HCSR04_SOUND_SPEED_CM_S) / 2.0f;

    /* Consume la medición: el sensor queda libre para el próximo Trigger */
    hcsr->edge_state = HCSR04_STATE_IDLE;

    /* Validación de rango: fuera de [MIN, MAX] se descarta la muestra */
    if ((distance < HCSR04_MIN_CM) || (distance > HCSR04_MAX_CM)) {
        return HCSR04_RejectSample(hcsr);
    }

    /* Muestra válida: entra al historial del filtro */
    hcsr->invalid_count = 0U;
    HCSR04_FilterPush(hcsr, distance);

    /* Mientras el historial no esté lleno se devuelve la lectura directa;
     * una vez lleno, se devuelve la mediana de las últimas N muestras. */
    if (hcsr->filter_count < HCSR04_FILTER_SIZE) {
        return distance;
    }

    return HCSR04_FilterMedian(hcsr);
}
