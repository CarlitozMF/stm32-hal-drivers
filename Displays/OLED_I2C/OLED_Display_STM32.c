/**
 * @file    OLED_Display_STM32.c
 * @author  Mamani Flores Carlos (UTN FRT)
 * @brief   Implementación del driver I2C para displays OLED SSD1306 / SH1106.
 * @details Gestión del framebuffer en RAM, rasterización de primitivas gráficas,
 *          texto con dos formatos de fuente y control de bajo nivel mediante la HAL de STM32.
 * @version 1.1
 * @date    2026
 */

#include "OLED_Display_STM32.h"
#include "OLED_Fonts_STM32.h"

/* ========================================================================== *
 * Constantes internas                                                        *
 * ========================================================================== */

#define OLED_TEST_DELAY_MS      1500                                /* Retardo base de la rutina de pruebas */
#define OLED_MAX_WIDTH          128                                 /* Ancho máximo soportado (píxeles) */
#define OLED_MAX_HEIGHT         64                                  /* Alto máximo soportado (píxeles) */
#define OLED_FRAMEBUFFER_SIZE   (OLED_MAX_WIDTH * OLED_MAX_HEIGHT / 8) /* 1024 bytes: 1 bit por píxel */
#define OLED_MAX_TX_BUFFER_SIZE (OLED_MAX_WIDTH + 1)                /* 1 byte de control I2C + hasta 128 de datos */

#define OLED_CTRL_COMMAND       0x00  /* Byte de control: lo que sigue es un comando (Co=0, D/C#=0) */
#define OLED_CTRL_DATA          0x40  /* Byte de control: lo que sigue son datos de GDDRAM (Co=0, D/C#=1) */
#define OLED_I2C_TIMEOUT_MS     100   /* Timeout de cada transacción I2C */

/* ========================================================================== *
 * Variables internas                                                         *
 * ========================================================================== */

/* Framebuffer único en RAM: de ahí que el driver soporte un solo display. */
static uint8_t oled_framebuffer[OLED_FRAMEBUFFER_SIZE];

/* Búfer de trama I2C: [byte de control][datos...]. Compartido, por lo que no es reentrante. */
static uint8_t oled_tx_buffer[OLED_MAX_TX_BUFFER_SIZE];

/* ========================================================================== *
 * Prototipos de funciones internas                                           *
 * ========================================================================== */

static void OLED_SendCommand(OLED_Config_t* config, uint8_t command);
static void OLED_SendData(OLED_Config_t* config, const uint8_t* data, uint16_t size);
static void OLED_HLine(OLED_Config_t* config, int16_t x1, int16_t x2, int16_t y, uint8_t color);

/* ========================================================================== *
 * Capa HAL débil (reemplazable desde el proyecto)                            *
 * ========================================================================== */

/* Transmisión I2C. Se puede redefinir para usar DMA, interrupciones u otro bus. */
__WEAK void OLED_HAL_I2C_Transmit(I2C_HandleTypeDef* hi2c, uint8_t address, uint8_t* data, uint16_t size, uint32_t timeout) {
	HAL_I2C_Master_Transmit(hi2c, address, data, size, timeout);
}

/* Escritura de GPIO, usada solo para el pin RESET opcional. */
__WEAK void OLED_HAL_GPIO_WritePin(GPIO_TypeDef* GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState) {
	HAL_GPIO_WritePin(GPIOx, GPIO_Pin, PinState);
}

/* Retardo bloqueante en milisegundos. */
__WEAK void OLED_HAL_Delay(uint32_t Delay) {
	HAL_Delay(Delay);
}

/* ========================================================================== *
 * Funciones internas                                                         *
 * ========================================================================== */

/*
 * Envía un comando de 1 byte al controlador.
 * Trama I2C: [0x00][comando]
 */
static void OLED_SendCommand(OLED_Config_t* config, uint8_t command) {
	oled_tx_buffer[0] = OLED_CTRL_COMMAND;
	oled_tx_buffer[1] = command;
	OLED_HAL_I2C_Transmit(config->hal_i2c_handle, config->i2c_address, oled_tx_buffer, 2, OLED_I2C_TIMEOUT_MS);
}

