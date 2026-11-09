/**
 * @file lcd_i2c_STM32.c
 * @author Mamani Flores Carlos (UTN FRT)
 * @brief Implementación del driver LCD HD44780 por I2C (PCF8574) para STM32 con HAL.
 * @details Contiene la lógica de gestión por I2C, empaquetado de nibbles, control de pines del expansor
 *          y funciones HMI para displays 16x2 y 20x4.
 *
 *          Arquitectura del driver (de abajo hacia arriba):
 *            1. LCD_I2C_WriteFrame : transmisión física por I2C (HAL) + backlight + manejo de errores.
 *            2. LCD_I2C_SendNibble : empaqueta 4 bits + RS + pulso de Enable (solo usado en Init).
 *            3. LCD_I2C_SendByte   : envía un byte completo como dos nibbles (modo de 4 bits).
 *            4. API pública        : comandos del LCD y funciones de alto nivel (print, barra, etc.).
 *
 *          Mapeo de pines del PCF8574 hacia el LCD (módulo estándar):
 *            P0 = RS | P1 = R/W (siempre 0) | P2 = E | P3 = Backlight | P4..P7 = D4..D7
 *
 * @version 2.1
 * @date 2026
 */

#include "lcd_i2c_STM32.h"

/* ========================================================================= */
/*                          DEFINICIONES PRIVADAS                            */
/* ========================================================================= */

/* --- Bits del byte que se escribe en el expansor PCF8574 --- */
#define LCD_PIN_RS          0x01U  /* P0: Register Select (0 = comando, 1 = dato)            */
#define LCD_PIN_EN          0x04U  /* P2: Enable (el LCD captura en el flanco de bajada)      */
#define LCD_PIN_BL          0x08U  /* P3: Backlight (ánodo del LED de retroiluminación)       */
/* Nota: P1 (R/W) se deja siempre en 0 porque este driver solo escribe en el LCD. */

/* Tiempo máximo de espera de cada transacción I2C. Corto a propósito: evita que un
 * bus I2C caído o un LCD desconectado bloquee el programa principal. */
#define LCD_I2C_TIMEOUT_MS  10U

/* --- Comandos del controlador HD44780 --- */
#define LCD_CMD_CLEAR       0x01U  /* Clear Display: borra DDRAM y lleva el cursor a (0,0)    */
#define LCD_CMD_ENTRY_MODE  0x06U  /* Entry Mode: I/D=1 (cursor incrementa), S=0 (sin shift)  */
#define LCD_CMD_FUNC_SET    0x28U  /* Function Set: 4 bits, 2 líneas, fuente 5x8              */
#define LCD_CMD_SET_CGRAM   0x40U  /* Set CGRAM Address: base para caracteres personalizados  */
#define LCD_CMD_SET_DDRAM   0x80U  /* Set DDRAM Address: base para posicionar el cursor       */

/* Instancia estática privada: guarda la configuración de hardware del display actual. */
static LCD_I2C_Config_t _lcd;

/*
 * Valor de la retroiluminación. Se inicializa con el bit P3 en 1 (encendida).
 * Todo byte enviado por I2C hace un OR lógico con este valor; de lo contrario,
 * cada escritura pondría P3 en 0 y la pantalla se apagaría.
 */
static uint8_t _backlight_val = LCD_PIN_BL;

/* Indica que Init terminó bien. Mientras sea 0, ninguna función toca el bus I2C. */
static uint8_t _initialized = 0;

/* Flag de error "pegajoso": se activa ante cualquier falla y se limpia al consultarlo
 * con LCD_I2C_GetError(). */
static uint8_t _error = 0;

/* ========================================================================= */
/*                           FUNCIONES PRIVADAS                              */
/* ========================================================================= */

