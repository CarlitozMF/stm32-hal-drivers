/**
 * @file    OLED_Display_STM32.h
 * @author  Mamani Flores Carlos (UTN FRT)
 * @brief   Cabecera del driver I2C para displays OLED SSD1306 / SH1106.
 * @details Define las estructuras de configuración, el tipo de fuente y los prototipos
 *          de la API pública. El driver trabaja con un *framebuffer* en RAM: todas las
 *          primitivas gráficas (píxel, línea, texto, etc.) dibujan sobre ese búfer y
 *          recién se ven en pantalla al llamar a OLED_UpdateDisplay().
 *
 * @par Organización de memoria
 * El framebuffer se organiza por **páginas**: cada página agrupa 8 filas de píxeles y
 * cada byte representa una columna vertical de 8 píxeles (el bit 0 es el píxel superior).
 * @code
 * indice = x + (y / 8) * width;
 * bit    = y % 8;
 * @endcode
 *
 * @par Sistema de coordenadas
 * Origen (0,0) en la esquina superior izquierda. X crece hacia la derecha e Y hacia abajo.
 * Todo lo que quede fuera de pantalla se recorta (clipping) sin generar error.
 *
 * @par Ejemplo de uso
 * @code
 * OLED_Config_t oled = {
 *     .driver_type    = OLED_DRIVER_SSD1306,
 *     .resolution     = OLED_RESOLUTION_128x64,
 *     .i2c_address    = 0x78,       // 0x3C << 1
 *     .hal_i2c_handle = &hi2c1,
 *     .rst_port       = NULL,       // sin pin de reset
 *     .rst_pin        = 0
 * };
 *
 * if (OLED_Init(&oled)) {
 *     OLED_DrawString(&oled, 0, 0, "Hola", &Font_7x10, OLED_COLOR_WHITE);
 *     OLED_UpdateDisplay(&oled);    // vuelca el framebuffer a la pantalla
 * }
 * @endcode
 *
 * @warning El framebuffer es un único búfer estático compartido en el .c, por lo que
 *          el driver soporta **un solo display** a la vez. Tampoco es reentrante:
 *          no llamar a sus funciones desde una ISR mientras se usan en el hilo principal.
 *
 * @version 1.1
 * @date    2026
 */

#ifndef OLED_DISPLAY_STM32_H
#define OLED_DISPLAY_STM32_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "main.h"   /* Provee la HAL de STM32 (I2C, GPIO, HAL_Delay) */

/** @defgroup OLED_Colors Macros de Colores
 *  @brief Valores de color para las funciones de dibujo (display monocromo).
 *  @{
 */
#define OLED_COLOR_BLACK 0 /**< Píxel apagado (negro). */
#define OLED_COLOR_WHITE 1 /**< Píxel encendido (blanco). */
/** @} */

/**
 * @brief Tipos de controlador OLED soportados.
 * @note  El SH1106 tiene 132 columnas de RAM; el driver compensa con un offset de
 *        2 columnas al volcar el framebuffer (ver OLED_UpdateDisplay()).
 */
typedef enum {
    OLED_DRIVER_SSD1306 = 0, /**< Controlador SSD1306 (típico en módulos de 0.96"). */
    OLED_DRIVER_SH1106  = 1  /**< Controlador SH1106 (típico en módulos de 1.3"). */
} OLED_DriverType_t;

/**
 * @brief Resoluciones físicas soportadas.
 */
typedef enum {
    OLED_RESOLUTION_128x32 = 0, /**< 128x32 píxeles (4 páginas). */
    OLED_RESOLUTION_128x64 = 1  /**< 128x64 píxeles (8 páginas). */
} OLED_Resolution_t;

/**
 * @brief Estructura de configuración y estado del display.
 * @details El usuario completa los campos de entrada antes de llamar a OLED_Init().
 *          Los campos marcados como "salida" los completa el driver.
 */
typedef struct {
    OLED_DriverType_t driver_type;      /**< [entrada] Tipo de controlador (SSD1306 o SH1106). */
    OLED_Resolution_t resolution;       /**< [entrada] Resolución del display. */
    uint8_t i2c_address;                /**< [entrada] Dirección I2C de 7 bits desplazada 1 bit a la izquierda
                                             (formato HAL). Ej.: 0x3C -> 0x78, 0x3D -> 0x7A. */

    I2C_HandleTypeDef* hal_i2c_handle;  /**< [entrada] Handle del periférico I2C de la HAL (ej. &hi2c1). */

    GPIO_TypeDef* rst_port;             /**< [entrada] Puerto GPIO del pin RESET (NULL si no se usa). */
    uint16_t rst_pin;                   /**< [entrada] Máscara del pin RESET (ej. GPIO_PIN_5). */

    uint8_t width;                      /**< [salida] Ancho efectivo en píxeles (lo fija OLED_Init()). */
    uint8_t height;                     /**< [salida] Alto efectivo en píxeles (lo fija OLED_Init()). */

    uint8_t* framebuffer;               /**< [salida] Puntero al framebuffer en RAM (lo asigna OLED_Init()). */
} OLED_Config_t;