/*
 * Envía hasta 128 bytes de datos gráficos a la GDDRAM (una fila de página completa).
 * Trama I2C: [0x40][dato0][dato1]...
 */
static void OLED_SendData(OLED_Config_t* config, const uint8_t* data, uint16_t size) {
	/* Protección contra desborde del búfer de transmisión. */
	if (size > OLED_MAX_WIDTH) {
		size = OLED_MAX_WIDTH;
	}
	oled_tx_buffer[0] = OLED_CTRL_DATA;
	memcpy(&oled_tx_buffer[1], data, size);
	OLED_HAL_I2C_Transmit(config->hal_i2c_handle, config->i2c_address, oled_tx_buffer, size + 1, OLED_I2C_TIMEOUT_MS);
}

/*
 * Dibuja una línea horizontal entre x1 y x2 (en cualquier orden) en la fila y.
 * Auxiliar para rectángulos y círculos rellenos (reemplaza la función anidada, que es
 * una extensión de GCC no portable).
 */
static void OLED_HLine(OLED_Config_t* config, int16_t x1, int16_t x2, int16_t y, uint8_t color) {
	if (x1 > x2) {
		int16_t temp = x1;
		x1 = x2;
		x2 = temp;
	}
	for (int16_t x = x1; x <= x2; x++) {
		OLED_DrawPixel(config, x, y, color);
	}
}

/* ========================================================================== *
 * Inicialización y control del display                                       *
 * ========================================================================== */

/*
 * Resetea (si hay pin), configura los registros del controlador y limpia pantalla y framebuffer.
 */
bool OLED_Init(OLED_Config_t* config) {
	/* Validación de punteros obligatorios. */
	if (config == NULL || config->hal_i2c_handle == NULL) {
		return false;
	}

	config->framebuffer = oled_framebuffer;

	/* Dimensiones efectivas según la resolución elegida. */
	if (config->resolution == OLED_RESOLUTION_128x32) {
		config->width = 128;
		config->height = 32;
	} else if (config->resolution == OLED_RESOLUTION_128x64) {
		config->width = 128;
		config->height = 64;
	} else {
		return false;
	}

	/* Pulso de RESET por hardware (opcional): nivel bajo y luego alto. */
	if (config->rst_port != NULL) {
		OLED_HAL_GPIO_WritePin(config->rst_port, config->rst_pin, GPIO_PIN_RESET);
		OLED_HAL_Delay(10);
		OLED_HAL_GPIO_WritePin(config->rst_port, config->rst_pin, GPIO_PIN_SET);
		OLED_HAL_Delay(10);
	}

	/* ---- Secuencia de inicialización del controlador ---- */
	OLED_SendCommand(config, 0xAE);                    /* Display OFF mientras se configura */
	OLED_SendCommand(config, 0xD5);                    /* Set Display Clock Divide Ratio / Frecuencia del oscilador */
	OLED_SendCommand(config, 0x80);                    /*   valor por defecto recomendado */
	OLED_SendCommand(config, 0xA8);                    /* Set Multiplex Ratio */
	OLED_SendCommand(config, config->height - 1);      /*   alto - 1 (0x1F para 32, 0x3F para 64) */
	OLED_SendCommand(config, 0xD3);                    /* Set Display Offset */
	OLED_SendCommand(config, 0x00);                    /*   sin desplazamiento vertical */
	OLED_SendCommand(config, 0x40);                    /* Set Display Start Line = 0 */
	OLED_SendCommand(config, 0x8D);                    /* Charge Pump Setting (generador interno de alta tensión) */
	OLED_SendCommand(config, 0x14);                    /*   habilitado (necesario alimentando a 3.3 V) */
	OLED_SendCommand(config, 0x20);                    /* Set Memory Addressing Mode */
	OLED_SendCommand(config, 0x02);                    /*   Page Addressing: se elige página y columna manualmente */
	OLED_SendCommand(config, 0xA1);                    /* Segment Re-map: invierte el orden de columnas (espejo horizontal) */
	OLED_SendCommand(config, 0xC8);                    /* COM Scan Direction: invierte el orden de filas (espejo vertical) */
	OLED_SendCommand(config, 0xDA);                    /* Set COM Pins Hardware Configuration */

	if (config->resolution == OLED_RESOLUTION_128x32) {
		OLED_SendCommand(config, 0x02);                /*   configuración secuencial para 128x32 */
	} else {
		OLED_SendCommand(config, 0x12);                /*   configuración alternativa para 128x64 */
	}

	OLED_SendCommand(config, 0x81);                    /* Set Contrast Control */
	OLED_SendCommand(config, 0xFF);                    /*   brillo máximo */
	OLED_SendCommand(config, 0xD9);                    /* Set Pre-charge Period */
	OLED_SendCommand(config, 0xF1);
	OLED_SendCommand(config, 0xDB);                    /* Set VCOMH Deselect Level */
	OLED_SendCommand(config, 0x40);
	OLED_SendCommand(config, 0xA4);                    /* Entire Display ON: muestra el contenido de la GDDRAM */
	OLED_SendCommand(config, 0xA6);                    /* Display normal (sin inversión) */
	OLED_SendCommand(config, 0x2E);                    /* Desactiva el scroll por hardware */
	OLED_SendCommand(config, 0xAF);                    /* Display ON */

	OLED_Clear(config, true);
	return true;
}