/*
 * Transmite una trama de hasta 4 bytes al PCF8574 en UNA sola transacción I2C.
 *
 * - Aplica el estado actual del backlight (OR) a cada byte de la trama.
 * - Si el driver no está inicializado, o la longitud excede el búfer, no hace nada.
 * - Si la HAL devuelve cualquier valor distinto de HAL_OK (NACK, timeout, bus ocupado),
 *   se activa el flag de error para que la aplicación pueda detectarlo.
 *
 * Enviar varios bytes en una sola transacción es mucho más rápido que hacer una
 * transacción por byte, porque se ahorra la dirección y el START/STOP de cada una.
 */
static void LCD_I2C_WriteFrame(const uint8_t *frame, uint8_t len) {
    uint8_t buf[4];

    if (!_initialized || len > sizeof(buf)) {
        return;
    }

    /* Copia local con el backlight mezclado, sin modificar la trama original */
    for (uint8_t i = 0; i < len; i++) {
        buf[i] = frame[i] | _backlight_val;
    }

    if (HAL_I2C_Master_Transmit(_lcd.hi2c, _lcd.i2c_addr, buf, len, LCD_I2C_TIMEOUT_MS) != HAL_OK) {
        _error = 1;
    }
}

/*
 * Envía un nibble (4 bits) al LCD junto con su pulso de habilitación (Enable).
 *
 * El HD44780 lee los datos del bus en el FLANCO DE BAJADA del pin E. Por eso la
 * trama son dos bytes consecutivos:
 *     [ dato | EN ]  -> E = 1 (datos estables en el bus)
 *     [ dato      ]  -> E = 0 (flanco de bajada: el LCD captura el nibble)
 *
 * No se necesitan retardos extra: cada byte I2C dura decenas de microsegundos,
 * muy por encima del ancho mínimo de pulso de E (450 ns según datasheet).
 *
 * Esta función se usa directamente solo en la secuencia de reset de Init, donde
 * hay que mandar nibbles sueltos; el resto del tiempo se usa SendByte.
 */
static void LCD_I2C_SendNibble(uint8_t nibble, uint8_t is_data) {
    /* Los datos D4..D7 del LCD están en P4..P7: se desplaza el nibble a los 4 bits altos */
    uint8_t data = (uint8_t)((nibble << 4) & 0xF0U);

    /* RS (P0): 1 si es un carácter/dato (DDRAM/CGRAM), 0 si es una instrucción */
    if (is_data) {
        data |= LCD_PIN_RS;
    }

    uint8_t frame[2] = { (uint8_t)(data | LCD_PIN_EN), data };
    LCD_I2C_WriteFrame(frame, 2);
}

/*
 * Envía un byte completo de 8 bits usando el modo de 4 bits del HD44780.
 *
 * El byte se parte en dos nibbles, primero el ALTO (bits 7..4) y luego el BAJO
 * (bits 3..0), y cada uno lleva su pulso de Enable. Las 4 transmisiones resultantes
 * se juntan en una sola transacción I2C:
 *     [ hi|EN ] [ hi ] [ lo|EN ] [ lo ]
 *
 * Sobre los tiempos: la mayoría de las instrucciones del HD44780 tardan ~37-40 us
 * en ejecutarse. El propio tiempo de transferencia I2C (>100 us a 100 kHz) ya cubre
 * ese margen, por lo que no hace falta ningún retardo adicional. Las únicas
 * excepciones son Clear y Home (~1.52 ms), que se resuelven en LCD_I2C_Clear.
 */
static void LCD_I2C_SendByte(uint8_t byte, uint8_t is_data) {
    uint8_t rs = is_data ? LCD_PIN_RS : 0U;

    uint8_t hi = (uint8_t)((byte & 0xF0U) | rs);        /* Nibble alto, ya en P4..P7  */
    uint8_t lo = (uint8_t)(((byte << 4) & 0xF0U) | rs); /* Nibble bajo, llevado a P4..P7 */

    uint8_t frame[4] = {
        (uint8_t)(hi | LCD_PIN_EN), hi,  /* Nibble alto + pulso de Enable */
        (uint8_t)(lo | LCD_PIN_EN), lo   /* Nibble bajo + pulso de Enable */
    };
    LCD_I2C_WriteFrame(frame, 4);
}

