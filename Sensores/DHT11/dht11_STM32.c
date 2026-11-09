/**
 * @file dht11_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación del driver DHT11 adaptado al estándar de arquitectura de manejadores.
 * @details Contiene la lógica de bajo nivel del protocolo de un hilo (single-wire), retardos de precisión por SysTick y control estricto de timeouts para evitar bloqueos del sistema.
 * @version 2.1
 * @date 2026
 */

#include <dht11_STM32.h>

/* ========================================================================= */
/* FUNCIONES AUXILIARES PRIVADAS                                             */
/* ========================================================================= */

/* Retardo interno en microsegundos usando el contador de ciclos del SysTick del núcleo ARM Cortex-M */
static void DHT11_Delay_us(uint16_t us) {
    uint32_t ticks_per_us = SystemCoreClock / 1000000U;
    uint32_t num_ticks = (uint32_t)us * ticks_per_us;

    uint32_t start = SysTick->VAL;
    uint32_t elapsed = 0;

    while (elapsed < num_ticks) {
        uint32_t current = SysTick->VAL;
        if (current <= start) {
            elapsed += (start - current);
        } else {
            elapsed += (start + (SysTick->LOAD - current));
        }
        start = current;
    }
}

/* Configura el pin GPIO correspondiente como salida digital Push-Pull */
static void DHT11_SetPinOutput(GPIO_TypeDef *port, uint16_t pin) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &GPIO_InitStruct);
}

/* Configura el pin GPIO correspondiente como entrada digital flotante para escuchar la línea */
static void DHT11_SetPinInput(GPIO_TypeDef *port, uint16_t pin) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(port, &GPIO_InitStruct);
}

/* Envía la señal de inicio (pulso bajo de 18ms) y valida los pulsos de respuesta (ACK) del sensor */
static DHT11_Status_t DHT11_SendStartSignal(DHT11_Handle_t *dev) {
    uint32_t timeout;

    /* 1. Pulso de inicio: forzar la línea a LOW durante 18 ms */
    DHT11_SetPinOutput(dev->gpio_port, dev->gpio_pin);
    HAL_GPIO_WritePin(dev->gpio_port, dev->gpio_pin, GPIO_PIN_RESET);
    HAL_Delay(18);

    /* 2. Mantener línea en HIGH durante 20 us */
    HAL_GPIO_WritePin(dev->gpio_port, dev->gpio_pin, GPIO_PIN_SET);
    DHT11_Delay_us(20);

    /* 3. Liberar línea y pasar a modo entrada para recibir la respuesta */
    DHT11_SetPinInput(dev->gpio_port, dev->gpio_pin);

    /* 4. Esperar la caída a LOW del pulso de respuesta del sensor (80 us LOW) con timeout */
    timeout = 100;
    while (HAL_GPIO_ReadPin(dev->gpio_port, dev->gpio_pin) == GPIO_PIN_SET) {
        DHT11_Delay_us(1);
        if (--timeout == 0) return DHT11_ERR_TIMEOUT;
    }

    /* 5. Esperar el pulso en HIGH del sensor (80 us HIGH) con timeout */
    timeout = 100;
    while (HAL_GPIO_ReadPin(dev->gpio_port, dev->gpio_pin) == GPIO_PIN_RESET) {
        DHT11_Delay_us(1);
        if (--timeout == 0) return DHT11_ERR_TIMEOUT;
    }

    /* 6. Esperar la caída final del pulso ACK para alinearse con el primer bit de datos */
    timeout = 100;
    while (HAL_GPIO_ReadPin(dev->gpio_port, dev->gpio_pin) == GPIO_PIN_SET) {
        DHT11_Delay_us(1);
        if (--timeout == 0) return DHT11_ERR_TIMEOUT;
    }

    return DHT11_OK;
}

