/**
 * @file bmp180_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación NO BLOQUEANTE del driver para el sensor BMP180/BMP085.
 * @details Emplea una máquina de estados cooperativa basada en temporización por ticks (HAL_GetTick()),
 *          desacoplando los tiempos de conversión del sensor y aplicando las fórmulas de compensación
 *          polinómica oficiales de Bosch.
 * @version 2.0
 * @date 2026
 */

#include <bmp180_STM32.h>
#include <math.h>

/* ============================================================================
 * DEFINICIONES Y MACROS PRIVADAS
 * ============================================================================ */
#define BMP180_REG_CHIPID       0xD0  /* Registro del Chip ID fijo (0x55) */
#define BMP180_REG_CONTROL      0xF4  /* Registro de control de conversiones */
#define BMP180_REG_TEMPDATA     0xF6  /* Registro de datos para temperatura */
#define BMP180_REG_PRESSUREDATA 0xF6  /* Registro de datos para presión */

#define BMP180_CMD_READ_TEMP    0x2E  /* Comando para iniciar lectura de temperatura */
#define BMP180_CMD_READ_PRESS   0x34  /* Comando base para iniciar lectura de presión */

#define BMP180_I2C_TIMEOUT_MS   10U   /* Timeout corto para transacciones I2C individuales */

/* ============================================================================
 * ESTADOS DE LA MÁQUINA DE ESTADOS INTERNA
 * ============================================================================ */
typedef enum {
    ST_BMP_UNINIT = 0,  /* Driver no inicializado o con error crítico */
    ST_BMP_IDLE,        /* Libre, esperando órdenes de medición */
    ST_BMP_WAIT_TEMP,   /* Esperando que transcurra el tiempo de conversión de temperatura (~4.5ms) */
    ST_BMP_WAIT_PRESS   /* Esperando que transcurra el tiempo de conversión de presión (según OSS) */
} BMP180_State_t;

/* ============================================================================
 * ESTRUCTURA PRIVADA DE CONTROL EXTENDIDA
 * ============================================================================ */
typedef struct {
    BMP180_State_t state;       /* Estado actual de la máquina de estados */
    uint32_t       t_start;     /* Marca de tiempo (tick) para control de tiempos de conversión */
    int32_t        ut;          /* Temperatura cruda leída (Uncompensated Temperature) */
    int32_t        up;          /* Presión cruda leída (Uncompensated Pressure) */
    float          last_temp;   /* Última temperatura compensada publicada (°C) */
    float          last_press;  /* Última presión compensada publicada (Pa) */
    bool           data_valid;  /* Bandera de disponibilidad de datos válidos */
} BMP180_Internal_t;

/* Instancia de control interna privada asociada al dispositivo */
static BMP180_Internal_t priv = {
    .state = ST_BMP_UNINIT,
    .t_start = 0,
    .ut = 0,
    .up = 0,
    .last_temp = 0.0f,
    .last_press = 0.0f,
    .data_valid = false
};

/* ============================================================================
 * FUNCIONES AUXILIARES PRIVADAS
 * ============================================================================ */

/* Comprueba si ya transcurrió un intervalo de tiempo de forma segura ante desbordamiento de ticks */
static bool elapsed(uint32_t ms) {
    return (HAL_GetTick() - priv.t_start) >= ms;
}

/* Realiza un reinicio por hardware/software del bus I2C ante bloqueos de comunicación */
static void recover_bus(BMP180_Handle_t *dev) {
    if (dev == NULL || dev->hi2c == NULL) return;
    if (HAL_I2C_GetError(dev->hi2c) == HAL_I2C_ERROR_AF) return; /* NACK puro: sensor ocupado/ausente */

    __HAL_I2C_DISABLE(dev->hi2c);
    dev->hi2c->Instance->CR1 |=  I2C_CR1_SWRST;
    dev->hi2c->Instance->CR1 &= ~I2C_CR1_SWRST;
    (void)HAL_I2C_Init(dev->hi2c);
}