/* ========================================================================= */
/*                              API PÚBLICA                                  */
/* ========================================================================= */

/*
 * Inicializa el driver y ejecuta la secuencia oficial de reset por software del
 * HD44780 (según datasheet de Hitachi, "Initializing by Instruction").
 *
 * Esta secuencia obliga al controlador a entrar en modo de 4 bits sin importar en
 * qué estado haya quedado al encender la placa (por ejemplo, tras un reset del
 * microcontrolador sin cortar la alimentación del LCD).
 *
 * Validaciones previas: si el puntero de configuración o el handler I2C son NULL,
 * o si el PCF8574 no responde en el bus, el driver queda deshabilitado y se activa
 * el flag de error (ver LCD_I2C_GetError). Así se evita bloquear el sistema con
 * timeouts repetidos en cada llamada posterior.
 */
void LCD_I2C_Init(const LCD_I2C_Config_t *config) {
    _initialized = 0;

    /* Validación de parámetros de entrada */
    if (config == NULL || config->hi2c == NULL) {
        _error = 1;
        return;
    }
    _lcd = *config;

    Utils_DelayUs_Init(); /* Prepara el sistema de retardos en microsegundos */

    /* Comprueba que el expansor PCF8574 conteste en la dirección configurada */
    if (HAL_I2C_IsDeviceReady(_lcd.hi2c, _lcd.i2c_addr, 2, LCD_I2C_TIMEOUT_MS) != HAL_OK) {
        _error = 1;
        return;
    }
    _initialized = 1;
    _backlight_val = LCD_PIN_BL; /* Backlight encendido por defecto */

    HAL_Delay(50); /* Estabilización de la alimentación del LCD (el datasheet pide >15 ms) */

    /* --- Secuencia de reset por software: tres veces 0x03 y luego 0x02 --- */
    LCD_I2C_SendNibble(0x03, 0); HAL_Delay(5);  /* 1ra vez: esperar >4.1 ms */
    LCD_I2C_SendNibble(0x03, 0); HAL_Delay(1);  /* 2da vez: esperar >100 us */
    LCD_I2C_SendNibble(0x03, 0);                /* 3ra vez                  */
    LCD_I2C_SendNibble(0x02, 0);                /* Conmutación definitiva a interfaz de 4 bits */

    /* --- Configuración del sistema (ya en modo de 4 bits, se envían bytes completos) --- */
    LCD_I2C_SendByte(LCD_CMD_FUNC_SET, 0);    /* 4 bits, 2 líneas, matriz 5x8 (sirve también para 20x4) */
    LCD_I2C_SendByte(LCD_I2C_CURSOR_OFF, 0);  /* Display ENCENDIDO, cursor apagado, sin parpadeo        */
    LCD_I2C_SendByte(LCD_CMD_ENTRY_MODE, 0);  /* El cursor avanza a la derecha, sin desplazar pantalla  */
    LCD_I2C_Clear();                          /* Limpia la DDRAM y posiciona el cursor en (0,0)         */
}

/*
 * Devuelve el estado del flag de error y lo limpia (lectura destructiva).
 * Permite a la aplicación detectar fallas de I2C que de otro modo pasarían en silencio:
 *     if (LCD_I2C_GetError()) { ... LCD desconectado o bus con problemas ... }
 */
uint8_t LCD_I2C_GetError(void) {
    uint8_t e = _error;
    _error = 0;
    return e;
}

/*
 * Enciende o apaga la retroiluminación.
 * Actualiza la variable global y envía de inmediato un byte con todo en cero
 * (EN = 0, RS = 0, datos = 0); WriteFrame le mezcla el nuevo estado del bit P3,
 * por lo que el efecto es visible al instante sin afectar el contenido del display.
 */