/**
 * @brief Descriptor de una fuente tipográfica.
 * @details Se soportan dos formatos de datos, ambos desde ASCII 32 (' ') hasta 126 ('~'):
 *          - **Fuente 6x8**: `width` entradas por carácter, una por columna; el bit 0 es la fila superior.
 *          - **Resto de fuentes**: `height` entradas por carácter, una por fila; los bits se leen desde
 *            el bit 15 (columna izquierda) hacia abajo, por lo que `width` debe ser **<= 16**.
 */
typedef struct {
    const uint16_t* data; /**< Puntero a la tabla de mapa de bits de la fuente. */
    uint8_t width;        /**< Ancho de cada carácter en píxeles. */
    uint8_t height;       /**< Alto de cada carácter en píxeles. */
} FontDef;

/* ========================================================================== *
 * Capa de abstracción de bajo nivel (implementadas como __WEAK)              *
 * ========================================================================== */

/**
 * @brief Transmite datos o comandos por I2C.
 * @note  Definida como __WEAK: se puede redefinir en el proyecto para usar DMA,
 *        otro bus o un puerto de otra familia STM32.
 * @param hi2c    Handle de I2C de la HAL.
 * @param address Dirección I2C (7 bits desplazada 1 a la izquierda).
 * @param data    Buffer a transmitir (el primer byte es el byte de control).
 * @param size    Cantidad de bytes a transmitir.
 * @param timeout Tiempo máximo de espera en ms.
 */
void OLED_HAL_I2C_Transmit(I2C_HandleTypeDef* hi2c, uint8_t address, uint8_t* data, uint16_t size, uint32_t timeout);

/**
 * @brief Escribe un nivel lógico en un pin GPIO (usado para el RESET opcional).
 * @note  Definida como __WEAK.
 * @param GPIOx    Puerto GPIO.
 * @param GPIO_Pin Máscara del pin.
 * @param PinState GPIO_PIN_RESET o GPIO_PIN_SET.
 */
void OLED_HAL_GPIO_WritePin(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState);

/**
 * @brief Retardo bloqueante en milisegundos.
 * @note  Definida como __WEAK (por defecto usa HAL_Delay()).
 * @param Delay Tiempo en ms.
 */
void OLED_HAL_Delay(uint32_t Delay);

/* ========================================================================== *
 * API pública                                                                *
 * ========================================================================== */

/**
 * @brief Inicializa el display: resetea (si hay pin), configura el controlador y limpia la pantalla.
 * @param config Configuración. Deben estar cargados los campos de entrada de @ref OLED_Config_t.
 * @retval true  Inicialización completada.
 * @retval false `config` o el handle I2C son NULL, o la resolución es inválida.
 * @note  Los errores de bus I2C no se detectan (las funciones de transmisión no devuelven estado).
 */
bool OLED_Init(OLED_Config_t* config);

/**
 * @brief Borra el framebuffer (todos los píxeles en negro).
 * @param config        Configuración del display.
 * @param clear_display Si es true, además actualiza la pantalla física de inmediato.
 */
void OLED_Clear(OLED_Config_t* config, bool clear_display);

/**
 * @brief Vuelca el framebuffer completo a la pantalla física, página por página.
 * @details Debe llamarse después de dibujar para que los cambios sean visibles.
 * @param config Configuración del display.
 */
void OLED_UpdateDisplay(OLED_Config_t* config);

/**
 * @brief Enciende o apaga un píxel en el framebuffer. Los píxeles fuera de pantalla se ignoran.
 * @param config Configuración del display.
 * @param x      Columna (0 a width-1).
 * @param y      Fila (0 a height-1).
 * @param color  #OLED_COLOR_BLACK o #OLED_COLOR_WHITE.
 */
void OLED_DrawPixel(OLED_Config_t* config, int16_t x, int16_t y, uint8_t color);

/**
 * @brief Dibuja una línea con el algoritmo de Bresenham.
 * @param config Configuración del display.
 * @param x0 X inicial.
 * @param y0 Y inicial.
 * @param x1 X final.
 * @param y1 Y final.
 * @param color Color de la línea.
 */
void OLED_DrawLine(OLED_Config_t* config, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);

/**
 * @brief Dibuja un rectángulo vacío o relleno.
 * @param config Configuración del display.
 * @param x      X de la esquina superior izquierda.
 * @param y      Y de la esquina superior izquierda.
 * @param w      Ancho en píxeles (debe ser > 0, sino no dibuja nada).
 * @param h      Alto en píxeles (debe ser > 0, sino no dibuja nada).
 * @param color  Color del rectángulo.
 * @param filled true = relleno, false = solo contorno.
 */
void OLED_DrawRectangle(OLED_Config_t* config, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color, bool filled);

/**
 * @brief Dibuja el contorno de un triángulo a partir de sus tres vértices.
 * @param config Configuración del display.
 * @param x1 X del vértice 1.
 * @param y1 Y del vértice 1.
 * @param x2 X del vértice 2.
 * @param y2 Y del vértice 2.
 * @param t_x3 X del vértice 3.
 * @param t_y3 Y del vértice 3.
 * @param color Color del triángulo.
 */
