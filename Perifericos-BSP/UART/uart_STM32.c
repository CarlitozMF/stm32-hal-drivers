/**
 * @file uart_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación del driver UART 100% no bloqueante con Ring Buffers RX/TX.
 * @details Contiene la gestión de colas circulares, secciones críticas seguras mediante PRIMASK
 *          y el despacho asíncrono integrado con la HAL de STM32.
 * @version 1.3
 * @date 2026
 */

#include <uart_STM32.h>

/* ============================================================================
 * CONFIGURACIONES Y CONSTANTES PRIVADAS
 * ============================================================================ */

/* Tamaño fijo para los buffers circulares de recepción y transmisión (en bytes) */
#define UART_BUFFER_SIZE 128

/*
 * Estructura de control para un Ring Buffer (FIFO circular).
 * Utiliza índices head (escritura) y tail (lectura) declarados como volatile
 * para garantizar la consistencia entre el bucle principal y las interrupciones (ISR).
 */
typedef struct {
    uint8_t buffer[UART_BUFFER_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} RingBuffer_t;

/* ============================================================================
 * VARIABLES PRIVADAS DEL DRIVER
 * ============================================================================ */
static UART_HandleTypeDef *uart_instance = NULL; /* Puntero a la instancia UART de la HAL */
static uint8_t rx_byte = 0;                      /* Variable de un solo byte para la escucha asíncrona RX */
static uint8_t tx_byte = 0;                      /* Byte actual en vuelo transmitido por interrupción */
static volatile uint8_t is_tx_busy = 0;          /* Bandera de estado: indica si el transmisor está activo */

/* Instancias independientes de los Ring Buffers para recepción y transmisión */
static RingBuffer_t rx_ring_buffer = { .head = 0, .tail = 0 };
static RingBuffer_t tx_ring_buffer = { .head = 0, .tail = 0 };

/* ============================================================================
 * SECCIONES CRÍTICAS SEGURAS (MANEJO DE PRIMASK)
 * ============================================================================
 * Almacenan el estado previo del registro PRIMASK y deshabilitan interrupciones.
 * Permiten llamadas anidadas seguras al restaurar exactamente el estado anterior.
 */
#define UART_ENTER_CRITICAL()  uint32_t _primask = __get_PRIMASK(); __disable_irq()
#define UART_EXIT_CRITICAL()   __set_PRIMASK(_primask)

/* ============================================================================
 * FUNCIONES AUXILIARES PRIVADAS
 * ============================================================================ */

/*
 * Gestiona y despacha el envío del próximo byte disponible en el Ring Buffer TX.
 * IMPORTANTE: Debe ejecutarse estrictamente con interrupciones deshabilitadas
 * para proteger la integridad de los punteros del buffer.
 * Lógica defensiva: si la HAL rechaza el envío, restituye el índice y libera el flag.
 */
static void tx_kick(void) {
    /* Si los punteros head y tail coinciden, el buffer está vacío */
    if (tx_ring_buffer.head == tx_ring_buffer.tail) {
        is_tx_busy = 0;
        return;
    }

    /* Extraemos el siguiente byte del buffer circular (FIFO) */
    tx_byte = tx_ring_buffer.buffer[tx_ring_buffer.tail];
    tx_ring_buffer.tail = (tx_ring_buffer.tail + 1) % UART_BUFFER_SIZE;

    /* Intentamos disparar la transmisión por interrupción con la HAL */
    if (HAL_UART_Transmit_IT(uart_instance, &tx_byte, 1) == HAL_OK) {
        is_tx_busy = 1;
    } else {
        /* Si la HAL falla, devolvemos el byte al buffer (retrocedemos el tail) y liberamos el flag */
        tx_ring_buffer.tail = (tx_ring_buffer.tail + UART_BUFFER_SIZE - 1) % UART_BUFFER_SIZE;
        is_tx_busy = 0;
    }
}

/* ============================================================================
 * FUNCIONES PÚBLICAS DEL DRIVER
 * ============================================================================ */

void UART_Driver_Init(UART_HandleTypeDef *huart) {
    uart_instance = huart;

    /* Reiniciamos los índices de ambos buffers a cero */
    rx_ring_buffer.head = 0;
    rx_ring_buffer.tail = 0;
    tx_ring_buffer.head = 0;
    tx_ring_buffer.tail = 0;

    is_tx_busy = 0;
}

void UART_Driver_SendChar(char c) {
    if (uart_instance == NULL) return;

    /* Protegemos la sección crítica al manipular el ring buffer TX desde el main */
    UART_ENTER_CRITICAL();

    /* Calculamos la siguiente posición del head de forma circular */
    uint16_t next_head = (tx_ring_buffer.head + 1) % UART_BUFFER_SIZE;

    /* Verificamos que el buffer no esté lleno (evitamos colisión con el tail) */
    if (next_head != tx_ring_buffer.tail) {
        tx_ring_buffer.buffer[tx_ring_buffer.head] = (uint8_t)c;
        tx_ring_buffer.head = next_head;
    }
    // Nota: Si el buffer está lleno, el carácter se descarta para evitar bloqueos.

    /* Si el transmisor hardware no está ocupado, arrancamos el proceso de envío */
    if (!is_tx_busy) {
        tx_kick();
    }

    UART_EXIT_CRITICAL();
}

void UART_Driver_SendString(const char *str) {
    if (str == NULL) return;

    /* Envía cadena carácter por carácter encolándolos en el buffer TX */
    while (*str != '\0') {
        UART_Driver_SendChar(*str);
        str++;
    }
}

void UART_Driver_StartReceive_IT(void) {
    if (uart_instance != NULL) {
        /* Dispara la primera recepción asíncrona de 1 byte para abrir la escucha por interrupción */
        HAL_UART_Receive_IT(uart_instance, &rx_byte, 1);
    }
}

uint8_t UART_Driver_DataAvailable(void) {
    /* Retorna verdadero si hay datos nuevos en el buffer RX (head != tail) */
    return (rx_ring_buffer.head != rx_ring_buffer.tail);
}

uint8_t UART_Driver_ReadChar(uint8_t *pData) {
    if (pData == NULL || !UART_Driver_DataAvailable()) {
        return 0; // No hay datos para leer
    }

    /* Extraemos el byte desde la cola del buffer RX */
    *pData = rx_ring_buffer.buffer[rx_ring_buffer.tail];
    rx_ring_buffer.tail = (rx_ring_buffer.tail + 1) % UART_BUFFER_SIZE;

    return 1; // Lectura exitosa
}

void UART_Driver_RxHandler(void) {
    if (uart_instance != NULL) {
        /* Calculamos la siguiente posición para almacenar el byte recibido */
        uint16_t next_head = (rx_ring_buffer.head + 1) % UART_BUFFER_SIZE;

        /* Si el buffer RX no está lleno, guardamos el byte recibido */
        if (next_head != rx_ring_buffer.tail) {
            rx_ring_buffer.buffer[rx_ring_buffer.head] = rx_byte;
            rx_ring_buffer.head = next_head;
        }

        /* Reiniciamos inmediatamente la escucha para el siguiente byte entrante */
        HAL_UART_Receive_IT(uart_instance, &rx_byte, 1);
    }
}

void UART_Driver_TxHandler(void) {
    if (uart_instance == NULL) return;

    /* Protegemos la sección crítica al despachar el siguiente byte desde la ISR de transmisión */
    UART_ENTER_CRITICAL();
    tx_kick();   /* Envía el próximo byte o libera is_tx_busy si el buffer se vació */
    UART_EXIT_CRITICAL();
}

void UART_Driver_ErrorHandler(void) {
    if (uart_instance != NULL) {
        /*
         * Nota: La HAL de ST limpia automáticamente los flags de error (ORE, NE, FE)
         * antes de invocar este callback. Únicamente re-armamos la recepción por IT.
         */
        HAL_UART_Receive_IT(uart_instance, &rx_byte, 1);
    }
}