void LCD_I2C_Backlight(uint8_t on) {
    _backlight_val = on ? LCD_PIN_BL : 0U;

    uint8_t frame[1] = { 0x00 };
    LCD_I2C_WriteFrame(frame, 1);
}

/*
 * Borra todo el display y lleva el cursor a (0,0) (comando Clear Display = 0x01).
 *
 * Este comando es el más lento del HD44780: tarda ~1.52 ms internamente. Se espera
 * 2 ms con Utils_DelayUs (y no con HAL_Delay) por dos motivos:
 *   - HAL_Delay depende de SysTick y puede colgarse si se llama desde una ISR de
 *     prioridad igual o menor que la de SysTick.
 *   - Utils_DelayUs tiene resolución de microsegundos, sin el error de +1 tick de HAL_Delay.
 *
 * Además, Clear devuelve la pantalla a su posición original si había sido desplazada.
 */
void LCD_I2C_Clear(void) {
    LCD_I2C_SendByte(LCD_CMD_CLEAR, 0);
    Utils_DelayUs(2000);
}

/*
 * Posiciona el cursor en (fila, columna) calculando la dirección de DDRAM.
 *
 * La memoria DDRAM del HD44780 no es lineal respecto de las filas. Los offsets
 * de inicio de cada fila son:
 *     Fila 0 -> 0x00    Fila 1 -> 0x40    Fila 2 -> 0x14    Fila 3 -> 0x54
 * Un 16x2 usa solo las filas 0 y 1 (offsets 0x00 y 0x40); un 20x4 usa las cuatro
 * (las filas 2 y 3 son la continuación de las filas 0 y 1 en la memoria).
 *
 * Los parámetros fuera de rango se limitan (clamp) al último valor válido, lo que
 * evita leer fuera de la tabla de offsets y escribir en direcciones inexistentes.
 */
void LCD_I2C_SetCursor(uint8_t row, uint8_t col) {
    static const uint8_t row_offsets[4] = {0x00, 0x40, 0x14, 0x54};
    uint8_t max_row, max_col;

    /* Límites según la geometría del display configurado */
    if (_lcd.type == LCD_I2C_20X4) {
        max_row = 3; max_col = 19;
    } else {
        max_row = 1; max_col = 15;
    }
    if (row > max_row) row = max_row;
    if (col > max_col) col = max_col;

    /* Comando Set DDRAM Address: 0x80 | dirección */
    LCD_I2C_SendByte((uint8_t)(LCD_CMD_SET_DDRAM + row_offsets[row] + col), 0);
}

/*
 * Imprime una cadena terminada en nulo ('\0') carácter por carácter, a partir de la
 * posición actual del cursor. El cursor avanza solo (Entry Mode con I/D = 1).
 * Si el puntero es NULL no hace nada.
 */
void LCD_I2C_Print(const char *str) {
    if (str == NULL) return;

    while (*str) {
        LCD_I2C_SendByte((uint8_t)(*str++), 1); /* is_data = 1 -> RS = 1 (escritura en DDRAM) */
    }
}

/*
 * Escribe un único carácter en la posición actual del cursor.
 * También sirve para los caracteres personalizados: WriteChar(0) a WriteChar(7)
 * muestran los símbolos guardados en la CGRAM.
 */
void LCD_I2C_WriteChar(char data) {
    LCD_I2C_SendByte((uint8_t)data, 1);
}

/*
 * Cambia la visibilidad/parpadeo del cursor.
 * Los valores del enum son directamente los comandos "Display Control" del HD44780
 * (0x0C, 0x0E, 0x0F), todos con el display encendido.
 */
void LCD_I2C_SetCursorMode(lcd_i2c_cursor_mode_t mode) {
    LCD_I2C_SendByte((uint8_t)mode, 0);
}

/*
 * Desplaza todo el contenido visible una posición hacia la izquierda o la derecha
 * (comando "Cursor/Display Shift"). La DDRAM no cambia: solo se mueve la "ventana"
 * visible. Para volver a la posición original se puede usar LCD_I2C_Clear().
 */
