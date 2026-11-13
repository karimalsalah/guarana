**Idiomas:** [English](../../README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português (Brasil)](README.pt-BR.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Índice](README.md)

**Discord:** [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM)

# Guarana

**Emulación de Nintendo Switch 2 ajustada para GPU integradas.**

Guarana es una bifurcación de código abierto de [Lichii](https://github.com/zortos293/Lichii) (rama `nx2`) para Windows x64. Está pensada para portátiles y consolas portátiles con gráficos integrados: primero Intel Arc y después AMD Radeon. Se centra en un ritmo de imagen más uniforme, la generación de fotogramas, un arranque rápido y un reparto más eficiente del presupuesto de energía.

> [!IMPORTANT]
> Guarana **no incluye** claves, firmware, juegos ni archivos del sistema. Necesitas los tuyos, extraídos de una consola y de juegos que poseas. No está afiliado a Nintendo ni al proyecto Lichii. Consulta el aviso legal más abajo.

## Qué es

- Un emulador gratuito y de código abierto bajo la licencia GPL-3.0-or-later.
- Ajustado para GPU integradas con memoria compartida y energía limitada.
- Requiere Windows x64, una CPU con AVX2 y un driver de GPU actualizado.
- No incluye claves, firmware, juegos ni archivos del sistema.

## Resultado medido

En un portátil con Core Ultra 9 285H, GPU integrada Arc 140T, driver de Intel 32.0.101.9175 y 32 GB de RAM, la corrección del enrutamiento de memoria global (`EDEN_NX2_GM=11`) subió Hyrule Field, en *Ocarina of Time*, de **14,8 fps** a **22,8 fps** de juego. Se midió en el mismo punto con el interruptor activado y desactivado, con la generación de fotogramas apagada.

Es un único dato de un solo equipo, un juego y una escena. Otros dispositivos, drivers, juegos y escenas no se han medido. Puedes [ayudar a medir](../../docs/BENCHMARKING.md).

## Estado de compatibilidad

Solo se ha probado la Arc 140T. El resto de GPU (Arc 140V, Arc B390, Radeon 780M, 880M, 890M y otras) no se han probado todavía. Tu [informe de dispositivo](../../docs/DEVICES.md#send-a-device-report) cuenta. Los equipos con 16 GB de memoria compartida están por debajo del mínimo recomendable; consulta [DEVICES.md](../../docs/DEVICES.md).

## Inicio rápido

1. Descarga el zip más reciente desde [Releases](https://github.com/karimalsalah/guarana/releases). Esa página es la única descarga oficial.
2. Extrae el zip completo en una carpeta en la que puedas escribir y ejecuta **`guarana.exe`**.
3. Instala tus claves y firmware: **Tools → Install Decryption Keys** y después **Tools → Install Firmware**.
4. Añade tu carpeta de juegos (doble clic en la lista vacía) e inicia un juego. La primera vez que entras en cada zona se compilan los shaders y hay tirones; después se cargan desde la caché.
5. Opcional: activa **Enable Frame Generation** en **Emulation → Configure… → Graphics**.

La configuración, las partidas guardadas y las cachés están en `%APPDATA%\guarana`. En el primer inicio, tus ajustes se trasladan automáticamente desde la carpeta antigua `%APPDATA%\lichii` (la carpeta antigua se queda donde está). Todos los interruptores están en [docs/switches.md](../../docs/switches.md) y los problemas habituales en [TROUBLESHOOTING.md](../../docs/TROUBLESHOOTING.md).

## Claves, firmware y juegos (aviso legal)

- Usa tus propias claves, firmware y juegos, extraídos de hardware que poseas. Guarana no los incluye y no se aceptan en issues, PR ni discusiones.
- No se atienden solicitudes ni enlaces de archivos protegidos por derechos de autor, ni preguntas sobre cómo obtenerlos.
- Guarana es un proyecto independiente. No está afiliado ni respaldado por Nintendo ni por el proyecto Lichii. "Nintendo" y "Nintendo Switch" son marcas de Nintendo, citadas solo para indicar con qué es compatible el software. Texto completo: [LEGAL.md](../../LEGAL.md).

## Cómo contribuir

- **Sin código:** ejecuta el [benchmark](../../docs/BENCHMARKING.md) en tu equipo y envía los resultados, o presenta un [informe de dispositivo](../../docs/DEVICES.md#send-a-device-report).
- **Con código:** elige un issue con la etiqueta `good first issue`, lee [CONTRIBUTING.md](../../CONTRIBUTING.md) y sigue la guía de [compilación](../../docs/Build.md).
- **Agentes de IA:** lee primero [AGENTS.md](../../AGENTS.md), la [arquitectura](../../docs/ARCHITECTURE.md) y la [política de IA](../../docs/AI_POLICY.md). Una persona es responsable de cada PR.

## Enlaces

- [README en inglés (versión de referencia)](../../README.md) · [Sitio web](https://karimalsalah.github.io/guarana/) · [Preguntas frecuentes](../../docs/FAQ.md)
- [Hoja de ruta](../../ROADMAP.md) · [Cambios](../../CHANGELOG.md) · [Licencia](../../LICENSE.txt)

## Licencia

Guarana es software libre distribuido bajo la **GNU General Public License v3.0 o posterior**.

Esta traducción puede no estar al día con la versión en inglés. Si hay diferencias, prevalece el README en inglés. La traducción aún no ha sido revisada por una persona hablante nativa.
