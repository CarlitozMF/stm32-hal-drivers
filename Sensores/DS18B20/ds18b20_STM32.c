/**
 * @file ds18b20_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación del driver para sensores de temperatura DS18B20 (1-Wire).
 * @details Maneja los tiempos del bus a nivel de microsegundos mediante un contador de
 *          hardware (TIM) configurado desde el .ioc con tick de 1 us. El pin se configura una sola vez como salida Open-Drain, de modo
 *          que el bus se libera escribiendo '1' y se lee por IDR sin reconfigurar el GPIO
 *          (HAL_GPIO_Init en cada bit introducía retardos que rompían la ventana de muestreo).
 *          Los time slots críticos se ejecutan con interrupciones deshabilitadas.
 * @version 1.1
 * @date 2026
 */

/* Includes ------------------------------------------------------------------*/
#include "ds18b20_STM32.h"

/* Private Macros ------------------------------------------------------------*/

/* Acceso directo a registros: BSRR es atómico y existe en todas las familias STM32 */
#define DS18B20_PIN_LOW(d)       ((d)->GPIOx->BSRR = ((uint32_t)(d)->GPIO_Pin << 16U))
#define DS18B20_PIN_RELEASE(d)   ((d)->GPIOx->BSRR = (uint32_t)(d)->GPIO_Pin)
#define DS18B20_PIN_READ(d)      (((d)->GPIOx->IDR & (d)->GPIO_Pin) ? 1U : 0U)

#define DS18B20_ENTER_CRITICAL() uint32_t primask_ = __get_PRIMASK(); __disable_irq()
#define DS18B20_EXIT_CRITICAL()  __set_PRIMASK(primask_)

/* Private Functions Prototypes ----------------------------------------------*/
static void DS18B20_DelayUs(DS18B20_NameTypedef* dev, uint16_t us);
static DS18B20_Status_t DS18B20_CheckTimerBase(DS18B20_NameTypedef* dev);
static uint8_t DS18B20_CRC8(const uint8_t* data, uint8_t len);
static DS18B20_Status_t DS18B20_Reset(DS18B20_NameTypedef* dev);
static void DS18B20_WriteBit(DS18B20_NameTypedef* dev, uint8_t bit);
static uint8_t DS18B20_ReadBit(DS18B20_NameTypedef* dev);
static void DS18B20_WriteByte(DS18B20_NameTypedef* dev, uint8_t data);
static uint8_t DS18B20_ReadByte(DS18B20_NameTypedef* dev);

/* Private Functions Implementation ------------------------------------------*/

/*
 * Genera una demora en microsegundos monitoreando el contador del Timer
 * asignado (1 tick = 1 us). Requiere ARR >= 0xFFFF (lo verifica DS18B20_Init).
 */
static void DS18B20_DelayUs(DS18B20_NameTypedef* dev, uint16_t us) {
    __HAL_TIM_SET_COUNTER(dev->htim, 0);
    while (__HAL_TIM_GET_COUNTER(dev->htim) < us);
}

/*
 * Verifica que el Timer cuente a 1 MHz: sincroniza con un flanco de SysTick, deja pasar
 * exactamente 2 ms y comprueba que el contador avanzó ~2000 ticks (+-5 %).
 * Requiere SysTick activo e interrupciones habilitadas (llamar luego de HAL_Init).
 */
static DS18B20_Status_t DS18B20_CheckTimerBase(DS18B20_NameTypedef* dev) {
    uint32_t t = HAL_GetTick();
    while (HAL_GetTick() == t);                 /* Sincroniza con un flanco de SysTick */

    __HAL_TIM_SET_COUNTER(dev->htim, 0);
    t = HAL_GetTick();
    while ((HAL_GetTick() - t) < 2U);           /* 2 ms exactos entre flancos */

    uint32_t ticks = __HAL_TIM_GET_COUNTER(dev->htim);
    return (ticks >= 1900U && ticks <= 2100U) ? DS18B20_OK : DS18B20_ERR_TIMER;
}

/*
 * CRC8 de Dallas/Maxim (polinomio x^8 + x^5 + x^4 + 1, 0x8C reflejado).
 * Al calcularlo sobre los datos + el byte de CRC, el resultado debe ser 0.
 */
