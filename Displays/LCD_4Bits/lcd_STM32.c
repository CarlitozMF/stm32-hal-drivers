/**
 * @file    lcd_STM32.c
 * @author  Mamani Flores Carlos (UTN FRT)
 * @brief   Implementación del driver LCD HD44780 para STM32 con HAL.
 * @details Contiene la lógica de gestión por nibbles en interfaz de 4 bits, tiempos de
 *          estabilización mediante retardos en microsegundos (Utils_DelayUs), mapeo dinámico
 *          de pines y funciones HMI (números, barra de progreso, caracteres personalizados).
 * @version 2.1
 * @date    2026
 */

#include "lcd_STM32.h"

/* ========================================================================== */
/*                          DEFINICIONES PRIVADAS                             */
/* ========================================================================== */

/* Comandos del HD44780 */
#define LCD_CMD_CLEAR          0x01U  /**< Clear Display (tiempo: ~1.52 ms)               */
#define LCD_CMD_ENTRY_MODE     0x06U  /**< Entry Mode: incrementa dirección, sin shift     */
#define LCD_CMD_DISPLAY_OFF    0x08U  /**< Display OFF                                     */
#define LCD_CMD_FUNCTION_SET   0x28U  /**< 4 bits, 2 líneas, matriz 5x8                    */
#define LCD_CMD_SET_CGRAM      0x40U  /**< Set CGRAM Address (base)                        */
#define LCD_CMD_SET_DDRAM      0x80U  /**< Set DDRAM Address (base)                        */

/** @brief Máximo valor absoluto representable por LCD_PrintFloat (evita overflow). */
#define LCD_FLOAT_MAX_VALUE    2.0e9f

/* Instancia estática privada para almacenar la configuración de hardware del LCD */
static LCD_Config_t _lcd;

/* ========================================================================== */
/*                         FUNCIONES PRIVADAS (BAJO NIVEL)                    */
/* ========================================================================== */

/**
 * @brief Genera un pulso en la línea Enable para que el LCD capture el nibble presente en D4-D7.
 */
static void LCD_PulseEnable(void) {
    HAL_GPIO_WritePin(_lcd.port_en, _lcd.pin_en, GPIO_PIN_RESET);
    Utils_DelayUs(2);   // Asegura EN en bajo antes del flanco (tiempo de setup de datos)
    HAL_GPIO_WritePin(_lcd.port_en, _lcd.pin_en, GPIO_PIN_SET);
    Utils_DelayUs(15);  // Ancho mínimo del pulso en Enable (alto): > 450 ns
    HAL_GPIO_WritePin(_lcd.port_en, _lcd.pin_en, GPIO_PIN_RESET);
    Utils_DelayUs(50);  // Tiempo de ejecución de la mayoría de comandos (~37 us)
}

/**
 * @brief Coloca un nibble (4 bits) en las líneas D4-D7 y lo envía con un pulso de Enable.
 * @param nibble Valor de 4 bits (solo se usan los bits 0 a 3).
 */
