/**
 * @file utils.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Retardos en microsegundos portables entre familias STM32.
 * @details Método preferido: contador de ciclos DWT->CYCCNT (Cortex-M3/M4/M7/M33).
 *          Respaldo: SysTick (Cortex-M0/M0+ o si el DWT no cuenta).
 *          El retardo es un mínimo garantizado: una interrupción puede alargarlo.
 * @version 2.1
 * @date 2026
 */

#include "utils.h"

/* Cortex-M23 tiene DWT pero sin CYCCNT, por eso se excluye */
#if (__CORTEX_M >= 3U) && (__CORTEX_M != 23U)
#define UTILS_HAS_DWT   1
#else
#define UTILS_HAS_DWT   0
#endif

#if UTILS_HAS_DWT
static uint8_t use_dwt = 0U;   /*!< 1 si el DWT->CYCCNT está corriendo */
#endif

/**
 * @brief Habilita el contador DWT si existe; si no, queda listo el respaldo SysTick.
 * @details No reinicia CYCCNT si ya estaba en marcha, así no interfiere con
 *          otros usos del contador (profiling, timeouts).
 */
void Utils_DelayUs_Init(void)
{
#if UTILS_HAS_DWT
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

#if (__CORTEX_M == 7U)
    *(volatile uint32_t *)0xE0001FB0UL = 0xC5ACCE55UL;  /* DWT->LAR: desbloqueo en Cortex-M7 */
#endif

    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) {
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }

    /* Verificar que el contador realmente avanza */
    uint32_t c0 = DWT->CYCCNT;
    __NOP(); __NOP(); __NOP(); __NOP();
    use_dwt = (DWT->CYCCNT != c0) ? 1U : 0U;
#endif
}

/**
 * @brief Retardo bloqueante en microsegundos.
 * @param us Microsegundos a esperar.
 */
void Utils_DelayUs(uint32_t us)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000U;
    if (cycles_per_us == 0U) cycles_per_us = 1U;

#if UTILS_HAS_DWT
    if (use_dwt) {
        const uint32_t ticks = us * cycles_per_us;
        const uint32_t start = DWT->CYCCNT;
        while ((DWT->CYCCNT - start) < ticks) { }
        return;
    }
#endif

    /* ---- Respaldo: SysTick (cuenta hacia abajo y recarga desde LOAD) ---- */

    /* Si SysTick no está corriendo, espera aproximada por software */
    if ((SysTick->CTRL & SysTick_CTRL_ENABLE_Msk) == 0U) {
        volatile uint32_t n = us * cycles_per_us / 4U + 1U;
        while (n--) { }
        return;
    }

    /* Si SysTick usa HCLK/8, cada tick dura 8 ciclos de CPU */
    if ((SysTick->CTRL & SysTick_CTRL_CLKSOURCE_Msk) == 0U) {
        cycles_per_us /= 8U;
        if (cycles_per_us == 0U) cycles_per_us = 1U;
    }

    const uint32_t ticks  = us * cycles_per_us;
    const uint32_t reload = SysTick->LOAD + 1U;
    uint32_t prev    = SysTick->VAL;
    uint32_t elapsed = 0U;

    while (elapsed < ticks) {
        uint32_t now = SysTick->VAL;
        elapsed += (now <= prev) ? (prev - now) : (prev + reload - now);
        prev = now;
    }
}