/*
 * Pone en cero todo el framebuffer y, opcionalmente, actualiza la pantalla.
 */
void OLED_Clear(OLED_Config_t* config, bool clear_display) {
	memset(config->framebuffer, 0, OLED_FRAMEBUFFER_SIZE);
	if (clear_display) {
		OLED_UpdateDisplay(config);
	}
}

/*
 * Vuelca el framebuffer a la GDDRAM recorriendo las páginas (8 filas de píxeles cada una).
 * Para cada página: se elige la página, se fija la columna inicial y se envían 'width' bytes.
 */
void OLED_UpdateDisplay(OLED_Config_t* config) {
	uint8_t i;
	for (i = 0; i < config->height / 8; i++) {
		OLED_SendCommand(config, 0xB0 + i);        /* Selecciona la página i (0xB0..0xB7) */

		/* Dirección de columna inicial = nibble bajo (0x0X) + nibble alto (0x1X). */
		if (config->driver_type == OLED_DRIVER_SH1106) {
			/* El SH1106 tiene 132 columnas de RAM y el panel de 128 queda centrado: offset de 2. */
			OLED_SendCommand(config, 0x02);
			OLED_SendCommand(config, 0x10);
		} else {
			OLED_SendCommand(config, 0x00);
			OLED_SendCommand(config, 0x10);
		}
		OLED_SendData(config, &config->framebuffer[config->width * i], config->width);
	}
}

/* ========================================================================== *
 * Primitivas gráficas                                                        *
 * ========================================================================== */

/*
 * Enciende o apaga un píxel. Cada byte del framebuffer es una columna de 8 píxeles:
 *   índice = x + (y / 8) * width   |   bit = y % 8
 * Los puntos fuera de pantalla se descartan.
 */
void OLED_DrawPixel(OLED_Config_t* config, int16_t x, int16_t y, uint8_t color) {
	if (x >= 0 && x < config->width && y >= 0 && y < config->height) {
		if (color == OLED_COLOR_WHITE) {
			config->framebuffer[x + (y / 8) * config->width] |= (1 << (y % 8));
		} else {
			config->framebuffer[x + (y / 8) * config->width] &= ~(1 << (y % 8));
		}
	}
}

/*
 * Línea por el algoritmo de Bresenham (solo aritmética entera, sin divisiones).
 * 'err' acumula el error entre la línea ideal y el píxel elegido.
 */
