# Categoría: Actuadores

Esta carpeta contiene la colección de drivers y módulos de control para **elementos de salida, actuadores de potencia y señalización** en microcontroladores STM32 utilizando la capa **ST HAL**.

Todos los drivers están desarrollados bajo la premisa de **ejecución no bloqueante**, modulando señales de salida directa por GPIO o utilizando periféricos de hardware dedicados.

## 📦 Drivers Disponibles en esta Categoría

| Driver / Módulo | Interfaz / Periférico | Versión | Estado | Descripción Principal |
| :--- | :--- | :---: | :---: | :--- |
| **[`LED_RGB`](LED_RGB/)** | GPIO / PWM (Timer HW) | `v1.2` | **Estable** | Driver no bloqueante para LEDs RGB (Cátodo/Ánodo Común), control digital, mezcla de colores y efectos de respiración/parpadeo. |
| **[`Servo`](Servo/)** | PWM (Timer HW) | `v2.0` | **Estable** | Control de servomotores estándar ($50\text{ Hz}$ / $20\text{ ms}$) con mapeo de ángulos ($0^\circ \dots 180^\circ$) y suavizado de movimiento (*easing*). |

## 🚀 Estructura de las Carpetas

Cada actuador cuenta con su propio subdirectorio autocontenido:

```text
Actuadores/
├── LED_RGB/
│   ├── rgb_gamma_lut_STM32
│   ├── led_rgb_STM32.h
│   ├── led_rgb_STM32.c
│   └── README.md
├── Servo/
│   ├── servo_STM32.h
│   ├── servo_STM32.c
│   └── README.md
└── README.md  <-- (Este archivo índice)
```

## 📜 Licencia y Soporte Académico

- **Autor:** Mamani Flores Carlos (UTN FRT)
- **Materia:** Técnicas Digitales 2 — Universidad Tecnológica Nacional (FRT)
- **Licencia:** **MIT** (Consúltese el archivo [LICENSE](../../LICENSE) en la raíz del repositorio).