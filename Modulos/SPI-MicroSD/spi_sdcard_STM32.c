/**
 * @file spi_sdcard_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Driver para MicroSD via SPI (STM32 HAL) con trazas opcionales por UART.
 * @details Contiene la gestión de protocolo SPI en modo esclavo, secuencia de arranque SDv1/SDv2/MMC,
 *          decodificación de tarjetas SDHC mediante CSD y lectura/escritura de bloques LBA de 512 bytes.
 * @version 1.2
 * @date 2026
 */

#include <spi_sdcard_STM32.h>
#include <string.h>

/* ============================================================================
 * CONFIGURACIÓN Y MACROS PRIVADAS
 * ============================================================================ */

/* Habilita o deshabilita las trazas de depuración por UART (1 = Habilitado, 0 = Deshabilitado) */
#ifndef SD_DEBUG
#define SD_DEBUG 0
#endif

/* Baudrate de la UART de debug para calcular los retardos de transmisión asíncrona */
#ifndef SD_DEBUG_BAUD
#define SD_DEBUG_BAUD 115200U
#endif

/*
 * Prescaler para la inicialización: el reloj SCK debe mantenerse estrictamente entre 100 kHz y 400 kHz.
 * Ej: SPI1 (APB2 a 90 MHz) / 256 = 351 kHz ; SPI3 (APB1 a 45 MHz) / 256 = 176 kHz.
 */
#ifndef SD_INIT_PRESCALER
#define SD_INIT_PRESCALER SPI_BAUDRATEPRESCALER_256
#endif

/*
 * Prescaler para operación normal en modo ejecución (SCK <= 25 MHz).
 * Ej: SPI1 (90 MHz): usar /8 -> 11.25 MHz ; SPI3 (45 MHz): usar /4 -> 11.25 MHz.
 */
#ifndef SD_RUN_PRESCALER
#define SD_RUN_PRESCALER SPI_BAUDRATEPRESCALER_4
#endif

#if SD_DEBUG
#include "uart_driver.h"
#include <stdio.h>
#include <stdarg.h>
#endif

/* ============================================================================
 * COMANDOS ESTÁNDAR DEL PROTOCOLO SPI PARA TARJETAS SD
 * ============================================================================ */
#define SD_CMD0    (0)         /* GO_IDLE_STATE: Reinicia la tarjeta a modo SPI */
#define SD_CMD1    (1)         /* SEND_OP_COND (Específico para tarjetas MMC) */
#define SD_CMD8    (8)         /* SEND_IF_COND: Verifica el voltaje de interfaz (SDv2) */
#define SD_CMD9    (9)         /* SEND_CSD: Lee el registro de datos específicos de la tarjeta (CSD) */
#define SD_CMD16   (16)        /* SET_BLOCKLEN: Configura el tamaño del bloque (fijado a 512 bytes) */
#define SD_CMD17   (17)        /* READ_SINGLE_BLOCK: Lee un bloque de 512 bytes */
#define SD_CMD24   (24)        /* WRITE_BLOCK: Escribe un bloque de 512 bytes */
#define SD_CMD55   (55)        /* APP_CMD: Prefijo indicador de que el siguiente comando es de aplicación (ACMD) */
#define SD_CMD58   (58)        /* READ_OCR: Lee el registro de condiciones de operación (OCR) */
#define SD_ACMD41  (0x80 | 41) /* SD_SEND_OP_COND (El bit 7 marca que requiere CMD55 previo) */

/* Máscaras y tipos de tarjeta (combinables en flags) */
#define CT_MMC     0x01        /* Tarjeta tipo Multimedia Card (MMC) */
#define CT_SD1     0x02        /* Tarjeta SD versión 1.x */
#define CT_SD2     0x04        /* Tarjeta SD versión 2.0 estándar */
#define CT_BLOCK   0x08        /* Direccionamiento por bloques de 512 bytes (SDHC / SDXC) */

/* ============================================================================
 * VARIABLES ESTÁTICAS PRIVADAS DEL DRIVER
 * ============================================================================ */
static SPI_HandleTypeDef *sd_hspi = NULL;   /* Puntero al periférico SPI activo */
static GPIO_TypeDef      *sd_cs_port = NULL;/* Puerto GPIO del pin Chip Select */
static uint16_t           sd_cs_pin = 0;    /* Máscara del pin Chip Select */
static uint8_t            card_type = 0;    /* Tipo de tarjeta detectada e inicializada */

