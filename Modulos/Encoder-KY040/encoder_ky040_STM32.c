/**
 * @file encoder_ky040_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación del driver para encoder rotativo KY-040.
 * @details Implementa la decodificación por tabla de estados de cuadratura,
 *          gestión atómica de secciones críticas y debounce temporal para el pulsador.
 * @version 1.3
 * @date 2026
 */

#include <encoder_ky040_STM32.h>

/* ============================================================================
 * CONFIGURACIONES Y CONSTANTES PRIVADAS
 * ============================================================================ */

/*
 * Número de transiciones de cuadratura (flancos) necesarias para registrar
 * un incremento o decremento completo (un "clic" físico o detent del encoder).
 * Los KY-040 económicos suelen requerir 4 cambios de estado para una muesca.
 */
#ifndef KY040_STEPS_PER_DETENT
#define KY040_STEPS_PER_DETENT  4
#endif

/*
 * Tiempo mínimo de silencio exigido al pulsador en milisegundos.
 * Actúa como filtro para ignorar los rebotes mecánicos tanto al presionar como al soltar.
 */
#ifndef KY040_SW_DEBOUNCE_MS
#define KY040_SW_DEBOUNCE_MS    50U
#endif

/*
 * Tabla de decodificación de cuadratura (Full Quadrature Decoding) de 16 posiciones.
 *
 * Matemáticamente, combinamos el estado anterior de los pines y el estado actual:
 * Índice de la tabla = (estado_previo << 2) | estado_actual
 * El estado se compone de: (Canal_A << 1) | Canal_B (un valor binario de 0 a 3).
 *
 * Resultados en la tabla:
 *  1 : Representa un avance válido en sentido horario (Clockwise).
 * -1 : Representa un avance válido en sentido antihorario (Counter-Clockwise).
 *  0 : Estado estacionario, transición inválida o rebote mecánico a filtrar.
 */
