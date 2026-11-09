/*
 * lm35_STM32.c
 * Implementación del driver no bloqueante para el LM35 con filtro de media móvil.
 * Autor: Mamani Flores Carlos (UTN FRT) - v1.2 - 2026
 * La documentación de la API está en lm35_STM32.h
 */

/* Includes ------------------------------------------------------------------*/
#include "lm35_STM32.h"

/* Constant definitions ------------------------------------------------------*/

/* Sensibilidad del LM35: 10 mV por cada °C */
#define LM35_SENSITIVITY_MV_PER_C   10.0f

/* Resolución máxima soportada (evita desplazamientos indefinidos en 1 << bits) */
#define LM35_MAX_RESOLUTION_BITS    16U

/* La máscara del índice circular solo funciona si el tamaño es potencia de 2 */
_Static_assert((LM35_FILTER_SIZE & (LM35_FILTER_SIZE - 1U)) == 0U,
               "LM35_FILTER_SIZE debe ser potencia de 2");

/* Public Functions Implementation -------------------------------------------*/

/*
 * LM35_STM32_Init
 * Inicializa el handler del sensor: guarda la configuración del ADC, precalcula
 * el valor máximo según la resolución y deja el filtro de media móvil en cero.
 *
 * Parámetros:
 *   hlm35           - Handler del sensor a inicializar.
 *   hadc            - Handler HAL del ADC (ya configurado por CubeMX).
 *   channel         - Canal ADC del LM35 (ADC_CHANNEL_x).
 *   sampling_time   - Tiempo de muestreo (ADC_SAMPLETIME_x).
 *   vref_mv         - Tensión de referencia del ADC en mV.
 *   resolution_bits - Resolución del ADC en bits (1 a 16).
 *
 * Retorna:
 *   LM35_STATUS_OK en caso de éxito, LM35_STATUS_PARAM_ERROR si algún
 *   parámetro es nulo o está fuera de rango.
 */
LM35_Status_t LM35_STM32_Init(LM35_STM32_t *hlm35, ADC_HandleTypeDef *hadc,
                              uint32_t channel, uint32_t sampling_time,
                              float vref_mv, uint32_t resolution_bits)
{
    /* Validar punteros y rangos antes de tocar la estructura */
    if ((hlm35 == NULL) || (hadc == NULL) || (vref_mv <= 0.0f) ||
        (resolution_bits == 0U) || (resolution_bits > LM35_MAX_RESOLUTION_BITS)) {
        return LM35_STATUS_PARAM_ERROR;
    }

    /* Guardar la configuración del hardware */
    hlm35->hadc          = hadc;
    hlm35->adc_channel   = channel;
    hlm35->sampling_time = sampling_time;
    hlm35->vref_mv       = vref_mv;

    /* Precalcular el valor máximo del ADC para no repetir el desplazamiento en cada conversión */
    hlm35->adc_max_value = (1UL << resolution_bits) - 1UL;

    /* Poner en cero las variables de medición */
    hlm35->raw_value     = 0U;
    hlm35->avg_raw       = 0U;

    /* Reiniciar el estado del filtro de media móvil */
    hlm35->filter_sum    = 0U;
    hlm35->buffer_index  = 0U;
    hlm35->samples_count = 0U;

    /* Limpiar la ventana para que no queden valores basura */
    for (uint8_t i = 0U; i < LM35_FILTER_SIZE; i++) {
        hlm35->filter_buffer[i] = 0U;
    }

    hlm35->state = LM35_STATE_IDLE;
    return LM35_STATUS_OK;
}

/*
 * LM35_STM32_ConvertRawToTemp
 * Convierte un valor RAW del ADC a grados Celsius:
 *   T[°C] = (raw * Vref[mV] / ADC_MAX) / 10 mV/°C
 *
 * Parámetros:
 *   hlm35   - Handler del sensor (aporta Vref y resolución).
 *   raw_val - Valor RAW del ADC a convertir.
 *
 * Retorna:
 *   Temperatura en °C, o 0.0f si el handler es nulo o no fue inicializado.
 */