/* ============================================================================
 * SISTEMA DE TRAZAS OPCIONALES POR UART
 * ============================================================================ */
#if SD_DEBUG
static void SD_Log(const char *fmt, ...)
{
    static char buf[80];            /* Estático: permanece en memoria durante la transmisión por interrupción */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    UART_Driver_SendString(buf);
    /* Espera calculada al baudrate configurado (10 bits por carácter) para evitar solapamiento */
    HAL_Delay((uint32_t)n * 10U * 1000U / SD_DEBUG_BAUD + 2U);
}
#define SD_LOG(...) SD_Log(__VA_ARGS__)
#else
#define SD_LOG(...) ((void)0)
#endif

/* ============================================================================
 * FUNCIONES DE BAJO NIVEL (CAPA FÍSICA SPI)
 * ============================================================================ */

/* Transmite un byte por SPI y simultáneamente recibe la respuesta del esclavo */
static uint8_t SD_TransferByte(uint8_t data)
{
    uint8_t rx = 0xFF;
    HAL_SPI_TransmitReceive(sd_hspi, &data, &rx, 1, 100);
    return rx;
}

/* Espera activa basada en ticks hasta que la tarjeta libere la línea MISO (devuelve 0xFF). Retorna 1 si quedó lista. */
static uint8_t SD_WaitReady(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();
    do {
        if (SD_TransferByte(0xFF) == 0xFF) return 1;
    } while ((HAL_GetTick() - t0) < ms);
    return 0;
}

/* Deselecciona la tarjeta poniendo CS en HIGH y emitiendo 8 ciclos de reloj extra (clocks dummy) */
static void SD_Deselect(void)
{
    HAL_GPIO_WritePin(sd_cs_port, sd_cs_pin, GPIO_PIN_SET);
    SD_TransferByte(0xFF);   /* 8 clocks adicionales para forzar la liberación del pin MISO por la SD */
}

/* Selecciona la tarjeta poniendo CS en LOW y esperando a que el bus esté listo */
static uint8_t SD_Select(void)
{
    HAL_GPIO_WritePin(sd_cs_port, sd_cs_pin, GPIO_PIN_RESET);
    SD_TransferByte(0xFF);
    if (SD_WaitReady(500)) return 1;
    SD_Deselect();
    return 0;
}

/* Modifica el prescaler de velocidad del bus SPI dinámicamente según la fase de init o ejecución */
static void SD_SetPrescaler(uint32_t prescaler)
{
    __HAL_SPI_DISABLE(sd_hspi);
    sd_hspi->Init.BaudRatePrescaler = prescaler;
    MODIFY_REG(sd_hspi->Instance->CR1, SPI_CR1_BR, prescaler);
    __HAL_SPI_ENABLE(sd_hspi);
}

/*
 * Envía un comando de 6 bytes al bus SPI y devuelve la respuesta R1 obtenida.
 * Nota: Deja el pin CS en bajo (activo); el llamador debe invocar SD_Deselect() al finalizar.
 */
static uint8_t SD_SendCmd(uint8_t cmd, uint32_t arg)
{
    uint8_t crc = 0x01;   /* CRC por defecto + bit de stop obligatorio en modo SPI */
    uint8_t res, n;

    if (cmd & 0x80) {     /* Si el bit 7 está activo, indica que es un comando de aplicación (ACMDn = CMD55 + CMDn) */
        cmd &= 0x7F;
        res = SD_SendCmd(SD_CMD55, 0);
        if (res > 1) return res;
    }

    SD_Deselect();
    if (!SD_Select()) return 0xFF;

    /* Códigos CRC específicos requeridos obligatoriamente por el estándar SD para CMD0 y CMD8 */
    if (cmd == SD_CMD0) crc = 0x95;
    if (cmd == SD_CMD8) crc = 0x87;

    /* Transmisión de la trama de 6 bytes del comando */
    SD_TransferByte(cmd | 0x40);          /* Byte de comando con bit de inicio */
    SD_TransferByte((uint8_t)(arg >> 24)); /* Argumento MSB */
    SD_TransferByte((uint8_t)(arg >> 16));
    SD_TransferByte((uint8_t)(arg >> 8));
    SD_TransferByte((uint8_t)(arg));       /* Argumento LSB */
    SD_TransferByte(crc);                  /* Byte de CRC y bit de parada */

    /* Bucle de espera de la respuesta R1 (el bit más significativo se pone a 0 al recibir respuesta válida) */
    n = 10;
    do {
        res = SD_TransferByte(0xFF);
    } while ((res & 0x80) && --n);

    return res;
}

