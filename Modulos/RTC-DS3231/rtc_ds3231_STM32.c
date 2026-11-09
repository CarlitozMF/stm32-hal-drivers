/**
 * @file rtc_ds3231_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación del driver modular para el RTC DS3231 utilizando la HAL de STM32.
 * @details Contiene la lógica de comunicación I2C por bloques secuenciales, conversiones BCD/Decimal,
 *          lectura de temperatura interna compensada, gestión de la bandera de oscilador detenido (OSF)
 *          y configuración completa de la Alarma 1.
 * @version 1.2
 * @date 2026
 */

#include "rtc_ds3231_STM32.h"

/* ============================================================================
 * CONSTANTES Y DIRECCIONES PRIVADAS DE REGISTROS
 * ============================================================================ */
#define DS3231_I2C_ADDRESS     0xD0   /* Dirección I2C del DS3231 (desplazada 1 bit a la izquierda para la HAL) */

/* Direcciones base de los registros internos del DS3231 */
#define DS3231_REG_SECONDS     0x00   /* Registro de segundos */
#define DS3231_REG_ALARM1_SEC  0x07   /* Registro inicial de la Alarma 1 (Segundos a Día/Fecha) */
#define DS3231_REG_CONTROL     0x0E   /* Registro de Control (Habilitación de alarmas e interrupciones) */
#define DS3231_REG_STATUS      0x0F   /* Registro de Estado (A1F, A2F, BSY, EN32kHz, OSF) */
#define DS3231_REG_TEMP_MSB    0x11   /* Registro de Temperatura MSB */

/* Bits del registro de estado (0x0F) */
#define DS3231_STATUS_OSF      0x80   /* Oscillator Stop Flag */
#define DS3231_STATUS_A2F      0x02   /* Alarm 2 Flag */
#define DS3231_STATUS_A1F      0x01   /* Alarm 1 Flag */

/* Bits del registro de control (0x0E) */
#define DS3231_CTRL_INTCN      0x04   /* Interrupt Control: 1 = pin INT/SQW como salida de alarma */
#define DS3231_CTRL_A1IE       0x01   /* Alarm 1 Interrupt Enable */

/* Bit de máscara de los registros de alarma */
#define DS3231_ALARM_MASK_BIT  0x80

/* Puntero estático al manejador del periférico I2C asignado durante el arranque */
static I2C_HandleTypeDef* s_hi2c = NULL;

/* ============================================================================
 * FUNCIONES AUXILIARES PRIVADAS
 * ============================================================================ */

/* Convierte un byte en formato BCD (Binary-Coded Decimal) a un número entero decimal estándar */
static uint8_t bcd_to_dec(uint8_t bcd) {
    return (uint8_t)((bcd >> 4) * 10U + (bcd & 0x0FU));
}

/* Convierte un número decimal estándar a formato BCD para almacenarlo en los registros del RTC */
static uint8_t dec_to_bcd(uint8_t dec) {
    return (uint8_t)(((dec / 10U) << 4) | (dec % 10U));
}

/* Lectura de 'len' registros consecutivos a partir de 'reg' */
static DS3231_Status ds3231_read(uint8_t reg, uint8_t *buf, uint16_t len) {
    if (HAL_I2C_Mem_Read(s_hi2c, DS3231_I2C_ADDRESS, reg, I2C_MEMADD_SIZE_8BIT,
                         buf, len, DS3231_I2C_TIMEOUT_MS) != HAL_OK) {
        return DS3231_ERROR;
    }
    return DS3231_OK;
}

/* Escritura de 'len' registros consecutivos a partir de 'reg' */
static DS3231_Status ds3231_write(uint8_t reg, uint8_t *buf, uint16_t len) {
    if (HAL_I2C_Mem_Write(s_hi2c, DS3231_I2C_ADDRESS, reg, I2C_MEMADD_SIZE_8BIT,
                          buf, len, DS3231_I2C_TIMEOUT_MS) != HAL_OK) {
        return DS3231_ERROR;
    }
    return DS3231_OK;
}