static uint8_t DS18B20_CRC8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0;
    while (len--) {
        uint8_t byte = *data++;
        for (uint8_t i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    return crc;
}

/* 1-Wire Low-Level Primitives -----------------------------------------------*/

/*
 * Emite el pulso de Master Reset (480 us en bajo) y muestrea la respuesta
 * del esclavo (Presence Pulse). La ventana de muestreo se ejecuta sin interrupciones.
 */
static DS18B20_Status_t DS18B20_Reset(DS18B20_NameTypedef* dev) {
    uint8_t presence;

    /* Drena el bus a cero durante t_RSTL (mínimo 480 us) */
    DS18B20_PIN_LOW(dev);
    DS18B20_DelayUs(dev, 480);

    {
        DS18B20_ENTER_CRITICAL();

        /* Suelta la línea y espera la respuesta del sensor (presence: 15-60 us de espera, 60-240 us de duración) */
        DS18B20_PIN_RELEASE(dev);
        DS18B20_DelayUs(dev, 70);

        /* Si el sensor detectó el Reset, mantiene la línea a GND (ACK) */
        presence = (DS18B20_PIN_READ(dev) == 0U) ? 1U : 0U;

        DS18B20_EXIT_CRITICAL();
    }

    /* Completa la ventana total t_RSTH del slot de Reset (mínimo 480 us) */
    DS18B20_DelayUs(dev, 410);

    return (presence == 1U) ? DS18B20_OK : DS18B20_ERR_NO_DEVICE;
}

/*
 * Transmite un bit respetando las especificaciones del time slot (>= 60 us):
 * - '1': pulso corto en bajo (6 us) y libera la línea.
 * - '0': mantiene el bus a GND 60 us.
 */
static void DS18B20_WriteBit(DS18B20_NameTypedef* dev, uint8_t bit) {
    DS18B20_ENTER_CRITICAL();

    DS18B20_PIN_LOW(dev);
    DS18B20_DelayUs(dev, bit ? 6U : 60U);
    DS18B20_PIN_RELEASE(dev);

    DS18B20_EXIT_CRITICAL();

    /* Completa el slot y deja tiempo de recuperación (>= 1 us) */
    DS18B20_DelayUs(dev, bit ? 64U : 10U);
}

/*
 * Inicia el slot de lectura bajando el bus 2 us, libera la línea y muestrea
 * el estado enviado por el sensor antes de los 15 us contados desde el flanco de bajada.
 */
static uint8_t DS18B20_ReadBit(DS18B20_NameTypedef* dev) {
    uint8_t bit;

    DS18B20_ENTER_CRITICAL();

    DS18B20_PIN_LOW(dev);
    DS18B20_DelayUs(dev, 2);
    DS18B20_PIN_RELEASE(dev);
    DS18B20_DelayUs(dev, 8);

    bit = (uint8_t)DS18B20_PIN_READ(dev);

    DS18B20_EXIT_CRITICAL();

    /* Completa el slot de lectura (>= 60 us) y la recuperación entre slots */
    DS18B20_DelayUs(dev, 55);

    return bit;
}

/*
 * Transmite un byte comenzando por el bit menos significativo (LSB First).
 */
static void DS18B20_WriteByte(DS18B20_NameTypedef* dev, uint8_t data) {
    for (uint8_t i = 0; i < 8; i++) {
        DS18B20_WriteBit(dev, (data >> i) & 0x01U);
    }
}

/*
 * Lee un byte del bus 1-Wire reconstruyéndolo desde el LSB hasta el MSB.
 */
static uint8_t DS18B20_ReadByte(DS18B20_NameTypedef* dev) {
    uint8_t data = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (DS18B20_ReadBit(dev)) {
            data |= (uint8_t)(1U << i);
        }
    }
    return data;
}

/* Public Functions Implementation -------------------------------------------*/

/*
 * Configura el pin como Open-Drain, arranca el Timer (configurado en el .ioc),
 * verifica su base de tiempo de 1 us y ejecuta el test de presencia.
 */