static void LCD_SendNibble(uint8_t nibble) {
    HAL_GPIO_WritePin(_lcd.port_d4, _lcd.pin_d4, (nibble & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(_lcd.port_d5, _lcd.pin_d5, (nibble & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(_lcd.port_d6, _lcd.pin_d6, (nibble & 0x04) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(_lcd.port_d7, _lcd.pin_d7, (nibble & 0x08) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    LCD_PulseEnable();
}

/**
 * @brief Envía un byte completo (comando o dato) al LCD en dos nibbles.
 * @param byte    Byte a enviar.
 * @param is_data 0 = instrucción (RS=0), distinto de 0 = dato (RS=1).
 * @note  No agrega retardos adicionales: los comandos lentos (Clear) los gestiona el llamador.
 */
static void LCD_SendByte(uint8_t byte, uint8_t is_data) {
    // Configurar el pin Register Select (RS): 0 = Instrucción, 1 = Registro de datos
    HAL_GPIO_WritePin(_lcd.port_rs, _lcd.pin_rs, is_data ? GPIO_PIN_SET : GPIO_PIN_RESET);

    // Transmitir primero el nibble alto y luego el nibble bajo
    LCD_SendNibble(byte >> 4);
    LCD_SendNibble(byte & 0x0F);
}

/**
 * @brief Imprime un entero sin signo de 32 bits en decimal.
 * @param value Valor a imprimir.
 */
static void LCD_PrintUInt(uint32_t value) {
    char buffer[10]; // UINT32_MAX tiene 10 dígitos
    uint8_t i = 0;

    if (value == 0) {
        LCD_WriteChar('0');
        return;
    }

    // Extraer dígitos de menor a mayor peso
    while (value > 0) {
        buffer[i++] = (char)('0' + (value % 10));
        value /= 10;
    }

    // Volcar los caracteres en orden correcto invirtiendo el búfer auxiliar
    while (i > 0) {
        LCD_WriteChar(buffer[--i]);
    }
}

/* ========================================================================== */
/*                              API PÚBLICA                                   */
/* ========================================================================== */

void LCD_Init(const LCD_Config_t *config) {
    if (config == NULL) return;

    _lcd = *config;
    Utils_DelayUs_Init();

    // Estado inicial seguro: todas las líneas en bajo (RS=0 es obligatorio para la secuencia)
    HAL_GPIO_WritePin(_lcd.port_rs, _lcd.pin_rs, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(_lcd.port_en, _lcd.pin_en, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(_lcd.port_d4, _lcd.pin_d4, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(_lcd.port_d5, _lcd.pin_d5, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(_lcd.port_d6, _lcd.pin_d6, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(_lcd.port_d7, _lcd.pin_d7, GPIO_PIN_RESET);

    HAL_Delay(50); // Estabilización inicial de VDD (>15 ms tras encendido)

    // Secuencia obligatoria de reseteo por software para forzar el modo de 4 bits
    LCD_SendNibble(0x03); HAL_Delay(5);  // > 4.1 ms
    LCD_SendNibble(0x03); HAL_Delay(1);  // > 100 us
    LCD_SendNibble(0x03); HAL_Delay(1);
    LCD_SendNibble(0x02); HAL_Delay(1);  // Conmutación definitiva a interfaz de 4 bits

    // Comandos de configuración inicial (orden indicado por el datasheet)
    LCD_SendByte(LCD_CMD_FUNCTION_SET, 0); // 4 bits, 2 líneas, matriz de 5x8 puntos
    LCD_SendByte(LCD_CMD_DISPLAY_OFF, 0);  // Display apagado durante la configuración
    LCD_Clear();                           // Borra la memoria y posiciona el cursor en (0,0)
    LCD_SendByte(LCD_CMD_ENTRY_MODE, 0);   // Cursor incrementa hacia la derecha, sin shift
    LCD_SendByte(LCD_CURSOR_OFF, 0);       // Display ENCENDIDO, cursor APAGADO, sin parpadeo
}

void LCD_Clear(void) {
    LCD_SendByte(LCD_CMD_CLEAR, 0);
    HAL_Delay(2); // El comando Clear requiere un tiempo de ejecución extendido (~1.52 ms)
}

void LCD_SetCursor(uint8_t row, uint8_t col) {
    // Direcciones base de DDRAM de cada fila. En el 20x4 las filas 2 y 3 son
    // continuación de las filas 0 y 1 (por eso 0x14 y 0x54).
    static const uint8_t row_offsets[4] = {0x00, 0x40, 0x14, 0x54};

    uint8_t max_row = (_lcd.type == LCD_20X4) ? 3 : 1;
    uint8_t max_col = (_lcd.type == LCD_20X4) ? 19 : 15;

    // Saturar parámetros para evitar accesos fuera de la tabla o direcciones inválidas
    if (row > max_row) row = max_row;
    if (col > max_col) col = max_col;

    LCD_SendByte((uint8_t)(LCD_CMD_SET_DDRAM + row_offsets[row] + col), 0);
}

void LCD_Print(const char *str) {
    if (str == NULL) return;

    while (*str) {
        LCD_SendByte((uint8_t)(*str++), 1);
    }
}

void LCD_WriteChar(char data) {
    LCD_SendByte((uint8_t)data, 1);
}

void LCD_SetCursorMode(lcd_cursor_mode_t mode) {
    LCD_SendByte((uint8_t)mode, 0);
}

void LCD_Shift(lcd_shift_dir_t direction) {
    LCD_SendByte((uint8_t)direction, 0);
}

void LCD_CreateCustomChar(uint8_t location, const uint8_t charmap[]) {
    if (charmap == NULL) return;

    location &= 0x7; // Asegurar rango válido de 0 a 7
    LCD_SendByte((uint8_t)(LCD_CMD_SET_CGRAM | (location << 3)), 0); // Apuntar a la CGRAM
    for (uint8_t i = 0; i < 8; i++) {
        LCD_SendByte(charmap[i], 1);
    }

    // IMPORTANTE: tras escribir en CGRAM el contador de direcciones sigue apuntando a la CGRAM.
    // Hay que volver a la DDRAM, de lo contrario los siguientes caracteres no se verían en pantalla.
    LCD_SendByte(LCD_CMD_SET_DDRAM, 0);
}

void LCD_PrintInt(int32_t value) {
    uint32_t magnitude;

    if (value < 0) {
        LCD_WriteChar('-');
        // Negación en aritmética sin signo: correcta incluso para INT32_MIN
        magnitude = 0U - (uint32_t)value;
    } else {
        magnitude = (uint32_t)value;
    }

    LCD_PrintUInt(magnitude);
}

void LCD_PrintFloat(float value, uint8_t decimal_places) {
    // Casos especiales: NaN (único valor distinto de sí mismo)
    if (value != value) {
        LCD_Print("NaN");
        return;
    }

    if (decimal_places > LCD_FLOAT_MAX_DECIMALS) {
        decimal_places = LCD_FLOAT_MAX_DECIMALS;
    }

    uint8_t is_negative = (value < 0.0f);
    if (is_negative) value = -value;

    // Fuera de rango (incluye +/- infinito): evita overflow en la conversión a entero
    if (value > LCD_FLOAT_MAX_VALUE) {
        LCD_Print(is_negative ? "-Ovf" : "Ovf");
        return;
    }

    // Factor 10^decimal_places
    uint32_t multiplier = 1;
    for (uint8_t i = 0; i < decimal_places; i++) {
        multiplier *= 10;
    }

    // Se redondea el valor COMPLETO escalado. Redondear solo la parte fraccionaria
    // fallaba con casos como 0.999 (2 decimales), donde el redondeo "se llevaba" un dígito.
    uint64_t scaled    = (uint64_t)(value * (float)multiplier + 0.5f);
    uint32_t int_part  = (uint32_t)(scaled / multiplier);
    uint32_t frac_part = (uint32_t)(scaled % multiplier);

    // Mostrar el signo solo si el resultado redondeado no es cero (evita "-0.00")
    if (is_negative && scaled != 0) {
        LCD_WriteChar('-');
    }

    LCD_PrintUInt(int_part);

    if (decimal_places > 0) {
        LCD_WriteChar('.');

        // Imprimir cada decimal de mayor a menor peso; esto rellena con ceros a
        // izquierda automáticamente (ej: frac=5, 2 decimales -> "05") sin búfer auxiliar.
        for (uint32_t div = multiplier / 10; div > 0; div /= 10) {
            LCD_WriteChar((char)('0' + ((frac_part / div) % 10)));
        }
    }
}

void LCD_DrawProgressBar(uint8_t row, uint8_t col, uint8_t width, uint8_t percentage) {
    if (width < 2) return; // Se necesitan al menos los dos corchetes (evita underflow de uint8_t)
    if (percentage > 100) percentage = 100;

    LCD_SetCursor(row, col);

    uint8_t inner_width  = width - 2; // Espacio útil interno descontando los corchetes [ y ]
    uint8_t filled_chars = (uint8_t)(((uint16_t)percentage * inner_width) / 100);

    // Dibujar delimitador de apertura
    LCD_WriteChar('[');

    // Sección activa de la barra con caracteres de avance '>'
    for (uint8_t i = 0; i < filled_chars; i++) {
        LCD_WriteChar('>');
    }

    // Resto del ancho con espacios en blanco
    for (uint8_t i = filled_chars; i < inner_width; i++) {
        LCD_WriteChar(' ');
    }

    // Dibujar delimitador de cierre
    LCD_WriteChar(']');
}

void LCD_Test(lcd_type_t type) {
    LCD_Clear();

    // 1. Test básico de impresión por filas
    LCD_SetCursor(0, 0);
    LCD_Print("--- LCD TEST ---");

    LCD_SetCursor(1, 0);
    LCD_Print("Driver OK");

    if (type == LCD_20X4) {
        LCD_SetCursor(2, 0);
        LCD_Print("Fila 3: 20x4 Activo");
        LCD_SetCursor(3, 0);
        LCD_Print("Fila 4: Verificando");
    }

    HAL_Delay(3000); // Pausa estática para lectura inicial

    // 2. Test de CGRAM (carácter personalizado en índice 0: un corazón)
    const uint8_t custom_char[8] = {
        0x00, 0x0A, 0x1F, 0x1F, 0x0E, 0x04, 0x00, 0x00
    };
    LCD_CreateCustomChar(0, custom_char);

    LCD_Clear();
    LCD_SetCursor(0, 0);
    LCD_Print("Custom Char: ");
    LCD_WriteChar(0); // Renderiza el carácter guardado en la posición 0 de la CGRAM

    HAL_Delay(2000);

    // 3. Test de números: enteros, flotantes y casos de borde
    LCD_Clear();
    LCD_SetCursor(0, 0);
    LCD_PrintInt(-12345);
    LCD_Print("  ");
    LCD_PrintFloat(3.14159f, 2);
    LCD_SetCursor(1, 0);
    LCD_PrintFloat(0.05f, 2);     // Verifica relleno de ceros ("0.05")
    LCD_Print(" ");
    LCD_PrintFloat(0.999f, 2);    // Verifica acarreo del redondeo ("1.00")
    HAL_Delay(3000);

    // 4. Test de barra de progreso
    LCD_Clear();
    for (uint8_t p = 0; p <= 100; p += 10) {
        LCD_DrawProgressBar(0, 0, 16, p);
        LCD_SetCursor(1, 0);
        LCD_PrintInt(p);
        LCD_Print("%  ");
        HAL_Delay(250);
    }

    // 5. Test de modos del cursor (parpadeo)
    LCD_Clear();
    LCD_SetCursor(1, 0);
    LCD_Print("Cursor Blink ON");
    LCD_SetCursorMode(LCD_CURSOR_BLINK);
    HAL_Delay(2500);
    LCD_SetCursorMode(LCD_CURSOR_OFF); // Desactivar el cursor al finalizar la prueba

    // 6. Test de desplazamiento dinámico de pantalla (Shift)
    LCD_Clear();
    LCD_SetCursor(0, 0);
    LCD_Print("Desplazando...");
    HAL_Delay(1000);

    for (int i = 0; i < 3; i++) {
        LCD_Shift(LCD_SHIFT_RIGHT);
        HAL_Delay(400);
    }
    for (int i = 0; i < 3; i++) {
        LCD_Shift(LCD_SHIFT_LEFT);
        HAL_Delay(400);
    }

    // 7. Cierre de la secuencia de test (Clear también resetea el shift)
    LCD_Clear();
    LCD_SetCursor(0, 0);
    LCD_Print("Test Finalizado!");
}