/* Validación de rangos de una estructura de tiempo completa */
static bool time_is_valid(const DS3231_Time *t) {
    return (t->seconds      <= 59U) &&
           (t->minutes      <= 59U) &&
           (t->hours        <= 23U) &&
           (t->day_of_week  >= 1U && t->day_of_week  <= 7U)  &&
           (t->day_of_month >= 1U && t->day_of_month <= 31U) &&
           (t->month        >= 1U && t->month        <= 12U) &&
           (t->year         <= 99U);
}

/*
 * Limpia en el registro de estado la(s) bandera(s) indicada(s) en 'flags_to_clear'.
 * En el DS3231 las banderas solo se pueden escribir a 0; escribir 1 las deja intactas.
 * Por eso se ponen en 1 todas las banderas que NO se quieren limpiar: así, si A2F/A1F/OSF
 * se activa entre la lectura y la escritura, no se pierde. El resto de bits (EN32kHz) se
 * preserva con el valor leído.
 */
static DS3231_Status clear_status_flags(uint8_t flags_to_clear) {
    uint8_t status = 0;
    DS3231_Status ret = ds3231_read(DS3231_REG_STATUS, &status, 1);
    if (ret != DS3231_OK) {
        return ret;
    }

    status |= (DS3231_STATUS_OSF | DS3231_STATUS_A2F | DS3231_STATUS_A1F);
    status &= (uint8_t)~flags_to_clear;

    return ds3231_write(DS3231_REG_STATUS, &status, 1);
}

/* ============================================================================
 * FUNCIONES PÚBLICAS DEL DRIVER
 * ============================================================================ */

/* Inicializa el driver del DS3231 asociando el bus I2C correspondiente */
DS3231_Status DS3231_Init(I2C_HandleTypeDef *hi2c) {
    if (hi2c == NULL) {
        return DS3231_ERR_INVALID_PARAM;
    }
    s_hi2c = hi2c;
    return DS3231_OK;
}

/* Escribe la hora y fecha actual en el módulo DS3231 */
DS3231_Status DS3231_WriteTime(const DS3231_Time *time) {
    if (time == NULL || s_hi2c == NULL || !time_is_valid(time)) {
        return DS3231_ERR_INVALID_PARAM;
    }

    uint8_t data[7];

    data[0] = dec_to_bcd(time->seconds);
    data[1] = dec_to_bcd(time->minutes);
    data[2] = dec_to_bcd(time->hours) & 0x3F;   /* Bit 6 = 0: formato 24 h */
    data[3] = dec_to_bcd(time->day_of_week);
    data[4] = dec_to_bcd(time->day_of_month);
    data[5] = dec_to_bcd(time->month);          /* Bit 7 (siglo) = 0 */
    data[6] = dec_to_bcd(time->year);

    /* Escritura secuencial en bloque de los 7 registros de tiempo a partir del registro 0x00 */
    DS3231_Status ret = ds3231_write(DS3231_REG_SECONDS, data, 7);
    if (ret != DS3231_OK) {
        return ret;
    }

    /* La hora ya es válida: se limpia la bandera OSF para indicar que el reloj fue reajustado */
    return clear_status_flags(DS3231_STATUS_OSF);
}