/* Recibe un bloque de datos desde la tarjeta precedido por el token de inicio 0xFE, seguido de 512 bytes y 2 bytes de CRC */
static uint8_t SD_RxDataBlock(uint8_t *buf, uint16_t len)
{
    uint8_t token;
    uint32_t t0 = HAL_GetTick();

    /* Espera activa del token de datos válido (0xFE) con timeout de 200ms */
    do {
        token = SD_TransferByte(0xFF);
    } while (token == 0xFF && (HAL_GetTick() - t0) < 200);

    if (token != 0xFE) return 0;

    /*
     * Lectura eficiente en el mismo búfer: cada byte TX (0xFF) genera un ciclo de reloj
     * que desplaza el byte RX correspondiente directamente "in place".
     */
    memset(buf, 0xFF, len);
    if (HAL_SPI_TransmitReceive(sd_hspi, buf, buf, len, 1000) != HAL_OK) return 0;

    SD_TransferByte(0xFF);   /* Descarta el byte alto de CRC */
    SD_TransferByte(0xFF);   /* Descarta el byte bajo de CRC */
    return 1;
}

/* ============================================================================
 * API PÚBLICA DEL DRIVER
 * ============================================================================ */

/* Secuencia completa de inicialización y detección automática de tipo de tarjeta SD */
SD_Status_t SPI_SD_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    uint8_t ty = 0, cmd, res = 0xFF, ocr[4];
    uint32_t t0;

    sd_hspi    = hspi;
    sd_cs_port = cs_port;
    sd_cs_pin  = cs_pin;
    card_type  = 0;

    /* Fuerza el prescaler lento para garantizar una frecuencia de reloj segura (<400 kHz) */
    SD_SetPrescaler(SD_INIT_PRESCALER);
    HAL_GPIO_WritePin(sd_cs_port, sd_cs_pin, GPIO_PIN_SET);
    HAL_Delay(10);

    /* Envía al menos 74 ciclos de reloj (pulsos de 0xFF) con el pin CS en alto para despertar la tarjeta */
    for (int i = 0; i < 10; i++) SD_TransferByte(0xFF);

    /* Envía CMD0 (GO_IDLE_STATE) con reintentos para llevar la tarjeta al estado de reposo SPI */
    for (int attempt = 0; attempt < 10; attempt++) {
        res = SD_SendCmd(SD_CMD0, 0);
        if (res == 1) break;
        SD_Deselect();
        for (int k = 0; k < 10; k++) SD_TransferByte(0xFF);
        HAL_Delay(10);
    }
    SD_LOG("[SD] CMD0 -> 0x%02X\r\n", (unsigned)res);

    if (res == 1) {
        /* Envía CMD8 para verificar la condición de voltaje de interfaz (obligatorio para tarjetas SDv2) */
        res = SD_SendCmd(SD_CMD8, 0x1AA);
        SD_LOG("[SD] CMD8 -> 0x%02X\r\n", (unsigned)res);

        if (res == 1) {
            /* Tarjeta SD versión 2.0+: Lee los 4 bytes de respuesta R7 */
            for (int n = 0; n < 4; n++) ocr[n] = SD_TransferByte(0xFF);

            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
                t0 = HAL_GetTick();
                do {
                    /* Envía ACMD41 con HCS (High Capacity Support) habilitado */
                    res = SD_SendCmd(SD_ACMD41, 1UL << 30);
                } while (res != 0 && (HAL_GetTick() - t0) < 1000);
                SD_LOG("[SD] ACMD41 -> 0x%02X\r\n", (unsigned)res);

                /* Lee el registro OCR (CMD58) para determinar si es SDHC/SDXC (bloques) o SDSC estándar */
                if (res == 0 && SD_SendCmd(SD_CMD58, 0) == 0) {
                    for (int n = 0; n < 4; n++) ocr[n] = SD_TransferByte(0xFF);
                    ty = (ocr[0] & 0x40) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
                }
            }
        } else {
            /* Tarjeta antigua SDv1 o MMC (el CMD8 devuelve comando ilegal 0x04) */
            if (SD_SendCmd(SD_ACMD41, 0) <= 1) {
                ty = CT_SD1;
                cmd = SD_ACMD41;
            } else {
                ty = CT_MMC;
                cmd = SD_CMD1;
            }

            t0 = HAL_GetTick();
            while (SD_SendCmd(cmd, 0) != 0) {
                if ((HAL_GetTick() - t0) > 1000) { ty = 0; break; }
            }
            /* Configura el tamaño del bloque a 512 bytes mediante CMD16 */
            if (ty && SD_SendCmd(SD_CMD16, 512) != 0) ty = 0;
        }
    }

    SD_Deselect();
    SD_LOG("[SD] Tipo de tarjeta detectada: 0x%02X\r\n", (unsigned)ty);

    if (!ty) return SD_ERROR_INIT;

    card_type = ty;

    /* Con la tarjeta inicializada con éxito, conmutamos al prescaler rápido para máxima velocidad de lectura/escritura */
    SD_SetPrescaler(SD_RUN_PRESCALER);
    return SD_OK;
}