float LM35_STM32_ConvertRawToTemp(const LM35_STM32_t *hlm35, uint32_t raw_val)
{
    /* Evitar punteros nulos y división por cero si no se inicializó el driver */
    if ((hlm35 == NULL) || (hlm35->adc_max_value == 0U)) {
        return 0.0f;
    }

    /* RAW -> tensión en mV según la referencia y la resolución */
    float voltage_mv = ((float)raw_val * hlm35->vref_mv) / (float)hlm35->adc_max_value;

    /* Tensión -> °C usando la sensibilidad del LM35 */
    return voltage_mv / LM35_SENSITIVITY_MV_PER_C;
}

/*
 * LM35_STM32_StartConversion
 * Inicia una conversión del ADC por interrupción (no bloqueante). Marca el
 * driver como BUSY de forma atómica, configura el canal y arranca el ADC.
 * El resultado se procesa luego en LM35_STM32_ADC_Callback().
 *
 * Parámetros:
 *   hlm35 - Handler del sensor.
 *
 * Retorna:
 *   LM35_STATUS_OK            - Conversión iniciada.
 *   LM35_STATUS_BUSY          - Ya hay una conversión en curso.
 *   LM35_STATUS_PARAM_ERROR   - Handler o ADC nulo.
 *   LM35_STATUS_ERROR         - Falló la configuración o el arranque del ADC.
 */
LM35_Status_t LM35_STM32_StartConversion(LM35_STM32_t *hlm35)
{
    if ((hlm35 == NULL) || (hlm35->hadc == NULL)) {
        return LM35_STATUS_PARAM_ERROR;
    }

    /* Sección crítica: el chequeo de BUSY y su asignación deben ser atómicos,
       si no, dos contextos podrían iniciar la conversión a la vez */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (hlm35->state == LM35_STATE_BUSY) {
        __set_PRIMASK(primask);     /* Restaurar el estado previo de interrupciones */
        return LM35_STATUS_BUSY;
    }
    hlm35->state = LM35_STATE_BUSY;
    __set_PRIMASK(primask);

    /* Seleccionar el canal del LM35 con el tiempo de muestreo configurado */
    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel      = hlm35->adc_channel;
    sConfig.Rank         = 1;
    sConfig.SamplingTime = hlm35->sampling_time;

    if (HAL_ADC_ConfigChannel(hlm35->hadc, &sConfig) != HAL_OK) {
        hlm35->state = LM35_STATE_IDLE;     /* Liberar el estado para poder reintentar */
        return LM35_STATUS_ERROR;
    }

    /* Arrancar la conversión; el resultado llega por interrupción */
    if (HAL_ADC_Start_IT(hlm35->hadc) != HAL_OK) {
        hlm35->state = LM35_STATE_IDLE;     /* Liberar el estado para poder reintentar */
        return LM35_STATUS_ERROR;
    }

    return LM35_STATUS_OK;
}

/*
 * LM35_STM32_ADC_Callback
 * Procesa la muestra entregada por el ADC. Lee el valor, actualiza el buffer
 * circular y la suma acumulada (media móvil O(1)) y publica el promedio RAW.
 * Se ejecuta en contexto de interrupción, por eso solo usa aritmética entera.
 * Debe llamarse desde HAL_ADC_ConvCpltCallback().
 *
 * Parámetros:
 *   hlm35 - Handler del sensor.
 *   hadc  - ADC que generó la interrupción; si no es el del sensor, se ignora.
 *
 * Retorna:
 *   Nada.
 */