/* Lee la hora y fecha actual desde el módulo DS3231 */
DS3231_Status DS3231_ReadTime(DS3231_Time *time) {
    if (time == NULL || s_hi2c == NULL) {
        return DS3231_ERR_INVALID_PARAM;
    }

    uint8_t data[7];

    /* Lectura secuencial en bloque de los 7 registros de tiempo a partir del registro 0x00 */
    DS3231_Status ret = ds3231_read(DS3231_REG_SECONDS, data, 7);
    if (ret != DS3231_OK) {
        return ret;
    }

    time->seconds = bcd_to_dec(data[0] & 0x7F);
    time->minutes = bcd_to_dec(data[1] & 0x7F);

    /* Horas: soporta tanto formato 24 h como 12 h (bit 6 = 1) y devuelve siempre 24 h */
    if (data[2] & 0x40) {
        uint8_t h = bcd_to_dec(data[2] & 0x1F);   /* 1-12 */
        bool pm   = (data[2] & 0x20) != 0;
        if (h == 12U) {
            h = 0;                                /* 12 AM = 00, 12 PM = 12 */
        }
        time->hours = pm ? (uint8_t)(h + 12U) : h;
    } else {
        time->hours = bcd_to_dec(data[2] & 0x3F);
    }

    time->day_of_week  = bcd_to_dec(data[3] & 0x07);
    time->day_of_month = bcd_to_dec(data[4] & 0x3F);
    time->month        = bcd_to_dec(data[5] & 0x1F);   /* Se descarta el bit de siglo (bit 7) */
    time->year         = bcd_to_dec(data[6]);

    return DS3231_OK;
}

/* Lee la temperatura interna del sensor del DS3231 */
DS3231_Status DS3231_ReadTemperature(float *temp) {
    if (temp == NULL || s_hi2c == NULL) {
        return DS3231_ERR_INVALID_PARAM;
    }

    uint8_t data[2];

    /* Lectura de los registros de temperatura (MSB en 0x11 y LSB en 0x12) */
    DS3231_Status ret = ds3231_read(DS3231_REG_TEMP_MSB, data, 2);
    if (ret != DS3231_OK) {
        return ret;
    }

    /*
     * El formato es de 10 bits con signo. Los 8 bits enteros están en data[0]
     * y los 2 bits fraccionarios están en los bits [7:6] de data[1].
     * Desplazamos 6 posiciones a la derecha (aritmético) para alinear el valor con signo.
     */
    int16_t raw_temp = (int16_t)(((uint16_t)data[0] << 8) | data[1]) >> 6;

    /* Cada incremento del valor crudo representa exactamente 0.25 °C */
    *temp = (float)raw_temp * 0.25f;

    return DS3231_OK;
}

/* Verifica si el oscilador se detuvo (Bandera OSF, bit 7 del registro de estado) */
DS3231_Status DS3231_CheckClockHalted(bool *is_halted) {
    if (is_halted == NULL || s_hi2c == NULL) {
        return DS3231_ERR_INVALID_PARAM;
    }

    uint8_t status = 0;
    DS3231_Status ret = ds3231_read(DS3231_REG_STATUS, &status, 1);
    if (ret != DS3231_OK) {
        return ret;
    }

    /*
     * OSF = 1 indica que el oscilador estuvo detenido (pérdida de Vcc y de batería,
     * arranque inicial, etc.) y que la hora almacenada puede no ser confiable.
     */
    *is_halted = (status & DS3231_STATUS_OSF) != 0;

    return DS3231_OK;
}

