# cpp-thermal-watch — Thermal Watch (Linux)

Monitor térmico de **Linux** en **C++23** que lee `/sys/class/thermal/` y `cpufreq` para mostrar en una pantalla: zonas térmicas con temperatura en °C (milligrados de sysfs), dispositivos de enfriamiento (cooling devices) con su estado de throttle (cur_state/max_state), y la frecuencia/gobernador actual de cada núcleo de CPU. Cero dependencias externas: compilación directa con `g++`.

## Features

- **Zonas térmicas** desde `/sys/class/thermal/thermal_zone*/{temp,type,policy}`: temperatura en °C (parseo de milligrados `45000` → `45.0`), tipo de sensor (`x86_pkg_temp`, `acpitz`, ...) y política (`step_wise`, `user_space`, ...)
- **Colores por temperatura**: verde < 60 °C, amarillo 60–umbral, rojo ≥ umbral (default 80 °C, ajustable con `--threshold N`)
- **Cooling devices** desde `/sys/class/thermal/cooling_device*/{type,max_state,cur_state}`: throttle % (cur/max) con barra de progreso y semáforo (rojo ≥80 %, amarillo ≥50 %)
- **Frecuencia de CPU** desde `/sys/devices/system/cpu/cpu*/cpufreq/`: gobernador (`powersave`, `performance`, ...), frecuencia actual y máxima por núcleo en GHz
- **Modo watch** (`--watch N`): refresco en vivo cada N segundos con limpieza de pantalla y `std::flush` (salida completa incluso con `timeout`/kill)
- Salida **JSON** completa para scripting (`--json`): `{zones[], cooling_devices[], cpus[], summary}`
- Manejo elegante de sistemas sin zonas térmicas (VMs/contenedores): mensajes explícitos, no crashea
- Colores ANSI desactivables (`--no-color`) para pipes/logs
- 16 tests de auto-verificación (`--selftest`): parseo sintético de milligrados/ints, formateo, throttle, barras, colores y smoke tests contra sysfs real

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o thermal-watch
```

## Usage

```bash
./thermal-watch                # snapshot: zonas + coolers + CPU
./thermal-watch --watch 2      # refresco en vivo cada 2s
./thermal-watch --threshold 70 # alerta roja desde 70 °C
./thermal-watch --json         # JSON completo para scripting
./thermal-watch --no-color     # sin ANSI (pipes/logs)
./thermal-watch --selftest     # auto-verificación
```

## Ejemplo (portátil con sensores reales)

```
THERMAL WATCH — Linux thermal monitor  (/sys/class/thermal + cpufreq)
------------------------------------------------------------------------------
  THERMAL ZONES (2)
   ZONE  TYPE                         TEMP     POLICY
     0   x86_pkg_temp                 67.0 C   step_wise
     1   acpitz                       45.0 C   user_space

  COOLING DEVICES (4)
   DEV  TYPE                         CUR/MAX   THROTTLE  BAR
     0   Processor                      0/100      0.0%  ................
     1   Processor                     45/100     45.0%  |||||||||.......
     2   intel_powerclamp               0/50       0.0%  ................

  CPU FREQUENCY (8 cores, governor: powersave)
   CPU  GOV          CUR        MAX
     0  powersave  2.40GHz   4.80GHz
     1  powersave  2.40GHz   4.80GHz

  SUMMARY  zones 2   coolers 4   cores 8   max temp 67.0 C
```

En VMs/contenedores sin sensores, el tool lo indica explícitamente (`no /sys/class/thermal/thermal_zone* on this system`) y muestra solo lo disponible.

## Estructura

```
src/main.cpp   — todo el código (monolítico, stdlib-only)
```

## Notas técnicas

- `temp` de sysfs está en **milligrados** (`45000` = 45.0 °C) — parseo con `std::from_chars` (que no salta whitespace: se trimea antes).
- `throttle_pct = cur_state / max_state * 100`; si `max_state == 0` el dispositivo está inactivo (0 %).
- Los tests de integración tratan la ausencia de zonas térmicas como caso válido (condicional), nunca como fallo.