void OLED_DrawLine(OLED_Config_t* config, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
	int16_t dx = abs(x1 - x0);
	int16_t sx = x0 < x1 ? 1 : -1;   /* Sentido de avance en X */
	int16_t dy = -abs(y1 - y0);      /* Negativo por convención del algoritmo */
	int16_t sy = y0 < y1 ? 1 : -1;   /* Sentido de avance en Y */
	int16_t err = dx + dy;
	int16_t e2;

	for (;;) {
		OLED_DrawPixel(config, x0, y0, color);
		if (x0 == x1 && y0 == y1) break;   /* Llegó al punto final */
		e2 = 2 * err;
		if (e2 >= dy) {                    /* Avanza en X */
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {                    /* Avanza en Y */
			err += dx;
			y0 += sy;
		}
	}
}

/*
 * Rectángulo vacío (4 líneas) o relleno (filas horizontales recortadas a la pantalla).
 * Para el contorno NO se recorta antes de dibujar: así, si el rectángulo se sale de la
 * pantalla, no aparecen bordes falsos en el límite (DrawPixel ya descarta lo externo).
 */
void OLED_DrawRectangle(OLED_Config_t* config, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color, bool filled) {
	if (w <= 0 || h <= 0) {
		return;
	}

	int16_t x1 = x + w - 1;   /* Esquina inferior derecha (inclusive) */
	int16_t y1 = y + h - 1;

	if (filled) {
		/* Se recorta al área visible para no recorrer píxeles inútiles. */
		int16_t xs = (x < 0) ? 0 : x;
		int16_t ys = (y < 0) ? 0 : y;
		int16_t xe = (x1 >= config->width)  ? config->width - 1  : x1;
		int16_t ye = (y1 >= config->height) ? config->height - 1 : y1;

		for (int16_t cy = ys; cy <= ye; cy++) {
			OLED_HLine(config, xs, xe, cy, color);
		}
	} else {
		OLED_DrawLine(config, x,  y,  x1, y,  color);   /* Lado superior */
		OLED_DrawLine(config, x,  y1, x1, y1, color);   /* Lado inferior */
		OLED_DrawLine(config, x,  y,  x,  y1, color);   /* Lado izquierdo */
		OLED_DrawLine(config, x1, y,  x1, y1, color);   /* Lado derecho */
	}
}

/*
 * Triángulo (solo contorno): une los tres vértices con líneas.
 */
void OLED_DrawTriangle(OLED_Config_t* config, int16_t x1, int16_t y1, int16_t x2, int16_t y2, int16_t t_x3, int16_t t_y3, uint8_t color) {
	OLED_DrawLine(config, x1, y1, x2, y2, color);
	OLED_DrawLine(config, x2, y2, t_x3, t_y3, color);
	OLED_DrawLine(config, t_x3, t_y3, x1, y1, color);
}

/*
 * Círculo (contorno) por el algoritmo del punto medio. Se calcula un solo octante
 * y se replica por simetría en los otros 7. 'd' es la variable de decisión.
 */
void OLED_DrawCircle(OLED_Config_t* config, int16_t x0, int16_t y0, int16_t radius, uint8_t color) {
	int16_t x = 0;
	int16_t y = radius;
	int16_t d = 3 - 2 * radius;

	while (y >= x) {
		/* Los 8 puntos simétricos */
		OLED_DrawPixel(config, x0 + x, y0 + y, color);
		OLED_DrawPixel(config, x0 - x, y0 + y, color);
		OLED_DrawPixel(config, x0 + x, y0 - y, color);
		OLED_DrawPixel(config, x0 - x, y0 - y, color);
		OLED_DrawPixel(config, x0 + y, y0 + x, color);
		OLED_DrawPixel(config, x0 - y, y0 + x, color);
		OLED_DrawPixel(config, x0 + y, y0 - x, color);
		OLED_DrawPixel(config, x0 - y, y0 - x, color);

		if (d < 0) {                       /* El punto medio queda dentro: solo avanza X */
			d = d + 4 * x + 6;
		} else {                           /* Queda fuera: avanza X y baja Y */
			d = d + 4 * (x - y) + 10;
			y--;
		}
		x++;
	}
}

/*
 * Círculo relleno: misma iteración que el contorno, pero en lugar de 8 píxeles
 * traza 4 líneas horizontales simétricas que cubren el interior.
 */
void OLED_DrawFilledCircle(OLED_Config_t* config, int16_t x0, int16_t y0, int16_t radius, uint8_t color) {
	int16_t x = 0;
	int16_t y = radius;
	int16_t d = 3 - 2 * radius;

	while (y >= x) {
		OLED_HLine(config, x0 - x, x0 + x, y0 + y, color);
		OLED_HLine(config, x0 - x, x0 + x, y0 - y, color);
		OLED_HLine(config, x0 - y, x0 + y, y0 + x, color);
		OLED_HLine(config, x0 - y, x0 + y, y0 - x, color);

		if (d < 0) {
			d = d + 4 * x + 6;
		} else {
			d = d + 4 * (x - y) + 10;
			y--;
		}
		x++;
	}
}

/* ========================================================================== *
 * Texto                                                                      *
 * ========================================================================== */

/*
 * Dibuja un carácter. Soporta dos formatos de fuente:
 *  - 6x8: una entrada por COLUMNA, bit 0 = fila superior.
 *  - Resto: una entrada de 16 bits por FILA, bit 15 = columna izquierda (width <= 16).
 * Solo se encienden los píxeles del glifo (fondo transparente).
 */
void OLED_DrawChar(OLED_Config_t* config, int16_t x, int16_t y, char character, const FontDef *font, uint8_t color) {
	/* Descarta lo que está totalmente fuera de pantalla y los caracteres no imprimibles. */
	if (x >= config->width || y >= config->height || character < 32 || character > 126) return;

	if (font->width == 6 && font->height == 8) {
		/* Formato por columnas */
		uint16_t char_data_offset = (character - 32) * font->width;
		for (int16_t col_idx = 0; col_idx < font->width; col_idx++) {
			uint8_t line = font->data[char_data_offset + col_idx];
			for (int16_t row_idx = 0; row_idx < font->height; row_idx++) {
				if ((line >> row_idx) & 0x01) {
					OLED_DrawPixel(config, x + col_idx, y + row_idx, color);
				}
			}
		}
	}
	else {
		/* Formato por filas de 16 bits */
		uint16_t char_data_offset = (character - 32) * font->height;
		for (int16_t row_idx = 0; row_idx < font->height; row_idx++) {
			uint16_t font_row_data = font->data[char_data_offset + row_idx];
			for (int16_t col_idx = 0; col_idx < font->width; col_idx++) {
				if ((font_row_data >> (15 - col_idx)) & 0x01) {
					OLED_DrawPixel(config, x + col_idx, y + row_idx, color);
				}
			}
		}
	}
}

/*
 * Dibuja una cadena con 1 px de separación entre letras y 2 px entre líneas.
 * Si el siguiente carácter no entra en el ancho, baja de línea (vuelve a 'x');
 * si tampoco entra en alto, se corta el texto.
 */
void OLED_DrawString(OLED_Config_t* config, int16_t x, int16_t y, const char* str, const FontDef *font, uint8_t color) {
	int16_t currentX = x;

	while (*str) {
		char c = *str++;
		if (c < 32 || c > 126) {
			continue;                              /* Omite caracteres no soportados */
		}

		/* Salto de línea automático ANTES de dibujar, para no dejar letras cortadas. */
		if (currentX + font->width > config->width) {
			currentX = x;
			y += font->height + 2;
			if (y + font->height > config->height) {
				break;                             /* No queda espacio vertical */
			}
		}

		OLED_DrawChar(config, currentX, y, c, font, color);
		currentX += font->width + 1;               /* Avanza el ancho del glifo + 1 px de espacio */
	}
}

/* ========================================================================== *
 * Efectos de pantalla                                                        *
 * ========================================================================== */

/*
 * Scroll a la izquierda por software: en cada página se desplazan los bytes (columnas)
 * con memmove y se rellena con ceros el borde derecho. Luego se actualiza la pantalla.
 */
void OLED_ScrollLeft_Software(OLED_Config_t* config, uint8_t pixels_to_scroll) {
	if (pixels_to_scroll == 0) return;
	if (pixels_to_scroll > config->width) pixels_to_scroll = config->width;   /* Evita tamaños negativos */

	for (uint8_t page = 0; page < config->height / 8; page++) {
		uint16_t page_start_idx = page * config->width;
		memmove(&config->framebuffer[page_start_idx],
				&config->framebuffer[page_start_idx + pixels_to_scroll],
				config->width - pixels_to_scroll);
		memset(&config->framebuffer[page_start_idx + (config->width - pixels_to_scroll)],
				0x00, pixels_to_scroll);
	}
	OLED_UpdateDisplay(config);
}

/*
 * Scroll a la derecha por software: se copia de derecha a izquierda para no pisar datos
 * que aún no se movieron, y se rellena con ceros el borde izquierdo.
 */
void OLED_ScrollRight_Software(OLED_Config_t* config, uint8_t pixels_to_scroll) {
	if (pixels_to_scroll == 0) return;
	if (pixels_to_scroll > config->width) pixels_to_scroll = config->width;   /* Evita desbordes en memset */

	for (uint8_t page = 0; page < config->height / 8; page++) {
		uint16_t page_start_idx = page * config->width;
		for (int16_t col = config->width - 1; col >= pixels_to_scroll; col--) {
			config->framebuffer[page_start_idx + col] = config->framebuffer[page_start_idx + col - pixels_to_scroll];
		}
		memset(&config->framebuffer[page_start_idx], 0x00, pixels_to_scroll);
	}
	OLED_UpdateDisplay(config);
}

/*
 * Inversión de color por hardware: no toca el framebuffer, solo cambia cómo el
 * controlador interpreta la GDDRAM.
 */
void OLED_InvertDisplay(OLED_Config_t* config, bool invert) {
	if (invert) {
		OLED_SendCommand(config, 0xA7);   /* Display invertido (1 = apagado, 0 = encendido) */
	} else {
		OLED_SendCommand(config, 0xA6);   /* Display normal */
	}
}

/*
 * Dibuja un bitmap monocromo EN EL FRAMEBUFFER (antes escribía directo a la pantalla y
 * se perdía en el siguiente OLED_UpdateDisplay). Formato: por páginas, bit 0 = píxel superior.
 * Acepta cualquier posición (x, y) con recorte automático.
 */
void OLED_DrawBitmap(OLED_Config_t* config, uint8_t x, uint8_t y, const uint8_t *bitmap, uint8_t width, uint8_t height) {
	if (bitmap == NULL || height % 8 != 0) {
		return;
	}

	uint8_t pages = height / 8;
	for (uint8_t page = 0; page < pages; page++) {
		for (uint8_t col = 0; col < width; col++) {
			uint8_t byte = bitmap[page * width + col];
			for (uint8_t bit = 0; bit < 8; bit++) {
				OLED_DrawPixel(config, x + col, y + page * 8 + bit,
						((byte >> bit) & 0x01) ? OLED_COLOR_WHITE : OLED_COLOR_BLACK);
			}
		}
	}
}

/* ========================================================================== *
 * Rutina de prueba                                                           *
 * ========================================================================== */

/*
 * Recorre todas las funciones del driver para validarlas visualmente.
 * Se adapta al alto del display (32 o 64 px). Es bloqueante.
 */
void OLED_Test(OLED_Config_t* config) {
	/* 1. Presentación */
	OLED_Clear(config, true);
	OLED_DrawString(config, 0, 0, "UTN FRT - TD2", &Font_7x10, OLED_COLOR_WHITE);
	if (config->height > 32) {
		OLED_DrawString(config, 0, 12, "NUCLEO F439ZI", &Font_7x10, OLED_COLOR_WHITE);
		OLED_DrawString(config, 0, 24, "Test OLED 128x64", &Font_6x8, OLED_COLOR_WHITE);
	} else {
		OLED_DrawString(config, 0, 12, "OLED 128x32 OK", &Font_6x8, OLED_COLOR_WHITE);
	}
	OLED_UpdateDisplay(config);
	OLED_HAL_Delay(OLED_TEST_DELAY_MS * 2);

	/* 2. Inversión de color (normal vs. invertido) */
	OLED_Clear(config, true);
	OLED_DrawString(config, 0, 4, "Normal...", &Font_7x10, OLED_COLOR_WHITE);
	OLED_UpdateDisplay(config);
	OLED_HAL_Delay(OLED_TEST_DELAY_MS);

	OLED_Clear(config, true);
	OLED_InvertDisplay(config, true);
	OLED_DrawString(config, 0, 4, "Invertido...", &Font_7x10, OLED_COLOR_WHITE);
	OLED_UpdateDisplay(config);
	OLED_HAL_Delay(OLED_TEST_DELAY_MS * 2);

	OLED_Clear(config, true);
	OLED_InvertDisplay(config, false);   /* Restablece el modo normal */

	/* 3. Scroll por software: izquierda y luego derecha */
	OLED_Clear(config, true);
	OLED_DrawString(config, 0, 0,  "A Izquierda! ", &Font_6x8, OLED_COLOR_WHITE);
	OLED_DrawString(config, 0, 10, " Tu mama... ", &Font_6x8, OLED_COLOR_WHITE);
	if (config->height > 32) {
		OLED_DrawString(config, 0, 20, " Sera mi suegra ", &Font_6x8, OLED_COLOR_WHITE);
	}
	OLED_UpdateDisplay(config);
	OLED_HAL_Delay(OLED_TEST_DELAY_MS);

	for (int k = 0; k < config->width; k++) {
		OLED_ScrollLeft_Software(config, 1);   /* 1 px por paso hasta vaciar la pantalla */
		OLED_HAL_Delay(5);
	}
	OLED_HAL_Delay(OLED_TEST_DELAY_MS);

	OLED_Clear(config, true);
	OLED_DrawString(config, 0, 0,  "A Derecha! ", &Font_6x8, OLED_COLOR_WHITE);
	OLED_DrawString(config, 0, 10, " Y tu papa... ", &Font_6x8, OLED_COLOR_WHITE);
	if (config->height > 32) {
		OLED_DrawString(config, 0, 20, " Sera mi suegro ", &Font_6x8, OLED_COLOR_WHITE);
	}
	OLED_UpdateDisplay(config);
	OLED_HAL_Delay(OLED_TEST_DELAY_MS);

	for (int k = 0; k < config->width; k++) {
		OLED_ScrollRight_Software(config, 1);
		OLED_HAL_Delay(5);
	}
	OLED_HAL_Delay(OLED_TEST_DELAY_MS);

	/* 4. Píxeles en las cuatro esquinas y una línea diagonal */
	OLED_Clear(config, true);
	OLED_DrawString(config, 0, 0, "Lineas y Pixeles", &Font_6x8, OLED_COLOR_WHITE);
	OLED_DrawPixel(config, 0, 10, OLED_COLOR_WHITE);
	OLED_DrawPixel(config, config->width - 1, 10, OLED_COLOR_WHITE);
	OLED_DrawPixel(config, 0, config->height - 1, OLED_COLOR_WHITE);
	OLED_DrawPixel(config, config->width - 1, config->height - 1, OLED_COLOR_WHITE);
	OLED_DrawLine(config, 5, 12, config->width - 5, config->height - 2, OLED_COLOR_WHITE);
	OLED_UpdateDisplay(config);
	OLED_HAL_Delay(OLED_TEST_DELAY_MS * 2);

	/* 5. Catálogo de fuentes */
	OLED_Clear(config, true);
	if (config->height == 32) {
		OLED_DrawString(config, 0, 0,  "6x8: ABC 123",  &Font_6x8,   OLED_COLOR_WHITE);
		OLED_DrawString(config, 0, 9,  "7x10: ABC 123", &Font_7x10,  OLED_COLOR_WHITE);
		OLED_DrawString(config, 0, 20, "11x18: AB",     &Font_11x18, OLED_COLOR_WHITE);
	}
	else {
		OLED_DrawString(config, 0, 0,  "6x8:",   &Font_6x8,   OLED_COLOR_WHITE);
		OLED_DrawString(config, 0, 9,  "7x10:",  &Font_7x10,  OLED_COLOR_WHITE);
		OLED_DrawString(config, 0, 20, "11x18:", &Font_11x18, OLED_COLOR_WHITE);
		OLED_DrawString(config, 0, 38, "16x26:", &Font_16x26, OLED_COLOR_WHITE);
	}
	OLED_UpdateDisplay(config);
	OLED_HAL_Delay(OLED_TEST_DELAY_MS * 3);

	/* 6. Cierre, con el mensaje centrado verticalmente según el alto */
	OLED_Clear(config, true);
	int16_t y_pos = (config->height <= 32) ? 11 : 27;
	OLED_DrawString(config, 0, y_pos, "Test Completo!", &Font_7x10, OLED_COLOR_WHITE);
	OLED_UpdateDisplay(config);
	OLED_HAL_Delay(OLED_TEST_DELAY_MS * 2);

	OLED_Clear(config, true);   /* Deja la pantalla limpia */
}