/* Escribe un byte en un registro del BMP180 vía I2C de forma no bloqueante (timeout corto) */
static BMP180_Status_t BMP180_Write8(BMP180_Handle_t *dev, uint8_t reg, uint8_t value) {
    uint8_t data[2] = {reg, value};
    if (HAL_I2C_Master_Transmit(dev->hi2c, BMP180_I2C_ADDRESS, data, 2, BMP180_I2C_TIMEOUT_MS) != HAL_OK) {
        return BMP180_ERROR_I2C;
    }
    return BMP180_OK;
}

/* Lee un byte desde un registro del BMP180 vía I2C */
static BMP180_Status_t BMP180_Read8(BMP180_Handle_t *dev, uint8_t reg, uint8_t *value) {
    if (HAL_I2C_Master_Transmit(dev->hi2c, BMP180_I2C_ADDRESS, &reg, 1, BMP180_I2C_TIMEOUT_MS) != HAL_OK) {
        return BMP180_ERROR_I2C;
    }
    if (HAL_I2C_Master_Receive(dev->hi2c, BMP180_I2C_ADDRESS, value, 1, BMP180_I2C_TIMEOUT_MS) != HAL_OK) {
        return BMP180_ERROR_I2C;
    }
    return BMP180_OK;
}

/* Lee un valor de 16 bits sin signo (Big Endian) desde un registro del BMP180 vía I2C */
static BMP180_Status_t BMP180_Read16(BMP180_Handle_t *dev, uint8_t reg, uint16_t *value) {
    uint8_t buffer[2];
    if (HAL_I2C_Master_Transmit(dev->hi2c, BMP180_I2C_ADDRESS, &reg, 1, BMP180_I2C_TIMEOUT_MS) != HAL_OK) {
        return BMP180_ERROR_I2C;
    }
    if (HAL_I2C_Master_Receive(dev->hi2c, BMP180_I2C_ADDRESS, buffer, 2, BMP180_I2C_TIMEOUT_MS) != HAL_OK) {
        return BMP180_ERROR_I2C;
    }
    *value = (uint16_t)((buffer[0] << 8) | buffer[1]);
    return BMP180_OK;
}

/* Lee un valor de 16 bits con signo desde un registro del BMP180 */
static BMP180_Status_t BMP180_ReadS16(BMP180_Handle_t *dev, uint8_t reg, int16_t *value) {
    uint16_t temp;
    if (BMP180_Read16(dev, reg, &temp) != BMP180_OK) {
        return BMP180_ERROR_I2C;
    }
    *value = (int16_t)temp;
    return BMP180_OK;
}

/* Lee los 11 coeficientes de calibración de fábrica guardados en la EEPROM interna del sensor */
static BMP180_Status_t BMP180_ReadCoefficients(BMP180_Handle_t *dev) {
    if (BMP180_ReadS16(dev, 0xAA, &dev->calib.ac1) != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_ReadS16(dev, 0xAC, &dev->calib.ac2) != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_ReadS16(dev, 0xAE, &dev->calib.ac3) != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_Read16(dev,  0xB0, &dev->calib.ac4) != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_Read16(dev,  0xB2, &dev->calib.ac5) != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_Read16(dev,  0xB4, &dev->calib.ac6) != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_ReadS16(dev, 0xB6, &dev->calib.b1)  != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_ReadS16(dev, 0xB8, &dev->calib.b2)  != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_ReadS16(dev, 0xBA, &dev->calib.mb)  != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_ReadS16(dev, 0xBC, &dev->calib.mc)  != BMP180_OK) return BMP180_ERROR_I2C;
    if (BMP180_ReadS16(dev, 0xBE, &dev->calib.md)  != BMP180_OK) return BMP180_ERROR_I2C;
    return BMP180_OK;
}

/* Calcula el coeficiente B5 necesario para las ecuaciones de compensación de temperatura y presión */
static int32_t BMP180_ComputeB5(BMP180_Handle_t *dev, int32_t ut) {
    int32_t x1 = (ut - (int32_t)dev->calib.ac6) * ((int32_t)dev->calib.ac5) >> 15;
    int32_t x2 = ((int32_t)dev->calib.mc << 11) / (x1 + (int32_t)dev->calib.md);
    return x1 + x2;
}