void LM35_STM32_ADC_Callback(LM35_STM32_t *hlm35, ADC_HandleTypeDef *hadc)
{
    /* El HAL llama este callback para cualquier ADC: procesar solo el nuestro */
    if ((hlm35 == NULL) || (hadc != hlm35->hadc)) {
        return;
    }

    /* 1. Leer la muestra y detener el ADC (conversión simple) */
    uint32_t sample = HAL_ADC_GetValue(hlm35->hadc);
    (void)HAL_ADC_Stop_IT(hlm35->hadc);

    /* Copia local del índice para evitar lecturas repetidas de memoria */
    uint8_t idx = hlm35->buffer_index;

    /* 2. Restar la muestra más vieja y sumar la nueva: media móvil en O(1) */
    hlm35->filter_sum -= hlm35->filter_buffer[idx];
    hlm35->filter_buffer[idx] = sample;
    hlm35->filter_sum += sample;

    /* 3. Avanzar el índice circular con máscara (equivale a módulo con tamaño potencia de 2) */
    hlm35->buffer_index = (uint8_t)((idx + 1U) & LM35_FILTER_MASK);

    /* 4. Contar muestras hasta llenar la ventana, así el promedio inicial no se sesga hacia 0 */
    if (hlm35->samples_count < LM35_FILTER_SIZE) {
        hlm35->samples_count++;
    }

    /* 5. Publicar resultados: solo enteros en la ISR, la conversión a float se hace fuera */
    hlm35->raw_value = sample;
    hlm35->avg_raw   = hlm35->filter_sum / hlm35->samples_count;

    /* 6. Avisar que hay un dato nuevo disponible */
    hlm35->state = LM35_STATE_READY;
}

/*
 * LM35_STM32_ADC_ErrorCallback
 * Maneja un error del ADC: detiene la conversión y devuelve el driver a IDLE
 * para que no quede en BUSY de forma permanente.
 * Debe llamarse desde HAL_ADC_ErrorCallback().
 *
 * Parámetros:
 *   hlm35 - Handler del sensor.
 *   hadc  - ADC que reportó el error; si no es el del sensor, se ignora.
 *
 * Retorna:
 *   Nada.
 */
void LM35_STM32_ADC_ErrorCallback(LM35_STM32_t *hlm35, ADC_HandleTypeDef *hadc)
{
    /* Atender solo errores de nuestro ADC */
    if ((hlm35 != NULL) && (hadc == hlm35->hadc)) {
        (void)HAL_ADC_Stop_IT(hlm35->hadc);
        hlm35->state = LM35_STATE_IDLE;     /* Evita quedar en BUSY para siempre */
    }
}

/*
 * LM35_STM32_GetTemperature
 * Entrega la última temperatura filtrada en °C. Convierte el promedio RAW
 * (calculado en la ISR) fuera de la interrupción y, si había un dato nuevo,
 * lo marca como consumido.
 *
 * Parámetros:
 *   hlm35  - Handler del sensor.
 *   temp_c - Puntero donde se guarda la temperatura en °C.
 *
 * Retorna:
 *   LM35_STATUS_OK            - Temperatura entregada en temp_c.
 *   LM35_STATUS_NO_DATA       - Aún no se completó ninguna conversión.
 *   LM35_STATUS_PARAM_ERROR   - Handler o puntero de salida nulo.
 */
LM35_Status_t LM35_STM32_GetTemperature(LM35_STM32_t *hlm35, float *temp_c)
{
    if ((hlm35 == NULL) || (temp_c == NULL)) {
        return LM35_STATUS_PARAM_ERROR;
    }

    /* Sin muestras no hay temperatura válida: no devolver un 0 engañoso */
    if (hlm35->samples_count == 0U) {
        return LM35_STATUS_NO_DATA;
    }

    /* Convertir el promedio RAW a °C fuera de la ISR */
    *temp_c = LM35_STM32_ConvertRawToTemp(hlm35, hlm35->avg_raw);

    /* Marcar el dato como consumido, sin pisar una conversión que esté en curso */
    if (hlm35->state == LM35_STATE_READY) {
        hlm35->state = LM35_STATE_IDLE;
    }

    return LM35_STATUS_OK;
}