DS18B20_Status_t DS18B20_Init(DS18B20_NameTypedef* dev, GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, TIM_HandleTypeDef* htim) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (dev == NULL || GPIOx == NULL || htim == NULL) return DS18B20_ERROR;

    dev->GPIOx = GPIOx;
    dev->GPIO_Pin = GPIO_Pin;
    dev->htim = htim;

    /* ARR debe permitir contar hasta 65535 us; si es menor, DelayUs(480) se colgaría */
    if (__HAL_TIM_GET_AUTORELOAD(htim) < 0xFFFFU) return DS18B20_ERR_TIMER;

    /* Pin Open-Drain: '1' = línea liberada (Pull-Up externa), '0' = línea a GND */
    DS18B20_PIN_RELEASE(dev);
    GPIO_InitStruct.Pin = GPIO_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOx, &GPIO_InitStruct);

    /* Arranca el Timer con la configuración del .ioc (no se modifica PSC ni ARR) */
    __HAL_TIM_SET_COUNTER(htim, 0);
    if (HAL_TIM_Base_Start(htim) != HAL_OK) return DS18B20_ERROR;

    if (DS18B20_CheckTimerBase(dev) != DS18B20_OK) return DS18B20_ERR_TIMER;

    return DS18B20_Reset(dev);
}

/*
 * Emite la orden global de conversión a todos los sensores (SKIP ROM 0xCC + CONVERT T 0x44).
 */
DS18B20_Status_t DS18B20_StartAllConversion(DS18B20_NameTypedef* dev) {
    if (dev == NULL) return DS18B20_ERROR;

    if (DS18B20_Reset(dev) != DS18B20_OK) return DS18B20_ERR_NO_DEVICE;

    DS18B20_WriteByte(dev, DS18B20_CMD_SKIP_ROM);
    DS18B20_WriteByte(dev, DS18B20_CMD_CONVERT_T);

    return DS18B20_OK;
}

/*
 * Mientras los sensores convierten responden '0' a un slot de lectura; al terminar, '1'.
 */
DS18B20_Status_t DS18B20_IsConversionDone(DS18B20_NameTypedef* dev, uint8_t* done) {
    if (dev == NULL || done == NULL) return DS18B20_ERROR;

    *done = DS18B20_ReadBit(dev);
    return DS18B20_OK;
}

/*
 * Lee los 9 bytes del Scratchpad, valida el CRC, descarta los bits indefinidos según la
 * resolución configurada y escala a grados Celsius.
 * Nota: el valor 85.0 °C es el de power-on reset; si aparece, la conversión pudo no ejecutarse.
 */
DS18B20_Status_t DS18B20_ReadTempByROM(DS18B20_NameTypedef* dev, const uint8_t* rom_code, float* temperature) {
    uint8_t scratchpad[9];

    if (dev == NULL || temperature == NULL) return DS18B20_ERROR;

    if (DS18B20_Reset(dev) != DS18B20_OK) return DS18B20_ERR_NO_DEVICE;

    if (rom_code == NULL) {
        /* Bus monopunto: se saltea la verificación de dirección */
        DS18B20_WriteByte(dev, DS18B20_CMD_SKIP_ROM);
    } else {
        /* Bus multipunto: direcciona al esclavo enviando sus 64 bits de ROM */
        DS18B20_WriteByte(dev, DS18B20_CMD_MATCH_ROM);
        for (uint8_t i = 0; i < DS18B20_ROM_CODE_SIZE; i++) {
            DS18B20_WriteByte(dev, rom_code[i]);
        }
    }

    DS18B20_WriteByte(dev, DS18B20_CMD_READ_SCRATCHPAD);

    for (uint8_t i = 0; i < 9; i++) {
        scratchpad[i] = DS18B20_ReadByte(dev);
    }

    /* CRC8 sobre los 9 bytes (incluido el byte 8 = CRC) debe dar 0 */
    if (DS18B20_CRC8(scratchpad, 9) != 0U) return DS18B20_ERR_CRC;

    /* Une Byte 0 (LSB) y Byte 1 (MSB) */
    uint16_t raw = (uint16_t)(((uint16_t)scratchpad[1] << 8) | scratchpad[0]);

    /* Resolución en bits 6:5 del registro de configuración (byte 4): 0 = 9 bits ... 3 = 12 bits.
       En resoluciones menores, los bits bajos están indefinidos y se anulan. */
    uint8_t resolution = (scratchpad[4] >> 5) & 0x03U;
    uint16_t undefined_mask = (uint16_t)((1U << (3U - resolution)) - 1U);
    raw &= (uint16_t)~undefined_mask;

    *temperature = (float)((int16_t)raw) / 16.0f;

    return DS18B20_OK;
}

/*
 * Lectura simplificada para un único sensor: dispara la conversión, espera consultando
 * al sensor (con timeout) y lee la temperatura.
 */
