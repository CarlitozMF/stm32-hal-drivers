/**
 * @file bh1750_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación del driver genérico para el sensor de luz digital BH1750FVI.
 * @details Contiene la lógica de negocio, mapeo de estados, control de tiempos de integración (MTreg),
 *          conversión a luxes y autoajuste dinámico utilizando la capa HAL de STM32.
 * @version 1.3
 * @date 2026
 */

#include <bh1750_STM32.h>

/* Inicializa la estructura del manejador del sensor y aplica la configuración inicial */
BH1750_Status_t BH1750_Init(BH1750_Handle_t *dev, void *hi2c, uint8_t address, BH1750_Mode_t mode) {
    if (dev == NULL || hi2c == NULL) {
        return BH1750_ERR_INVALID_PARAM;
    }

    dev->hi2c = hi2c;
    dev->address = address;
    dev->mode = BH1750_UNCONFIGURED;
    dev->mtreg = BH1750_DEFAULT_MTREG;
    dev->conv_factor = 1.2f; /* Factor típico de corrección óptica según hoja de datos */
    dev->last_read_timestamp = 0;

    /* Configurar el modo de operación inicial enviando la instrucción I2C correspondiente */
    if (BH1750_Configure(dev, mode) != BH1750_OK) {
        return BH1750_ERR_I2C_TRANSMIT;
    }

    /* Establecer el valor por defecto del registro de tiempo de medición (MTreg = 69) */
    if (BH1750_SetMTreg(dev, BH1750_DEFAULT_MTREG) != BH1750_OK) {
        return BH1750_ERR_I2C_TRANSMIT;
    }

    return BH1750_OK;
}

/* Configura el modo de medición del sensor BH1750 enviando el comando I2C correspondiente */
BH1750_Status_t BH1750_Configure(BH1750_Handle_t *dev, BH1750_Mode_t mode) {
    uint8_t cmd = (uint8_t)mode;
    I2C_HandleTypeDef *i2c_handle = (I2C_HandleTypeDef *)(dev->hi2c);

    /* Transmisión del comando con desplazamiento de 1 bit para cumplir la especificación de dirección de la HAL */
    if (HAL_I2C_Master_Transmit(i2c_handle, (dev->address << 1), &cmd, 1, HAL_MAX_DELAY) != HAL_OK) {
        return BH1750_ERR_I2C_TRANSMIT;
    }

    dev->mode = mode;
    dev->last_read_timestamp = HAL_GetTick();

    /* Pausa de estabilización requerida por el hardware tras un cambio de modo (10 ms) */
    HAL_Delay(10);

    return BH1750_OK;
}

/* Configura el registro de tiempo de medición (MTreg) para ajustar la sensibilidad del sensor */
BH1750_Status_t BH1750_SetMTreg(BH1750_Handle_t *dev, uint8_t mtreg) {
    if (mtreg < BH1750_MTREG_MIN || mtreg > BH1750_MTREG_MAX) {
        return BH1750_ERR_OUT_OF_RANGE;
    }

    uint8_t data[1];
    I2C_HandleTypeDef *i2c_handle = (I2C_HandleTypeDef *)(dev->hi2c);

    /* 1. Envío de los bits superiores de MTreg: patrón 01000xxx */
    data[0] = (0b01000 << 3) | (mtreg >> 5);
    if (HAL_I2C_Master_Transmit(i2c_handle, (dev->address << 1), data, 1, HAL_MAX_DELAY) != HAL_OK) {
        return BH1750_ERR_I2C_TRANSMIT;
    }

    /* 2. Envío de los bits inferiores de MTreg: patrón 011xxxxx */
    data[0] = (0b011 << 5) | (mtreg & 0b11111);
    if (HAL_I2C_Master_Transmit(i2c_handle, (dev->address << 1), data, 1, HAL_MAX_DELAY) != HAL_OK) {
        return BH1750_ERR_I2C_TRANSMIT;
    }

    /* 3. Re-emisión de la instrucción del modo de operación activo para confirmar el nuevo valor */
    data[0] = (uint8_t)dev->mode;
    if (HAL_I2C_Master_Transmit(i2c_handle, (dev->address << 1), data, 1, HAL_MAX_DELAY) != HAL_OK) {
        return BH1750_ERR_I2C_TRANSMIT;
    }

    dev->mtreg = mtreg;
    HAL_Delay(10);

    return BH1750_OK;
}

