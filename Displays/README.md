# Categoría: Displays

Esta carpeta contiene la colección de drivers y módulos de control para **pantallas alfanuméricas, matriciales de 7 segmentos y visores gráficos** (OLED / TFT) en microcontroladores STM32 utilizando la capa **ST HAL**.

Todos los drivers están optimizados para operar sin bloquear el bucle principal (`while(1)`), integrando formateo de caracteres, gráficos de pantalla o temporizaciones precisas por hardware/software.

## 📦 Drivers Disponibles en esta Categoría

| Driver / Módulo | Interfaz / Periférico | Dependencia `Utils` | Versión | Estado | Descripción Principal | 
| ----- | ----- | ----- | ----- | ----- | ----- | 
| [**`Display_TM1637`**](Display_TM1637/) | Bit-Banging (GPIO OD) | **Sí (`utils.h`)** | `v1.2` | **Estable** | Display de 7 segmentos (4/6 dígitos). Detección ACK, reconexión en caliente (*hot-plug*), reloj $HH:MM$ y ceros a la izquierda. | 
| [**`LCD_4Bits`**](LCD_4Bits/) | GPIO (4 Bits directos) | **Sí (`utils.h`)** | `v2.1` | **Estable** | Driver paralelo directo de 4 bits para pantallas alfanuméricas HD44780 (16x2, 20x4) con retardos precisos. | 
| [**`LCD_I2C`**](LCD_I2C/) | I2C (PCF8574) | **Sí (`utils.h`)** | `v2.1` | **Estable** | Interfaz I2C para pantallas HD44780 mediante expansor de puerto PCF8574 y retardo de microsegundos. | 
| [**`OLED_I2C`**](OLED_I2C/) | I2C (SSD1306) | No | `v1.1` | **Estable** | Pantalla OLED gráfica de $128\times64$ píxeles con buffer de pantalla (*framebuffer*), renderizado de texto y primitivas gráficas. | 
| [**`TFT_ST7735`**](TFT_ST7735/) | SPI + GPIO (DC/RST/CS) | No | `v2.3` | **Estable** | Pantalla gráfica a color TFT de alta velocidad mediante bus SPI con soporte para fuentes personalizadas y gráficos. | 

## ⚠️ Dependencia del Módulo `utils_STM32`

Los drivers de pantallas alfanuméricas por bit-banging o comandos de temporización sensible (**`Display_TM1637`**, **`LCD_4Bits`** y **`LCD_I2C`**) requieren la inclusión del módulo auxiliar **`utils_STM32`** (`Dependencias/Utils/utils_STM32.h`) para ejecutar los tiempos de *hold/setup* de señal o el envío de pulsos Enable/Clock en microsegundos ($\mu\text{s}$).

Asegúrate de inicializar el contador de microsegundos al arrancar el programa en `main.c`:

```c
#include "main.h"
#include "utils.h"

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();

    /* Inicialización del módulo de microsegundos requerido por los displays */
    Utils_DelayUs_Init();

    /* Inicialización de los drivers de pantalla... */
}
```

## 🚀 Estructura de las Carpetas

Cada pantalla cuenta con su propio subdirectorio autocontenido:

```
Displays/
├── Display_TM1637/
│   ├── tm1637_STM32.h
│   ├── tm1637_STM32.c
│   └── README.md
├── LCD_4Bits/
│   ├── lcd_4bits_STM32.h
│   ├── lcd_4bits_STM32.c
│   └── README.md
├── LCD_I2C/
│   ├── lcd_i2c_STM32.h
│   ├── lcd_i2c_STM32.c
│   └── README.md
├── OLED_I2C/
│   ├── OLED_Fonts_STM32.h
│   ├── OLED_Fonts_STM32.c
│   ├── oled_i2c_STM32.h
│   ├── oled_i2c_STM32.c
│   └── README.md
├── TFT_ST7735/
│   ├── tft_fonts_STM32.h
│   ├── tft_fonts_STM32.c
│   ├── tft_graphics_STM32.h
│   ├── tft_graphics_STM32.c
│   ├── tft_st7735_STM32.h
│   ├── tft_st7735_STM32.c
│   └── README.md
└── README.md  <-- (Este archivo índice)
```

## 📜 Licencia y Soporte Académico

* **Autor:** Mamani Flores Carlos (UTN FRT)
* **Materia:** Técnicas Digitales 2 — Universidad Tecnológica Nacional (FRT)
* **Licencia:** **MIT** (Consúltese el archivo [LICENSE](../../LICENSE) en la raíz del repositorio).