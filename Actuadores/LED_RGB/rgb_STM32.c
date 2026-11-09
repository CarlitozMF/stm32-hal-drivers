/**
 * @file rgb_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación lógica del driver RGB con encapsulamiento y efectos.
 * @details Este archivo contiene la lógica de bajo nivel para la manipulación
 *          de registros PWM, conversión de modelos de color HSV a RGB, corrección
 *          Gamma y la máquina de estados para efectos no bloqueantes.
 * @version 1.2
 * @date 2026
 */

#include <rgb_STM32.h>

#if RGB_USE_GAMMA_LUT
  #if (MAX_PWM_VALUE != 1000)
    #error "La LUT de gamma está generada para MAX_PWM_VALUE = 1000"
  #endif
  #include <rgb_gamma_lut_STM32.h>
#endif

/* ============================================================================
 * FUNCIONES AUXILIARES PRIVADAS (INTERNAL HELPERS)
 * ============================================================================ */

/* Recorta un valor de intensidad lógica al límite máximo de resolución (0-1000) */
static uint16_t RGB_Clamp_Logical(uint16_t value) {
    return (value > MAX_PWM_VALUE) ? MAX_PWM_VALUE : value;
}

/* Recorta un valor en punto flotante al rango unitario estricto [0.0, 1.0] */
static float RGB_Clamp_Unit(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

/* Incrementa de forma segura el valor de un canal controlando saturación (evita overflow de uint16_t) */
static uint8_t RGB_Step_Up(uint16_t *ch, uint16_t step, uint16_t limit) {
    if ((uint32_t)(*ch) + step >= limit) {
        *ch = limit;
        return 1;
    }
    *ch += step;
    return 0;
}

/* Decrementa de forma segura el valor de un canal evitando underflow por debajo de cero */
static uint8_t RGB_Step_Down(uint16_t *ch, uint16_t step) {
    if (*ch <= step) {
        *ch = 0;
        return 1;
    }
    *ch -= step;
    return 0;
}

/* Aplica la corrección Gamma para adaptar la respuesta del LED a la percepción no lineal del ojo humano */
static uint16_t RGB_Apply_Gamma(uint16_t value, uint16_t max_limit) {
    if (value == 0) return 0;
    value = RGB_Clamp_Logical(value);

#if RGB_USE_GAMMA_LUT
    /* Consulta O(1) instantánea en tabla Flash precalculada (Optimizado para Cortex-M0/M3) */
    uint16_t duty = rgb_gamma_lut[value];
#else
    /* Cálculo dinámico en punto flotante vía powf() (Requiere FPU por hardware) */
    float normalized = (float)value / (float)MAX_PWM_VALUE;
    uint16_t duty = (uint16_t)(powf(normalized, GAMMA_CORRECTION) * MAX_PWM_VALUE + 0.5f);
#endif

    /* Aplicación del tope máximo absoluto de hardware (max_brightness) */
    return (duty > max_limit) ? max_limit : duty;
}

/* Escribe el ciclo de trabajo final en el registro de comparación (CCR) invirtiendo si es Ánodo Común */
static void RGB_Set_Duty_Cycle_Raw(RGB_LED_t *led_ptr, uint32_t channel, uint16_t brightness_value) {
    if (!led_ptr || !led_ptr->htim) return;

    uint16_t final_duty;

    if (led_ptr->led_type == LED_TYPE_CATHODE_COMMON) {
        /* Cátodo Común: Lógica directa (CCR = Brillo) */
        final_duty = brightness_value;
    } else {
        /* Ánodo Común: Lógica invertida (Brillo 0 -> CCR = ARR = 1000 -> Apagado total) */
        final_duty = MAX_PWM_VALUE - brightness_value;
    }

    __HAL_TIM_SET_COMPARE(led_ptr->htim, channel, final_duty);
}

/* ============================================================================
 * IMPLEMENTACIÓN DE LA API PÚBLICA
 * ============================================================================ */

/* Copia la configuración de hardware, reinicia el estado y arranca los canales PWM del timer */
void RGB_Init_Single(RGB_LED_t *led_ptr, RGB_Config_t *config) {
    if (!led_ptr || !config) return;

    /* Desencapsular parámetros de configuración de hardware hacia el objeto de estado */
    led_ptr->htim           = config->htim;
    led_ptr->R_channel      = config->R_channel;
    led_ptr->G_channel      = config->G_channel;
    led_ptr->B_channel      = config->B_channel;
    led_ptr->led_type       = config->led_type;
    led_ptr->max_brightness = (config->max_brightness > MAX_PWM_VALUE)
                              ? MAX_PWM_VALUE : config->max_brightness;

    /* Inicializar variables dinámicas */
    led_ptr->current_r = 0;
    led_ptr->current_g = 0;
    led_ptr->current_b = 0;
    led_ptr->active_effect = EFFECT_NONE;
    led_ptr->fade_state = FADE_G_IN_RED_TO_YELLOW;
    led_ptr->effect_brightness = MAX_PWM_VALUE;
    led_ptr->last_tick_time = HAL_GetTick();

    /* Estado inicial de reposo seguro (OFF) */
    RGB_Set_Color_Direct(led_ptr, 0, 0, 0);

    /* Iniciar la generación de señal PWM en los tres canales del Timer HAL */
    HAL_TIM_PWM_Start(led_ptr->htim, led_ptr->R_channel);
    HAL_TIM_PWM_Start(led_ptr->htim, led_ptr->G_channel);
    HAL_TIM_PWM_Start(led_ptr->htim, led_ptr->B_channel);
}

/* Aplica un color directo recortando rangos, calculando Gamma y actualizando registros CCR */
void RGB_Set_Color_Direct(RGB_LED_t *led_ptr, uint16_t r, uint16_t g, uint16_t b) {
    if (!led_ptr) return;

    /* Normalización de valores lógicos */
    r = RGB_Clamp_Logical(r);
    g = RGB_Clamp_Logical(g);
    b = RGB_Clamp_Logical(b);

    /* Mapeo con corrección Gamma y escalado por brillo máximo */
    uint16_t rf = RGB_Apply_Gamma(r, led_ptr->max_brightness);
    uint16_t gf = RGB_Apply_Gamma(g, led_ptr->max_brightness);
    uint16_t bf = RGB_Apply_Gamma(b, led_ptr->max_brightness);

    /* Transmisión al periférico de hardware */
    RGB_Set_Duty_Cycle_Raw(led_ptr, led_ptr->R_channel, rf);
    RGB_Set_Duty_Cycle_Raw(led_ptr, led_ptr->G_channel, gf);
    RGB_Set_Duty_Cycle_Raw(led_ptr, led_ptr->B_channel, bf);

    /* Guardar valores lógicos actuales en el historial del LED */
    led_ptr->current_r = r;
    led_ptr->current_g = g;
    led_ptr->current_b = b;
}

/* Establece colores predefinidos asignando máxima intensidad lógica a los canales requeridos */
void RGB_Set_Preset(RGB_LED_t *led_ptr, RGB_PresetColor_t color) {
    if (!led_ptr) return;

    const uint16_t full = MAX_PWM_VALUE;
    switch (color) {
        case COLOR_RED:     RGB_Set_Color_Direct(led_ptr, full, 0, 0); break;
        case COLOR_GREEN:   RGB_Set_Color_Direct(led_ptr, 0, full, 0); break;
        case COLOR_BLUE:    RGB_Set_Color_Direct(led_ptr, 0, 0, full); break;
        case COLOR_YELLOW:  RGB_Set_Color_Direct(led_ptr, full, full, 0); break;
        case COLOR_CYAN:    RGB_Set_Color_Direct(led_ptr, 0, full, full); break;
        case COLOR_MAGENTA: RGB_Set_Color_Direct(led_ptr, full, 0, full); break;
        case COLOR_WHITE:   RGB_Set_Color_Direct(led_ptr, full, full, full); break;
        case COLOR_OFF:     RGB_Set_Color_Direct(led_ptr, 0, 0, 0); break;
        default: break;
    }
}

/* Convierte coordenadas del espacio de color HSV a RGB en punto fijo entero (Q8/Q10) */
void RGB_Set_Color_HSV(RGB_LED_t *led_ptr, float h, float s, float v) {
    if (!led_ptr) return;

    /* Conversión de parámetros flotantes de entrada a números enteros con escalado binario */
    const int32_t HUE_FULL   = 360 * 256;   /* Rango completo de tono en formato Q8 */
    const int32_t HUE_SECTOR = 60 * 256;    /* Ancho de cada uno de los 6 sectores en Q8 */

    int32_t hq = (int32_t)(h * 256.0f) % HUE_FULL;
    if (hq < 0) hq += HUE_FULL;

    int32_t sq = (int32_t)(RGB_Clamp_Unit(s) * 1000.0f + 0.5f);  /* Escala 0-1000 */
    int32_t vq = (int32_t)(RGB_Clamp_Unit(v) * 1000.0f + 0.5f);  /* Escala 0-1000 */

    int32_t sector = hq / HUE_SECTOR;                     /* Determinación del sector del prisma (0 a 5) */
    int32_t f = ((hq % HUE_SECTOR) * 1000) / HUE_SECTOR;  /* Posición relativa dentro del sector */

    int32_t c = (vq * sq) / 1000;
    int32_t x = (sector & 1) ? (c * (1000 - f)) / 1000 : (c * f) / 1000;
    int32_t m = vq - c;

    int32_t r, g, b;
    switch (sector) {
        case 0:  r = c; g = x; b = 0; break;
        case 1:  r = x; g = c; b = 0; break;
        case 2:  r = 0; g = c; b = x; break;
        case 3:  r = 0; g = x; b = c; break;
        case 4:  r = x; g = 0; b = c; break;
        default: r = c; g = 0; b = x; break;
    }

    RGB_Set_Color_Direct(led_ptr, (uint16_t)(r + m), (uint16_t)(g + m), (uint16_t)(b + m));
}

/* Ejecuta la máquina de estados de efectos dinámicos en segundo plano basada en intervalos de tiempo */
void RGB_Effects_Handler(RGB_LED_t *led_ptr, uint32_t fade_interval_ms, uint16_t fade_step) {
    if (!led_ptr || led_ptr->active_effect == EFFECT_NONE) return;

    /* Control de temporización no bloqueante mediante comparación de ticks */
    uint32_t now = HAL_GetTick();
    if ((now - led_ptr->last_tick_time) < fade_interval_ms) return;
    led_ptr->last_tick_time = now;

    if (fade_step == 0) fade_step = 1;

    if (led_ptr->active_effect == EFFECT_FADE_RAINBOW) {
        uint16_t peak = led_ptr->effect_brightness;

        /* Máquina de estados de transición entre los 6 colores primarios y secundarios */
        switch (led_ptr->fade_state) {
            case FADE_G_IN_RED_TO_YELLOW:
                if (RGB_Step_Up(&led_ptr->current_g, fade_step, peak))
                    led_ptr->fade_state = FADE_R_OUT_YELLOW_TO_GREEN;
                break;
            case FADE_R_OUT_YELLOW_TO_GREEN:
                if (RGB_Step_Down(&led_ptr->current_r, fade_step))
                    led_ptr->fade_state = FADE_B_IN_GREEN_TO_CYAN;
                break;
            case FADE_B_IN_GREEN_TO_CYAN:
                if (RGB_Step_Up(&led_ptr->current_b, fade_step, peak))
                    led_ptr->fade_state = FADE_G_OUT_CYAN_TO_BLUE;
                break;
            case FADE_G_OUT_CYAN_TO_BLUE:
                if (RGB_Step_Down(&led_ptr->current_g, fade_step))
                    led_ptr->fade_state = FADE_R_IN_BLUE_TO_MAGENTA;
                break;
            case FADE_R_IN_BLUE_TO_MAGENTA:
                if (RGB_Step_Up(&led_ptr->current_r, fade_step, peak))
                    led_ptr->fade_state = FADE_B_OUT_MAGENTA_TO_RED;
                break;
            case FADE_B_OUT_MAGENTA_TO_RED:
                if (RGB_Step_Down(&led_ptr->current_b, fade_step))
                    led_ptr->fade_state = FADE_G_IN_RED_TO_YELLOW;
                break;
            default:
                led_ptr->fade_state = FADE_G_IN_RED_TO_YELLOW;
                break;
        }
        RGB_Set_Color_Direct(led_ptr, led_ptr->current_r, led_ptr->current_g, led_ptr->current_b);
    }
}

/* Configura los parámetros iniciales e inicia el barrido del efecto Rainbow */
void RGB_Start_Effect_Rainbow(RGB_LED_t *led_ptr, uint16_t peak_brightness) {
    if (!led_ptr) return;
    RGB_Stop_Effect(led_ptr);

    led_ptr->effect_brightness = RGB_Clamp_Logical(peak_brightness);
    led_ptr->current_r = led_ptr->effect_brightness;
    led_ptr->current_g = 0;
    led_ptr->current_b = 0;
    led_ptr->fade_state = FADE_G_IN_RED_TO_YELLOW;
    led_ptr->last_tick_time = HAL_GetTick();
    led_ptr->active_effect = EFFECT_FADE_RAINBOW;
}

/* Desactiva el motor de efectos manteniendo el último color establecido */
void RGB_Stop_Effect(RGB_LED_t *led_ptr) {
    if (led_ptr) led_ptr->active_effect = EFFECT_NONE;
}