/* Aplica las fórmulas de compensación matemática de Bosch para obtener T y P definitivas */
static void BMP180_CalculateResults(BMP180_Handle_t *dev, float *out_temp, float *out_press) {
    int32_t b5 = BMP180_ComputeB5(dev, priv.ut);

    /* Cálculo de la temperatura compensada en °C */
    *out_temp = ((b5 + 8) >> 4) / 10.0f;

    /* Cálculo de la presión atmosférica compensada en Pascales (Pa) */
    int32_t b6, x1, x2, x3, b3, p;
    uint32_t b4, b7;

    b6 = b5 - 4000;
    x1 = ((int32_t)dev->calib.b2 * ((b6 * b6) >> 12)) >> 11;
    x2 = ((int32_t)dev->calib.ac2 * b6) >> 11;
    x3 = x1 + x2;
    b3 = (((((int32_t)dev->calib.ac1) * 4 + x3) << dev->mode) + 2) >> 2;

    x1 = ((int32_t)dev->calib.ac3 * b6) >> 13;
    x2 = ((int32_t)dev->calib.b1 * ((b6 * b6) >> 12)) >> 16;
    x3 = ((x1 + x2) + 2) >> 2;
    b4 = ((uint32_t)dev->calib.ac4 * (uint32_t)(x3 + 32768)) >> 15;

    b7 = ((uint32_t)priv.up - (uint32_t)b3) * (uint32_t)(50000 >> dev->mode);
    if (b7 < 0x80000000) {
        p = (b7 << 1) / b4;
    } else {
        p = (b7 / b4) << 1;
    }

    x1 = (p >> 8) * (p >> 8);
    x1 = (x1 * 3038) >> 16;
    x2 = (-7357 * p) >> 16;
    p = p + ((x1 + x2 + 3791) >> 4);

    *out_press = (float)p;
}

/* ============================================================================
 * IMPLEMENTACIÓN DE LA API PÚBLICA
 * ============================================================================ */

/* Inicializa el sensor BMP180, valida el bus I2C, verifica el Chip ID y lee los coeficientes de calibración */
BMP180_Status_t BMP180_Init(BMP180_Handle_t *dev, I2C_HandleTypeDef *hi2c, bmp180_mode_t mode) {
    if (dev == NULL || hi2c == NULL) return BMP180_ERROR_I2C;

    dev->hi2c = hi2c;
    dev->mode = (mode > BMP180_MODE_ULTRAHIGHRES) ? BMP180_MODE_ULTRAHIGHRES : mode;

    priv.data_valid = false;

    /* Validar Chip ID (el BMP180 responde fijamente con 0x55) */
    uint8_t chipid = 0;
    if (BMP180_Read8(dev, BMP180_REG_CHIPID, &chipid) != BMP180_OK) {
        priv.state = ST_BMP_UNINIT;
        return BMP180_ERROR_I2C;
    }
    if (chipid != 0x55) {
        priv.state = ST_BMP_UNINIT;
        return BMP180_ERROR_CHIPID;
    }

    /* Cargar coeficientes de calibración de fábrica desde la EEPROM del sensor */
    if (BMP180_ReadCoefficients(dev) != BMP180_OK) {
        priv.state = ST_BMP_UNINIT;
        return BMP180_ERROR_I2C;
    }

    priv.state = ST_BMP_IDLE;
    return BMP180_OK;
}

/* Inicia un nuevo ciclo de medición asíncrona de forma no bloqueante */
BMP180_Status_t BMP180_StartMeasurement(BMP180_Handle_t *dev) {
    if (dev == NULL || priv.state != ST_BMP_IDLE) {
        return BMP180_BUSY;
    }

    /* Paso 1: Disparar la conversión de temperatura (primer paso obligatorio especificado por Bosch) */
    if (BMP180_Write8(dev, BMP180_REG_CONTROL, BMP180_CMD_READ_TEMP) != BMP180_OK) {
        recover_bus(dev);
        return BMP180_ERROR_I2C;
    }

    priv.t_start = HAL_GetTick();
    priv.state = ST_BMP_WAIT_TEMP;
    return BMP180_OK;
}

