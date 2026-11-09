# Categoría: Dependencias

Esta carpeta alberga los **módulos auxiliares, librerías base y herramientas de soporte** requeridas por diversos drivers del repositorio para garantizar su correcto funcionamiento en microcontroladores STM32.

Las dependencias aquí contenidas están optimizadas para ofrecer soporte portable, atómico y transparente a lo largo de las distintas familias Cortex-M (Cortex-M0, M0+, M3, M4, M7 y M33).

## 📦 Módulos Disponibles en esta Categoría

| Módulo / Librería | Subdirectorio | Versión | Estado | Descripción Principal |
| :--- | :--- | :---: | :---: | :--- |
| **[`Utils`](Utils/)** | `Dependencias/Utils/` | `v2.1` | **Estable** | Módulo de retardo bloqueante en microsegundos ($\mu\text{s}$) atómico y portable con fallback automático entre DWT (`CYCCNT`) y `SysTick`. |

## ⚙️ Uso de Dependencias en el Repositorio

Los siguientes drivers del repositorio dependen explícitamente de los servicios de temporización en microsegundos provistos por **`utils.h`**:

- **`Display_TM1637`** (Bit-Banging de alta velocidad): Requiere `Utils` para temporizar las señales de reloj $CLK$ y los pulsos de datos $DIO$.
- **`LCD_I2C`** (HD44780 sobre PCF8574): Utiliza `Utils` para generar los tiempos de *latch* del pin Enable ($E$) y los retardos de comandos del display.
- **`LCD_4Bits`** (HD44780 en modo paralelo directo): Emplea `Utils` para garantizar los tiempos de *setup/hold* de los buses de datos de 4 bits.

## 🚀 Estructura del Directorio

```text
Dependencias/
├── Utils/
│   ├── docs/
│   ├── utils.c
│   ├── utils.h
│   └── README.md
└── README.md  <-- (Este archivo índice)
```

## 📜 Licencia y Soporte Académico

- **Autor:** Mamani Flores Carlos (UTN FRT)
- **Materia:** Técnicas Digitales 2 — Universidad Tecnológica Nacional (FRT)
- **Licencia:** **MIT** (Consúltese el archivo [LICENSE](../LICENSE) en la raíz del repositorio).