void OLED_DrawTriangle(OLED_Config_t* config, int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t t_x3, int16_t t_y3, uint8_t color);

/**
 * @brief Dibuja el contorno de un círculo (algoritmo del punto medio / Bresenham).
 * @param config Configuración del display.
 * @param x0 X del centro.
 * @param y0 Y del centro.
 * @param radius Radio en píxeles.
 * @param color Color del círculo.
 */
void OLED_DrawCircle(OLED_Config_t* config, int16_t x0, int16_t y0, int16_t radius, uint8_t color);

/**
 * @brief Dibuja un círculo relleno.
 * @param config Configuración del display.
 * @param x0 X del centro.
 * @param y0 Y del centro.
 * @param radius Radio en píxeles.
 * @param color Color del círculo.
 */
void OLED_DrawFilledCircle(OLED_Config_t* config, int16_t x0, int16_t y0, int16_t radius, uint8_t color);

/**
 * @brief Dibuja un carácter ASCII (32 a 126) en el framebuffer.
 * @details Solo se encienden los píxeles del glifo; el fondo no se modifica (texto transparente).
 * @param config    Configuración del display.
 * @param x         X de la esquina superior izquierda del carácter.
 * @param y         Y de la esquina superior izquierda del carácter.
 * @param character Carácter a dibujar (fuera de 32..126 se ignora).
 * @param font      Fuente a utilizar (@ref FontDef).
 * @param color     Color del carácter.
 */
void OLED_DrawChar(OLED_Config_t* config, int16_t x, int16_t y, char character, const FontDef *font, uint8_t color);

/**
 * @brief Dibuja una cadena de texto con salto de línea automático.
 * @details Si el siguiente carácter no entra en el ancho de pantalla, continúa en la línea
 *          siguiente (alineada a `x`). Si tampoco entra en alto, se descarta el resto.
 *          Los caracteres fuera de 32..126 (acentos, '\\n', etc.) se omiten.
 * @param config Configuración del display.
 * @param x      X de inicio.
 * @param y      Y de inicio.
 * @param str    Cadena terminada en nulo.
 * @param font   Fuente a utilizar.
 * @param color  Color del texto.
 */
void OLED_DrawString(OLED_Config_t* config, int16_t x, int16_t y, const char* str, const FontDef *font, uint8_t color);

/**
 * @brief Desplaza el contenido hacia la izquierda por software y actualiza la pantalla.
 * @details Los píxeles que entran por la derecha quedan en negro. Si `pixels_to_scroll`
 *          supera el ancho, se limita al ancho (pantalla en blanco).
 * @param config Configuración del display.
 * @param pixels_to_scroll Cantidad de columnas a desplazar.
 */
void OLED_ScrollLeft_Software(OLED_Config_t* config, uint8_t pixels_to_scroll);

/**
 * @brief Desplaza el contenido hacia la derecha por software y actualiza la pantalla.
 * @details Los píxeles que entran por la izquierda quedan en negro. Si `pixels_to_scroll`
 *          supera el ancho, se limita al ancho (pantalla en blanco).
 * @param config Configuración del display.
 * @param pixels_to_scroll Cantidad de columnas a desplazar.
 */
void OLED_ScrollRight_Software(OLED_Config_t* config, uint8_t pixels_to_scroll);

/**
 * @brief Invierte los colores de toda la pantalla por hardware (comandos 0xA6 / 0xA7).
 * @note  Actúa directamente sobre el controlador; no modifica el framebuffer.
 * @param config Configuración del display.
 * @param invert true = colores invertidos, false = normal.
 */
void OLED_InvertDisplay(OLED_Config_t* config, bool invert);

/**
 * @brief Dibuja un bitmap monocromo en el framebuffer (llamar luego a OLED_UpdateDisplay()).
 * @details Formato del bitmap: el mismo del framebuffer (por páginas, 1 byte = 8 píxeles
 *          verticales, bit 0 arriba), es decir el que generan herramientas como image2cpp
 *          en modo "vertical, 1 bit por píxel". El bitmap es opaco: los bits en 0 se dibujan en negro.
 * @param config Configuración del display.
 * @param x      X de la esquina superior izquierda.
 * @param y      Y de la esquina superior izquierda.
 * @param bitmap Datos del bitmap (`width * height / 8` bytes).
 * @param width  Ancho en píxeles.
 * @param height Alto en píxeles (múltiplo de 8, sino no dibuja).
 */
void OLED_DrawBitmap(OLED_Config_t* config, uint8_t x, uint8_t y, const uint8_t *bitmap, uint8_t width, uint8_t height);

/**
 * @brief Ejecuta una secuencia de pruebas: texto, inversión, scroll, primitivas y fuentes.
 * @note  Es bloqueante (tarda varios segundos).
 * @param config Configuración del display.
 */
void OLED_Test(OLED_Config_t* config);

#endif /* OLED_DISPLAY_STM32_H */