/* Lee un bloque de 512 bytes de la tarjeta SD en la dirección de sector lógica especificada */
SD_Status_t SPI_SD_ReadBlock(uint8_t *pBuffer, uint32_t sector)
{
    if (!card_type) return SD_ERROR_INIT;

    /* Si la tarjeta es SDSC o SDv1, el direccionamiento se realiza por byte exacto, no por sector LBA */
    if (!(card_type & CT_BLOCK)) sector <<= 9;   /* Multiplica por 512 equivalente a sector << 9 */

    if (SD_SendCmd(SD_CMD17, sector) != 0) {
        SD_Deselect();
        return SD_ERROR_COMMAND;
    }

    uint8_t ok = SD_RxDataBlock(pBuffer, 512);
    SD_Deselect();
    return ok ? SD_OK : SD_ERROR_TIMEOUT;
}

/* Escribe un bloque de 512 bytes hacia la tarjeta SD en la dirección de sector lógica especificada */
SD_Status_t SPI_SD_WriteBlock(const uint8_t *pBuffer, uint32_t sector)
{
    if (!card_type) return SD_ERROR_INIT;

    if (!(card_type & CT_BLOCK)) sector <<= 9;

    if (SD_SendCmd(SD_CMD24, sector) != 0) {
        SD_Deselect();
        return SD_ERROR_COMMAND;
    }

    SD_TransferByte(0xFF);   /* 1 byte de separación obligatorio antes del token de datos */
    SD_TransferByte(0xFE);   /* Token de inicio de bloque de datos (Data Token) */

    /* Transmisión por DMA/SPI del bloque completo de 512 bytes */
    if (HAL_SPI_Transmit(sd_hspi, (uint8_t *)pBuffer, 512, 1000) != HAL_OK) {
        SD_Deselect();
        return SD_ERROR_COMMAND;
    }

    SD_TransferByte(0xFF);   /* Envía CRC dummy alto */
    SD_TransferByte(0xFF);   /* Envía CRC dummy bajo */

    /* Lee la respuesta de datos devuelta por la tarjeta */
    uint8_t response = SD_TransferByte(0xFF);
    if ((response & 0x1F) != 0x05) { /* El código 0x05 indica que los datos fueron aceptados correctamente */
        SD_Deselect();
        return SD_ERROR_COMMAND;
    }

    /* Espera activa mientras la tarjeta procesa y programa internamente los datos en su memoria flash */
    if (!SD_WaitReady(500)) {
        SD_Deselect();
        return SD_ERROR_TIMEOUT;
    }

    SD_Deselect();
    return SD_OK;
}

/* Obtiene la capacidad total de almacenamiento de la tarjeta consultando el registro CSD (CMD9) */
SD_Status_t SPI_SD_GetSectorCount(uint32_t *sectors)
{
    uint8_t csd[16];

    if (!card_type) return SD_ERROR_INIT;

    if (SD_SendCmd(SD_CMD9, 0) != 0 || !SD_RxDataBlock(csd, 16)) {
        SD_Deselect();
        return SD_ERROR_COMMAND;
    }
    SD_Deselect();

    /* Decodificación del registro CSD en función de la versión (CSD v1 o CSD v2) */
    if ((csd[0] >> 6) == 1) {   /* CSD Versión 2.0 (Tarjetas de alta capacidad SDHC / SDXC) */
        uint32_t csize = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        *sectors = (csize + 1) << 10; /* Cada unidad C_SIZE representa 512 KB */
    } else {                    /* CSD Versión 1.0 (Tarjetas SD estándar de menor capacidad) */
        uint8_t  n     = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
        uint32_t csize = (csd[8] >> 6) + ((uint32_t)csd[7] << 2) + ((uint32_t)(csd[6] & 3) << 10) + 1;
        *sectors = csize << (n - 9);
    }
    return SD_OK;
}