void LCD_I2C_Shift(lcd_i2c_shift_dir_t direction) {
    LCD_I2C_SendByte((uint8_t)direction, 0);
}

/*
 * Define un carácter personalizado de 5x8 píxeles en la CGRAM del LCD.
 * Hay 8 ranuras disponibles (0 a 7), que luego se muestran con WriteChar(0..7).
 *
 * Funcionamiento:
 *   1. Se apunta el contador de direcciones a la CGRAM (comando 0x40 | índice*8).
 *   2. Se escriben las 8 filas del símbolo (cada byte usa sus 5 bits bajos).
 *   3. IMPORTANTE: se vuelve a DDRAM con el comando 0x80. Sin este paso, el contador
 *      de direcciones seguiría apuntando a la CGRAM y los siguientes caracteres
 *      que se impriman terminarían sobrescribiendo los símbolos en vez de aparecer
 *      en pantalla. Tras este comando el cursor queda en la posición (0,0).
 */
void LCD_I2C_CreateCustomChar(uint8_t location, const uint8_t charmap[]) {
    if (charmap == NULL) return;

    location &= 0x7U; /* Fuerza el índice al rango 0..7 (8 ranuras de CGRAM) */

    LCD_I2C_SendByte((uint8_t)(LCD_CMD_SET_CGRAM | (location << 3)), 0);
    for (uint8_t i = 0; i < 8; i++) {
        LCD_I2C_SendByte(charmap[i], 1); /* Cada byte es una fila de píxeles */
    }

    LCD_I2C_SendByte(LCD_CMD_SET_DDRAM, 0); /* Vuelve a modo DDRAM (cursor en 0,0) */
}

/*
 * Imprime un entero de 32 bits con signo, sin usar printf (que es pesado en
 * microcontroladores y suele requerir soporte de newlib-nano).
 *
 * Algoritmo:
 *   - Si es negativo, imprime '-' y trabaja con el valor absoluto.
 *   - La magnitud se calcula pasando por int64_t para evitar el desbordamiento
 *     que ocurre al negar INT32_MIN (-2147483648) en 32 bits.
 *   - Los dígitos se extraen de menor a mayor peso (módulo 10), por lo que quedan
 *     en orden inverso en el búfer; luego se imprimen al revés.
 *
 * El búfer de 12 posiciones alcanza para los 10 dígitos máximos de un uint32_t.
 */
void LCD_I2C_PrintInt(int32_t value) {
    char buffer[12];
    uint8_t i = 0;
    uint32_t mag;

    if (value < 0) {
        LCD_I2C_WriteChar('-');
        mag = (uint32_t)(-(int64_t)value);
    } else {
        mag = (uint32_t)value;
    }

    /* Caso especial: el cero no genera dígitos en el bucle de abajo */
    if (mag == 0) {
        LCD_I2C_WriteChar('0');
        return;
    }

    /* Extracción de dígitos (orden inverso) */
    while (mag > 0) {
        buffer[i++] = (char)('0' + (mag % 10U));
        mag /= 10U;
    }

    /* Impresión en el orden correcto (del más significativo al menos significativo) */
    while (i > 0) {
        LCD_I2C_WriteChar(buffer[--i]);
    }
}

/*
 * Imprime un float con una cantidad fija de decimales, sin usar printf("%f").
 *
 * Pasos:
 *   1. Casos especiales: NaN (detectado porque NaN != NaN) y valores que no caben
 *      en un int32 (incluye +infinito) se muestran como "NaN" / "OVF".
 *   2. Se limita decimal_places a 9 (máximo que entra en un uint32_t como multiplicador).
 *   3. Se separa parte entera y fraccionaria; la fracción se escala por 10^decimales
 *      y se redondea sumando 0.5.
 *   4. ACARREO: si el redondeo hace que la fracción llegue a 10^decimales
 *      (ej: 0.996 con 2 decimales -> 100), se suma 1 a la parte entera y se resta el
 *      multiplicador. Resultado correcto: "1.00" y no "0.00".
 *   5. Los decimales se generan de derecha a izquierda en un búfer, lo que completa
 *      automáticamente con ceros a la izquierda (ej: 3.05 -> "05").
 *
 * Limitación: un float tiene ~7 dígitos significativos; pedir más decimales
 * de los que la precisión permite mostrará dígitos poco significativos.
 */