/* Configura y habilita la Alarma 1 del DS3231 con su respectivo modo de coincidencia */
DS3231_Status DS3231_SetAlarm1(const DS3231_Time *alarm_time, DS3231_Alarm1_Mode mode) {
    if (alarm_time == NULL || s_hi2c == NULL) {
        return DS3231_ERR_INVALID_PARAM;
    }

    uint8_t data[4];

    /* Configuración de las máscaras de coincidencia (Bits A1M1 a A1M4 en los registros de alarma) */
    switch (mode) {
        case DS3231_A1_EVERY_SECOND:
            data[0] = DS3231_ALARM_MASK_BIT; /* A1M1 = 1 (Ignorar segundos) */
            data[1] = DS3231_ALARM_MASK_BIT; /* A1M2 = 1 (Ignorar minutos) */
            data[2] = DS3231_ALARM_MASK_BIT; /* A1M3 = 1 (Ignorar horas) */
            data[3] = DS3231_ALARM_MASK_BIT; /* A1M4 = 1 (Ignorar día/fecha) */
            break;

        case DS3231_A1_MATCH_S:
            if (alarm_time->seconds > 59U) {
                return DS3231_ERR_INVALID_PARAM;
            }
            data[0] = dec_to_bcd(alarm_time->seconds) & 0x7F; /* A1M1 = 0 (Coincidir segundos) */
            data[1] = DS3231_ALARM_MASK_BIT;
            data[2] = DS3231_ALARM_MASK_BIT;
            data[3] = DS3231_ALARM_MASK_BIT;
            break;

        case DS3231_A1_MATCH_MS:
            if (alarm_time->seconds > 59U || alarm_time->minutes > 59U) {
                return DS3231_ERR_INVALID_PARAM;
            }
            data[0] = dec_to_bcd(alarm_time->seconds) & 0x7F;
            data[1] = dec_to_bcd(alarm_time->minutes) & 0x7F; /* A1M2 = 0 (Coincidir minutos y segundos) */
            data[2] = DS3231_ALARM_MASK_BIT;
            data[3] = DS3231_ALARM_MASK_BIT;
            break;

        case DS3231_A1_MATCH_HMS:
            if (alarm_time->seconds > 59U || alarm_time->minutes > 59U ||
                alarm_time->hours > 23U) {
                return DS3231_ERR_INVALID_PARAM;
            }
            data[0] = dec_to_bcd(alarm_time->seconds) & 0x7F;
            data[1] = dec_to_bcd(alarm_time->minutes) & 0x7F;
            data[2] = dec_to_bcd(alarm_time->hours)   & 0x3F; /* A1M3 = 0, bit 6 = 0 (24 h) */
            data[3] = DS3231_ALARM_MASK_BIT;
            break;

        case DS3231_A1_MATCH_DHMS:
            if (alarm_time->seconds > 59U || alarm_time->minutes > 59U ||
                alarm_time->hours > 23U ||
                alarm_time->day_of_month < 1U || alarm_time->day_of_month > 31U) {
                return DS3231_ERR_INVALID_PARAM;
            }
            data[0] = dec_to_bcd(alarm_time->seconds)      & 0x7F;
            data[1] = dec_to_bcd(alarm_time->minutes)      & 0x7F;
            data[2] = dec_to_bcd(alarm_time->hours)        & 0x3F;
            data[3] = dec_to_bcd(alarm_time->day_of_month) & 0x3F; /* A1M4 = 0, DY/DT = 0 (fecha) */
            break;

        default:
            return DS3231_ERR_INVALID_PARAM;
    }

    /* Escritura en bloque de los 4 registros de la Alarma 1 (desde 0x07 hasta 0x0A) */
    DS3231_Status ret = ds3231_write(DS3231_REG_ALARM1_SEC, data, 4);
    if (ret != DS3231_OK) {
        return ret;
    }

    /* Se limpia una posible A1F pendiente para que no dispare la interrupción apenas se habilite */
    ret = DS3231_ClearAlarm1Flag();
    if (ret != DS3231_OK) {
        return ret;
    }

    /* Lectura previa del Registro de Control (0x0E) para no alterar otros bits configurados */
    uint8_t control_reg = 0;
    ret = ds3231_read(DS3231_REG_CONTROL, &control_reg, 1);
    if (ret != DS3231_OK) {
        return ret;
    }

    /*
     * Modificación de bits de control:
     * - Bit 2 (INTCN = 1): Habilita el modo de interrupción por hardware en el pin INT/SQW.
     * - Bit 0 (A1IE = 1): Habilita la interrupción por coincidencia de la Alarma 1.
     */
    control_reg |= (DS3231_CTRL_INTCN | DS3231_CTRL_A1IE);

    /* Escritura del nuevo valor en el Registro de Control */
    return ds3231_write(DS3231_REG_CONTROL, &control_reg, 1);
}

/* Limpia la bandera de disparo de la Alarma 1 (A1F) en el registro de estado */
DS3231_Status DS3231_ClearAlarm1Flag(void) {
    if (s_hi2c == NULL) {
        return DS3231_ERR_INVALID_PARAM;
    }

    /* Solo A1F pasa a 0; OSF y A2F se preservan (ver clear_status_flags) */
    return clear_status_flags(DS3231_STATUS_A1F);
}
