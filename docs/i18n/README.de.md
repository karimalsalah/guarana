**Sprachen:** [English](../../README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português (Brasil)](README.pt-BR.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Übersicht](README.md)

**Discord:** [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM)

# Guarana

**Nintendo-Switch-2-Emulation, abgestimmt auf integrierte Grafikchips.**

Guarana ist ein Open-Source-Fork von [Lichii](https://github.com/zortos293/Lichii) (Branch `nx2`) für Windows x64. Das Projekt richtet sich an Notebooks und Handhelds mit integrierter Grafik: zuerst Intel Arc, danach AMD Radeon. Schwerpunkte sind gleichmäßigere Bildabstände, Bildgenerierung, schneller Start und eine effizientere Verteilung des Strombudgets.

> [!IMPORTANT]
> Guarana enthält **keine** Schlüssel, Firmware, Spiele oder Systemdateien. Sie brauchen Ihre eigenen, von einer Konsole und von Spielen, die Ihnen gehören. Guarana steht in keiner Verbindung zu Nintendo oder zum Lichii-Projekt. Die rechtlichen Hinweise finden Sie unten.

## Was ist das?

- Ein kostenloser Open-Source-Emulator unter der GPL-3.0-or-later.
- Abgestimmt auf integrierte GPUs mit gemeinsamem Speicher und begrenzter Leistung.
- Benötigt Windows x64, eine CPU mit AVX2 und einen aktuellen Grafiktreiber.
- Enthält keine Schlüssel, Firmware, Spiele oder Systemdateien.

## Gemessenes Ergebnis

Gemessen auf einem Notebook mit Core Ultra 9 285H, integrierter Arc 140T, Intel-Treiber 32.0.101.9175 und 32 GB RAM: Die Korrektur des globalen Speicher-Routings (`EDEN_NX2_GM=11`) hat in *Ocarina of Time* im Hyrule Field die Spielbildrate von **14,8 fps** auf **22,8 fps** erhöht. Verglichen wurde an derselben Stelle, mit der Option an und aus, bei deaktivierter Bildgenerierung.

Das ist ein einzelner Messwert von einem Gerät, einem Spiel und einer Szene. Andere Geräte, Treiber, Spiele und Szenen sind nicht gemessen. [Helfen Sie beim Messen](../../docs/BENCHMARKING.md).

## Unterstützung

Bisher getestet wurde nur die Arc 140T. Alle anderen GPUs (Arc 140V, Arc B390, Radeon 780M, 880M, 890M und weitere) sind noch ungetestet. Ein [Gerätebericht](../../docs/DEVICES.md#send-a-device-report) hilft. Geräte mit 16 GB gemeinsamem Speicher liegen unter der empfohlenen Mindestausstattung; siehe [DEVICES.md](../../docs/DEVICES.md).

## Schnellstart

1. Laden Sie das neueste zip von [Releases](https://github.com/karimalsalah/guarana/releases) herunter. Diese Seite ist der einzige offizielle Download.
2. Entpacken Sie das gesamte zip in einen Ordner, in den Sie schreiben dürfen, und starten Sie **`guarana.exe`**.
3. Installieren Sie Ihre Schlüssel und Firmware: **Tools → Install Decryption Keys** und danach **Tools → Install Firmware**.
4. Fügen Sie Ihren Spieleordner hinzu (Doppelklick auf die leere Spieleliste) und starten Sie ein Spiel. Beim ersten Betreten jedes Bereichs werden Shader kompiliert, was zu Rucklern führt; danach lädt das Spiel sie aus dem Cache.
5. Optional: Aktivieren Sie **Enable Frame Generation** unter **Emulation → Configure… → Graphics**.

Einstellungen, Speicherstände und Caches liegen in `%APPDATA%\guarana`. Beim ersten Start werden Ihre Einstellungen automatisch aus dem alten Ordner `%APPDATA%\lichii` übernommen (der alte Ordner bleibt bestehen). Alle Schalter stehen in [docs/switches.md](../../docs/switches.md), häufige Probleme in [TROUBLESHOOTING.md](../../docs/TROUBLESHOOTING.md).

## Eigene Schlüssel, Firmware und Spiele (rechtlicher Hinweis)

- Verwenden Sie Ihre eigenen Schlüssel, Firmware und Spiele, die Sie von Hardware extrahiert haben, die Ihnen gehört. Guarana stellt diese nicht bereit und akzeptiert keine Weitergabe in Issues, PRs oder Diskussionen.
- Anfragen oder Links zu urheberrechtlich geschützten Dateien sowie Fragen, wie man sie beschafft, werden nicht bearbeitet.
- Guarana ist ein unabhängiges Projekt. Es ist weder mit Nintendo noch mit dem Lichii-Projekt verbunden und wird von ihnen weder unterstützt noch gesponsert. „Nintendo“ und „Nintendo Switch“ sind Marken von Nintendo und werden nur benannt, um die Kompatibilität der Software zu beschreiben. Volltext: [LEGAL.md](../../LEGAL.md).

## Mitmachen

- **Ohne Code:** Führen Sie den [Benchmark](../../docs/BENCHMARKING.md) aus und senden Sie die Ergebnisse, oder reichen Sie einen [Gerätebericht](../../docs/DEVICES.md#send-a-device-report) ein.
- **Mit Code:** Beginnen Sie mit einem Issue mit dem Label `good first issue`, lesen Sie [CONTRIBUTING.md](../../CONTRIBUTING.md) und folgen Sie der [Build-Anleitung](../../docs/Build.md).
- **KI-Agenten:** Lesen Sie zuerst [AGENTS.md](../../AGENTS.md), die [Architektur](../../docs/ARCHITECTURE.md) und die [KI-Richtlinie](../../docs/AI_POLICY.md). Jeder PR braucht eine verantwortliche Person.

## Links

- [Englische README (Referenz)](../../README.md) · [Webseite](https://karimalsalah.github.io/guarana/) · [FAQ](../../docs/FAQ.md)
- [Roadmap](../../ROADMAP.md) · [Changelog](../../CHANGELOG.md) · [Lizenz](../../LICENSE.txt)

## Lizenz

Guarana ist freie Software unter der **GNU General Public License v3.0 oder neuer**.

Diese Übersetzung kann hinter der englischen Fassung zurückliegen. Bei Abweichungen gilt die englische README. Die Übersetzung wurde noch nicht von einer Muttersprachlerin oder einem Muttersprachler geprüft.