DS18B20_Status_t DS18B20_ReadSingleTemp(DS18B20_NameTypedef* dev, float* temperature) {
    uint8_t done = 0;
    uint32_t start;

    if (dev == NULL || temperature == NULL) return DS18B20_ERROR;

    if (DS18B20_StartAllConversion(dev) != DS18B20_OK) return DS18B20_ERR_NO_DEVICE;

    start = HAL_GetTick();
    do {
        (void)DS18B20_IsConversionDone(dev, &done);
        if (!done) {
            if ((HAL_GetTick() - start) > DS18B20_CONVERSION_TIMEOUT_MS) return DS18B20_ERR_TIMEOUT;
            HAL_Delay(1);
        }
    } while (!done);

    return DS18B20_ReadTempByROM(dev, NULL, temperature);
}

/*
 * Algoritmo Search ROM (Maxim AN187) para descubrir las direcciones de 64 bits de los sensores.
 */
DS18B20_Status_t DS18B20_ScanBus(DS18B20_NameTypedef* dev, uint8_t rom_table[][DS18B20_ROM_CODE_SIZE], uint8_t max_sensors, uint8_t* found_count) {
    uint8_t last_discrepancy = 0;
    uint8_t last_device_flag = 0;
    uint8_t last_zero_branch;
    uint8_t rom_byte_number;
    uint8_t bit_number;
    uint8_t asm_bit;
    uint8_t cmp_asm_bit;
    uint8_t search_direction;
    uint8_t current_rom[DS18B20_ROM_CODE_SIZE] = {0};

    if (dev == NULL || found_count == NULL || rom_table == NULL) return DS18B20_ERROR;

    *found_count = 0;

    while (!last_device_flag && (*found_count < max_sensors)) {
        if (DS18B20_Reset(dev) != DS18B20_OK) {
            return DS18B20_ERR_NO_DEVICE;
        }

        DS18B20_WriteByte(dev, DS18B20_CMD_SEARCH_ROM);

        bit_number = 1;
        rom_byte_number = 0;
        last_zero_branch = 0;

        while (bit_number <= 64) {
            /* Lee el bit real y su complemento enviados por los esclavos */
            asm_bit = DS18B20_ReadBit(dev);
            cmp_asm_bit = DS18B20_ReadBit(dev);

            /* '1' y '1': ningún dispositivo participa en la búsqueda */
            if (asm_bit == 1U && cmp_asm_bit == 1U) {
                break;
            }

            if (asm_bit != cmp_asm_bit) {
                /* Todos los dispositivos activos coinciden en este bit */
                search_direction = asm_bit;
            } else {
                /* Discrepancia: hay sensores con '0' y con '1' */
                if (bit_number < last_discrepancy) {
                    search_direction = (current_rom[rom_byte_number] & (1U << ((bit_number - 1U) % 8U))) ? 1U : 0U;
                } else {
                    search_direction = (bit_number == last_discrepancy) ? 1U : 0U;
                }

                if (search_direction == 0U) {
                    last_zero_branch = bit_number;
                }
            }

            if (search_direction == 1U) {
                current_rom[rom_byte_number] |= (uint8_t)(1U << ((bit_number - 1U) % 8U));
            } else {
                current_rom[rom_byte_number] &= (uint8_t)~(1U << ((bit_number - 1U) % 8U));
            }

            /* Informa a los esclavos la rama elegida para descartar a los que no coincidan */
            DS18B20_WriteBit(dev, search_direction);

            bit_number++;
            rom_byte_number = (uint8_t)((bit_number - 1U) / 8U);
        }

        /* Búsqueda incompleta: no hay más dispositivos que respondan (evita bucle infinito) */
        if (bit_number <= 64U) {
            break;
        }

        /* ROM completa: validar CRC8 (7 bytes + byte de CRC debe dar 0) */
        if (DS18B20_CRC8(current_rom, DS18B20_ROM_CODE_SIZE) != 0U) {
            return DS18B20_ERR_CRC;
        }

        last_discrepancy = last_zero_branch;
        if (last_discrepancy == 0U) {
            last_device_flag = 1U;
        }

        for (uint8_t i = 0; i < DS18B20_ROM_CODE_SIZE; i++) {
            rom_table[*found_count][i] = current_rom[i];
        }
        (*found_count)++;
    }

    return (*found_count > 0U) ? DS18B20_OK : DS18B20_ERR_NO_DEVICE;
}