/* Motor principal de la máquina de estados cooperativa del driver. Debe llamarse en el bucle principal */
BMP180_Status_t BMP180_Process(BMP180_Handle_t *dev) {
    if (dev == NULL) return BMP180_ERROR_I2C;

    switch (priv.state) {
        case ST_BMP_UNINIT:
        case ST_BMP_IDLE:
            return BMP180_OK;

        case ST_BMP_WAIT_TEMP:
        {
            /* El BMP180 requiere un tiempo máximo de conversión de temperatura de 4.5 ms */
            if (!elapsed(5)) {
                return BMP180_BUSY; /* Aún convirtiendo: devuelve el control inmediato al bucle principal */
            }

            uint16_t temp_raw;
            if (BMP180_Read16(dev, BMP180_REG_TEMPDATA, &temp_raw) != BMP180_OK) {
                recover_bus(dev);
                priv.state = ST_BMP_IDLE;
                return BMP180_ERROR_I2C;
            }
            priv.ut = (int32_t)temp_raw;

            /* Paso 2: Encadenar el disparo de conversión de presión según el modo OSS configurado */
            uint8_t cmd_press = BMP180_CMD_READ_PRESS + (dev->mode << 6);
            if (BMP180_Write8(dev, BMP180_REG_CONTROL, cmd_press) != BMP180_OK) {
                recover_bus(dev);
                priv.state = ST_BMP_IDLE;
                return BMP180_ERROR_I2C;
            }

            priv.t_start = HAL_GetTick();
            priv.state = ST_BMP_WAIT_PRESS;
            return BMP180_BUSY;
        }

        case ST_BMP_WAIT_PRESS:
        {
            /* Determinar el tiempo de espera según el modo de sobremuestreo (OSS) seleccionado */
            uint32_t required_delay = 5;  /* Ultra Low Power (4.5 ms máx) */
            if (dev->mode == BMP180_MODE_STANDARD)      required_delay = 8;  /* Standard (7.5 ms máx) */
            else if (dev->mode == BMP180_MODE_HIGHRES)       required_delay = 14; /* High Resolution (13.5 ms máx) */
            else if (dev->mode == BMP180_MODE_ULTRAHIGHRES)  required_delay = 26; /* Ultra High Resolution (25.5 ms máx) */

            if (!elapsed(required_delay)) {
                return BMP180_BUSY; /* Aún convirtiendo presión: no bloquea el microcontrolador */
            }

            uint16_t p16;
            uint8_t p8;
            int32_t p32;

            if (BMP180_Read16(dev, BMP180_REG_PRESSUREDATA, &p16) != BMP180_OK) {
                recover_bus(dev);
                priv.state = ST_BMP_IDLE;
                return BMP180_ERROR_I2C;
            }
            p32 = (uint32_t)p16 << 8;

            if (BMP180_Read8(dev, BMP180_REG_PRESSUREDATA + 2, &p8) != BMP180_OK) {
                recover_bus(dev);
                priv.state = ST_BMP_IDLE;
                return BMP180_ERROR_I2C;
            }
            p32 += p8;
            p32 >>= (8 - dev->mode); /* Ajuste de desplazamiento de bits según el sobremuestreo */
            priv.up = p32;

            /* Paso 3: Calcular resultados matemáticos finales compensados (T y P) */
            BMP180_CalculateResults(dev, &priv.last_temp, &priv.last_press);
            priv.data_valid = true;

            priv.state = ST_BMP_IDLE;
            return BMP180_DATA_READY; /* Ciclo finalizado con éxito */
        }

        default:
            priv.state = ST_BMP_IDLE;
            return BMP180_OK;
    }
}

/* Consulta si el driver se encuentra libre (en estado IDLE) */
bool BMP180_IsIdle(BMP180_Handle_t *dev) {
    (void)dev;
    return (priv.state == ST_BMP_IDLE);
}

/* Copia los últimos valores de temperatura y presión compensados hacia las variables del usuario */
bool BMP180_GetData(BMP180_Handle_t *dev, float *temperature, float *pressure) {
    (void)dev;
    if (!priv.data_valid) return false;

    if (temperature != NULL) *temperature = priv.last_temp;
    if (pressure    != NULL) *pressure    = priv.last_press;
    return true;
}

/* Calcula la altitud estimada en metros a partir de la presión atmosférica y la presión de referencia a nivel del mar */
float BMP180_PressureToAltitude(float seaLevel, float atmospheric) {
    /* Ecuación barométrica internacional estipulada en el datasheet del BMP180 */
    return 44330.0f * (1.0f - powf(atmospheric / seaLevel, 0.1903f));
}