/* Lee 1 byte (8 bits) discriminando el ancho del pulso alto: '0' (~28us HIGH) o '1' (~70us HIGH) */
static DHT11_Status_t DHT11_ReadByte(DHT11_Handle_t *dev, uint8_t *p_value) {
    uint8_t i, data = 0;
    uint32_t timeout;

    for (i = 0; i < 8; i++) {
        /* Esperar el flanco de subida que marca el inicio del bit (precedido por 50 us LOW) */
        timeout = 100;
        while (HAL_GPIO_ReadPin(dev->gpio_port, dev->gpio_pin) == GPIO_PIN_RESET) {
            DHT11_Delay_us(1);
            if (--timeout == 0) return DHT11_ERR_TIMEOUT;
        }

        /* Esperar 40 us para muestrear el nivel lógico en el centro del pulso */
        DHT11_Delay_us(40);

        data <<= 1;
        if (HAL_GPIO_ReadPin(dev->gpio_port, dev->gpio_pin) == GPIO_PIN_SET) {
            data |= 1;
        }

        /* Esperar a que la línea baje finalizando la transmisión del bit actual */
        timeout = 100;
        while (HAL_GPIO_ReadPin(dev->gpio_port, dev->gpio_pin) == GPIO_PIN_SET) {
            DHT11_Delay_us(1);
            if (--timeout == 0) return DHT11_ERR_TIMEOUT;
        }
    }

    *p_value = data;
    return DHT11_OK;
}

/* ========================================================================= */
/* FUNCIONES PÚBLICAS DEL DRIVER                                            */
/* ========================================================================= */

/* Inicializa el manejador, asigna los puertos/pines y deja el bus en nivel alto en reposo */
DHT11_Status_t DHT11_Init(DHT11_Handle_t *dev, GPIO_TypeDef *gpio_port, uint16_t gpio_pin) {
    if (dev == NULL || gpio_port == NULL) {
        return DHT11_ERR_INVALID_PARAM;
    }

    dev->gpio_port = gpio_port;
    dev->gpio_pin = gpio_pin;
    dev->last_data.temperature = 0.0f;
    dev->last_data.humidity = 0.0f;

    /* Configurar el bus en estado inactivo por defecto (nivel alto mediante pull-up interna/externa) */
    DHT11_SetPinOutput(dev->gpio_port, dev->gpio_pin);
    HAL_GPIO_WritePin(dev->gpio_port, dev->gpio_pin, GPIO_PIN_SET);

    return DHT11_OK;
}

/* Ejecuta la secuencia completa de lectura del protocolo de 1 hilo (5 bytes) y verifica el checksum */
DHT11_Status_t DHT11_ReadData(DHT11_Handle_t *dev) {
    if (dev == NULL || dev->gpio_port == NULL) {
        return DHT11_ERR_INVALID_PARAM;
    }

    uint8_t buffer[5] = {0};
    uint8_t checksum = 0;

    /* 1. Transmitir señal de arranque y comprobar presencia del sensor */
    if (DHT11_SendStartSignal(dev) != DHT11_OK) {
        return DHT11_ERR_TIMEOUT;
    }

    /* 2. Recibir secuencialmente los 5 bytes de datos */
    if (DHT11_ReadByte(dev, &buffer[0]) != DHT11_OK) return DHT11_ERR_TIMEOUT; /* Humedad entera */
    if (DHT11_ReadByte(dev, &buffer[1]) != DHT11_OK) return DHT11_ERR_TIMEOUT; /* Humedad decimal */
    if (DHT11_ReadByte(dev, &buffer[2]) != DHT11_OK) return DHT11_ERR_TIMEOUT; /* Temperatura entera */
    if (DHT11_ReadByte(dev, &buffer[3]) != DHT11_OK) return DHT11_ERR_TIMEOUT; /* Temperatura decimal */
    if (DHT11_ReadByte(dev, &buffer[4]) != DHT11_OK) return DHT11_ERR_TIMEOUT; /* Byte de Checksum */

    /* 3. Validar checksum: suma de los primeros 4 bytes comparada con el quinto byte */
    checksum = (uint8_t)(buffer[0] + buffer[1] + buffer[2] + buffer[3]);
    if (checksum != buffer[4]) {
        return DHT11_ERR_CHECKSUM;
    }

    /* 4. Convertir datos a números flotantes y actualizar el manejador */
    dev->last_data.humidity = (float)buffer[0] + ((float)buffer[1] / 10.0f);
    dev->last_data.temperature = (float)buffer[2] + ((float)buffer[3] / 10.0f);

    return DHT11_OK;
}

/* Copia de manera segura la última lectura almacenada en el manejador hacia la estructura de salida */
DHT11_Status_t DHT11_GetLastData(DHT11_Handle_t *dev, DHT11_Data_t *data) {
    if (dev == NULL || data == NULL) {
        return DHT11_ERR_INVALID_PARAM;
    }

    *data = dev->last_data;
    return DHT11_OK;
}