static const int8_t kQuadTable[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

/* ============================================================================
 * SECCIONES CRÍTICAS (PROTECCIÓN CONTRA CONDICIONES DE CARRERA)
 * ============================================================================
 * Guardamos el registro PRIMASK para saber si las interrupciones ya estaban
 * deshabilitadas previamente, permitiendo llamadas anidadas seguras.
 */
#define KY040_ENTER_CRITICAL()  uint32_t _primask = __get_PRIMASK(); __disable_irq()
#define KY040_EXIT_CRITICAL()   do { if (!_primask) __enable_irq(); } while (0)

/* ============================================================================
 * FUNCIONES AUXILIARES PRIVADAS (INLINE)
 * ============================================================================ */

/* Lee los pines físicos A y B y los empaqueta en un byte de 2 bits (Bit 1 = CLK, Bit 0 = DT) */
static inline uint8_t ky040_read_state(const KY040_t *dev) {
    uint8_t a = (HAL_GPIO_ReadPin(dev->port_A, dev->pin_A) == GPIO_PIN_SET) ? 1U : 0U;
    uint8_t b = (HAL_GPIO_ReadPin(dev->port_B, dev->pin_B) == GPIO_PIN_SET) ? 1U : 0U;
    return (uint8_t)((a << 1) | b);
}

/* Aplica límites estrictos (Saturación) sobre el valor de posición */
static inline int32_t ky040_clamp(const KY040_t *dev, int32_t v) {
    if (v > dev->max_val) return dev->max_val;
    if (v < dev->min_val) return dev->min_val;
    return v;
}

/* ============================================================================
 * FUNCIONES PÚBLICAS DEL DRIVER
 * ============================================================================ */

/* Inicializa la estructura de control, ordena límites y fija el estado inicial en el punto medio del rango */
void KY040_Init(KY040_t *dev,
                GPIO_TypeDef* pA, uint16_t pinA,
                GPIO_TypeDef* pB, uint16_t pinB,
                GPIO_TypeDef* pSW, uint16_t pinSW,
                int32_t min, int32_t max) {

    if (dev == NULL) return;

    /* Ordenamiento defensivo si los límites fueron pasados de forma invertida */
    if (min > max) {
        int32_t tmp = min;
        min = max;
        max = tmp;
    }

    /* Asignación de los recursos de hardware a la estructura */
    dev->port_A  = pA;   dev->pin_A  = pinA;
    dev->port_B  = pB;   dev->pin_B  = pinB;
    dev->port_SW = pSW;  dev->pin_SW = pinSW;

    dev->min_val = min;
    dev->max_val = max;

    /*
     * Cálculo de la posición inicial fija en el punto medio exacto del rango.
     * Se utiliza casting a int64_t para prevenir desbordamientos matemáticos con rangos extensos.
     */
    dev->position = min + (int32_t)(((int64_t)max - (int64_t)min) / 2);

    /* Inicialización de temporizadores, estados y acumuladores de pasos */
    dev->last_tick_SW = HAL_GetTick();
    dev->sw_pressed   = 0;
    dev->accum        = 0;

    /* Captura del estado eléctrico inicial de los pines para partir de una base limpia */
    dev->state        = ky040_read_state(dev);
}

/* Manejador unificado invocable desde la interrupción EXTI de la HAL */
void KY040_IRQ_Handler(KY040_t *dev, uint16_t triggered_pin) {

    /* --- CASO 1: Interrupción disparada por giro (Canal CLK o Canal DT) --- */
    if (triggered_pin == dev->pin_A || triggered_pin == dev->pin_B) {

        /* 1. Leer el estado eléctrico actual de ambos canales */
        uint8_t curr = ky040_read_state(dev);

        /* 2. Decodificar la transición mediante el historial de estados y la tabla de cuadratura */
        int8_t step = kQuadTable[(dev->state << 2) | curr];

        /* 3. Actualizar el estado previo */
        dev->state = curr;

        /* 4. Si el paso representa una transición válida, acumular la trayectoria */
        if (step != 0) {
            dev->accum += step;

            /* Mover la posición lógica cuando se acumulan suficientes pasos por muesca mecánica */
            if (dev->accum >= KY040_STEPS_PER_DETENT) {
                dev->accum = 0;
                dev->position = ky040_clamp(dev, dev->position + 1);
            } else if (dev->accum <= -KY040_STEPS_PER_DETENT) {
                dev->accum = 0;
                dev->position = ky040_clamp(dev, dev->position - 1);
            }
        }
    }
    /* --- CASO 2: Interrupción disparada por el pulsador (Pin SW) --- */
    else if (triggered_pin == dev->pin_SW) {
        uint32_t ahora  = HAL_GetTick();
        uint32_t quieto = ahora - dev->last_tick_SW;

        /* Actualización de la marca de tiempo para el cálculo del filtro anti-rebote */
        dev->last_tick_SW = ahora;

        /* Verificar el tiempo de silencio transcurrido y la lectura física activa en BAJO (Active Low) */
        if (quieto >= KY040_SW_DEBOUNCE_MS &&
            HAL_GPIO_ReadPin(dev->port_SW, dev->pin_SW) == GPIO_PIN_RESET) {
            dev->sw_pressed = 1;
        }
    }
}

/* Retorna la posición actual garantizando una lectura atómica mediante sección crítica */
int32_t KY040_GetPosition(KY040_t *dev) {
    KY040_ENTER_CRITICAL();
    int32_t p = dev->position;
    KY040_EXIT_CRITICAL();
    return p;
}

/* Modifica la posición actual aplicando saturación de rango y reseteando la trayectoria parcial */
void KY040_SetPosition(KY040_t *dev, int32_t new_position) {
    KY040_ENTER_CRITICAL();
    dev->position = ky040_clamp(dev, new_position);
    dev->accum = 0;
    KY040_EXIT_CRITICAL();
}

/* Lee y resetea de forma atómica la bandera de pulsación */
uint8_t KY040_IsButtonPressed(KY040_t *dev) {
    uint8_t pressed;

    KY040_ENTER_CRITICAL();
    pressed = dev->sw_pressed;
    dev->sw_pressed = 0;
    KY040_EXIT_CRITICAL();

    return pressed;
}