void LCD_I2C_PrintFloat(float value, uint8_t decimal_places) {
    if (value != value) {              /* NaN: la única comparación que da falso consigo misma */
        LCD_I2C_Print("NaN");
        return;
    }
    if (decimal_places > 9) decimal_places = 9;

    if (value < 0.0f) {
        LCD_I2C_WriteChar('-');
        value = -value;                /* Trabaja con el valor absoluto */
    }
    if (value >= 2147483000.0f) {      /* No cabe en int32 (incluye +inf) */
        LCD_I2C_Print("OVF");
        return;
    }

    /* multiplier = 10^decimal_places */
    uint32_t multiplier = 1;
    for (uint8_t i = 0; i < decimal_places; i++) {
        multiplier *= 10U;
    }

    int32_t  int_part   = (int32_t)value;
    uint32_t frac_value = (uint32_t)((value - (float)int_part) * (float)multiplier + 0.5f);

    /* Acarreo por redondeo (ver descripción arriba) */
    if (frac_value >= multiplier) {
        int_part++;
        frac_value -= multiplier;
    }

    LCD_I2C_PrintInt(int_part);
    if (decimal_places == 0) return;   /* Sin decimales: no imprime el punto */
    LCD_I2C_WriteChar('.');

    /* Genera los dígitos decimales de derecha a izquierda (incluye ceros a la izquierda) */
    char buf[9];
    for (int8_t i = (int8_t)decimal_places - 1; i >= 0; i--) {
        buf[i] = (char)('0' + (frac_value % 10U));
        frac_value /= 10U;
    }
    for (uint8_t i = 0; i < decimal_places; i++) {
        LCD_I2C_WriteChar(buf[i]);
    }
}

/*
 * Dibuja una barra de progreso en texto, con el formato  [>>>>      ].
 *
 *   - 'width' es el ancho TOTAL, corchetes incluidos; el área útil es width - 2.
 *   - Si width < 3 no hay espacio útil (y width - 2 en uint8_t daría un valor enorme),
 *     por eso se retorna sin dibujar.
 *   - La cantidad de caracteres '>' se calcula proporcionalmente al porcentaje.
 *     Se usa uint16_t en la multiplicación para evitar desbordes con anchos grandes.
 *   - Se redibuja siempre toda la barra (llenos + vacíos), así que al disminuir el
 *     porcentaje no quedan restos de dibujos anteriores.
 */
void LCD_I2C_DrawProgressBar(uint8_t row, uint8_t col, uint8_t width, uint8_t percentage) {
    if (width < 3) return;
    if (percentage > 100) percentage = 100;

    LCD_I2C_SetCursor(row, col);

    uint8_t inner_width  = (uint8_t)(width - 2U);  /* Descuenta '[' y ']' */
    uint8_t filled_chars = (uint8_t)(((uint16_t)percentage * inner_width) / 100U);

    LCD_I2C_WriteChar('[');
    for (uint8_t i = 0; i < inner_width; i++) {
        LCD_I2C_WriteChar(i < filled_chars ? '>' : ' '); /* Lleno o vacío */
    }
    LCD_I2C_WriteChar(']');
}