/* Verifica de forma no bloqueante si ha transcurrido el tiempo necesario para leer una nueva muestra */
bool BH1750_IsMeasurementReady(BH1750_Handle_t *dev, bool max_wait) {
    uint32_t delay_time = 0;

    /* Cálculo escalado del tiempo de integración según el valor de MTreg en relación al valor por defecto */
    switch (dev->mode) {
        case BH1750_CONTINUOUS_HIGH_RES_MODE:
        case BH1750_CONTINUOUS_HIGH_RES_MODE_2:
        case BH1750_ONE_TIME_HIGH_RES_MODE:
        case BH1750_ONE_TIME_HIGH_RES_MODE_2:
            delay_time = max_wait ? (180 * dev->mtreg / BH1750_DEFAULT_MTREG)
                                  : (120 * dev->mtreg / BH1750_DEFAULT_MTREG);
            break;
        case BH1750_CONTINUOUS_LOW_RES_MODE:
        case BH1750_ONE_TIME_LOW_RES_MODE:
            delay_time = max_wait ? (24 * dev->mtreg / BH1750_DEFAULT_MTREG)
                                  : (16 * dev->mtreg / BH1750_DEFAULT_MTREG);
            break;
        default:
            return false;
    }

    return (HAL_GetTick() - dev->last_read_timestamp) >= delay_time;
}

/* Lee los datos del sensor por I2C y calcula el nivel de iluminación actual en Lux */
BH1750_Status_t BH1750_ReadLightLevel(BH1750_Handle_t *dev, float *lux_level) {
    if (dev->mode == BH1750_UNCONFIGURED || lux_level == NULL) {
        return BH1750_ERR_NOT_CONFIGURED;
    }

    uint8_t rx_buffer[2];
    I2C_HandleTypeDef *i2c_handle = (I2C_HandleTypeDef *)(dev->hi2c);

    /* Recepción de 2 bytes (High Byte + Low Byte) vía I2C */
    if (HAL_I2C_Master_Receive(i2c_handle, (dev->address << 1), rx_buffer, 2, HAL_MAX_DELAY) != HAL_OK) {
        return BH1750_ERR_I2C_RECEIVE;
    }

    dev->last_read_timestamp = HAL_GetTick();

    /* Reconstrucción del entero de 16 bits */
    uint16_t raw_value = (rx_buffer[0] << 8) | rx_buffer[1];
    float level = (float)raw_value;

    /* Escalamiento si el registro MTreg fue modificado respecto a los 69 por defecto */
    if (dev->mtreg != BH1750_DEFAULT_MTREG) {
        level *= ((float)BH1750_DEFAULT_MTREG / (float)dev->mtreg);
    }

    /* Corrección para modos de Alta Resolución 2 (sensibilidad duplicada a 0.5 Lux) */
    if (dev->mode == BH1750_ONE_TIME_HIGH_RES_MODE_2 || dev->mode == BH1750_CONTINUOUS_HIGH_RES_MODE_2) {
        level /= 2.0f;
    }

    /* Conversión a Lux dividiendo por el factor de calibración de 1.2 especificado en la hoja de datos */
    level /= dev->conv_factor;
    *lux_level = level;

    return BH1750_OK;
}

/* Ajusta automáticamente el registro MTreg en tiempo de ejecución en función de la lectura actual */
bool BH1750_AutoAdjust(BH1750_Handle_t *dev, float current_lux) {
    uint8_t new_mtreg = dev->mtreg;

    if (current_lux >= BH1750_HIGH_LIGHT_THRESHOLD) {
        /* Ambiente con alta intensidad de luz: se reduce el MTreg para atenuar la sensibilidad */
        if (dev->mtreg > BH1750_MTREG_MIN + 20) {
            new_mtreg -= 20;
        } else {
            new_mtreg = BH1750_MTREG_MIN;
        }
    }
    else if (current_lux <= BH1750_LOW_LIGHT_THRESHOLD) {
        /* Ambiente con baja iluminación: se incrementa el MTreg para aumentar la sensibilidad */
        if (dev->mtreg < BH1750_MTREG_MAX - 20) {
            new_mtreg += 20;
        } else {
            new_mtreg = BH1750_MTREG_MAX;
        }
    } else {
        /* Lectura dentro del rango nominal de trabajo */
        return false;
    }

    /* Transmitir actualización si el valor resultante difiere del estado actual */
    if (new_mtreg != dev->mtreg) {
        if (BH1750_SetMTreg(dev, new_mtreg) == BH1750_OK) {
            return true;
        }
    }

    return false;
}