/*
 * Secuencia integral de pruebas del hardware. Verifica, en orden:
 *   1. Impresión de texto y asignación de filas según la geometría (16x2 o 20x4).
 *   2. CGRAM: creación y visualización de un carácter personalizado (corazón).
 *   3. Cursor parpadeante.
 *   4. Impresión de enteros, floats (incluido el caso de acarreo 0.996 -> 1.00)
 *      y barra de progreso animada.
 *   5. Desplazamiento de pantalla a la derecha y a la izquierda.
 *   6. Mensaje final.
 *
 * Es bloqueante (usa HAL_Delay) y está pensada para ejecutarse una sola vez tras
 * LCD_I2C_Init, como prueba de puesta en marcha.
 */
void LCD_I2C_Test(lcd_i2c_type_t type) {
    LCD_I2C_Clear();

    /* 1. Texto estático por filas */
    LCD_I2C_SetCursor(0, 0);
    LCD_I2C_Print("--- LCD TEST ---");
    LCD_I2C_SetCursor(1, 0);
    LCD_I2C_Print("Driver I2C OK");

    /* Las filas 3 y 4 solo existen en el display de 20x4 */
    if (type == LCD_I2C_20X4) {
        LCD_I2C_SetCursor(2, 0);
        LCD_I2C_Print("Fila 3: 20x4 Activo");
        LCD_I2C_SetCursor(3, 0);
        LCD_I2C_Print("Fila 4: Verificando");
    }
    HAL_Delay(3000); /* Pausa para poder leer la pantalla */

    /* 2. CGRAM: carácter personalizado (un corazón). Cada byte es una fila de 5 píxeles. */
    const uint8_t custom_char[8] = {
        0x00, 0x0A, 0x1F, 0x1F, 0x0E, 0x04, 0x00, 0x00
    };
    LCD_I2C_CreateCustomChar(0, custom_char);

    LCD_I2C_Clear();
    LCD_I2C_SetCursor(0, 0);
    LCD_I2C_Print("Custom Char: ");
    LCD_I2C_WriteChar(0); /* Muestra la ranura 0 de la CGRAM */
    HAL_Delay(2000);

    /* 3. Cursor parpadeante */
    LCD_I2C_SetCursor(1, 0);
    LCD_I2C_Print("Cursor Blink ON");
    LCD_I2C_SetCursorMode(LCD_I2C_CURSOR_BLINK);
    HAL_Delay(2500);
    LCD_I2C_SetCursorMode(LCD_I2C_CURSOR_OFF); /* Apaga el cursor al terminar la prueba */

    /* 4. Números y barra de progreso */
    LCD_I2C_Clear();
    LCD_I2C_SetCursor(0, 0);
    LCD_I2C_PrintInt(-12345);          /* Entero negativo */
    LCD_I2C_Print(" ");
    LCD_I2C_PrintFloat(3.14159f, 2);   /* Debe mostrar 3.14 */
    LCD_I2C_Print(" ");
    LCD_I2C_PrintFloat(0.996f, 2);     /* Debe mostrar 1.00 (prueba del acarreo) */
    for (uint8_t p = 0; p <= 100; p += 20) {
        LCD_I2C_DrawProgressBar(1, 0, 16, p); /* Barra de 16 caracteres en la fila 1 */
        HAL_Delay(400);
    }
    HAL_Delay(1000);

    /* 5. Desplazamiento dinámico de la pantalla */
    LCD_I2C_Clear();
    LCD_I2C_SetCursor(0, 0);
    LCD_I2C_Print("Desplazando...");
    HAL_Delay(1000);

    for (int i = 0; i < 3; i++) {
        LCD_I2C_Shift(LCD_I2C_SHIFT_RIGHT);
        HAL_Delay(400);
    }
    for (int i = 0; i < 3; i++) {
        LCD_I2C_Shift(LCD_I2C_SHIFT_LEFT);
        HAL_Delay(400);
    }

    /* 6. Cierre de la secuencia (Clear también restablece el desplazamiento) */
    LCD_I2C_Clear();
    LCD_I2C_SetCursor(0, 0);
    LCD_I2C_Print("Test Finalizado!");
}
