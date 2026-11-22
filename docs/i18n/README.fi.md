**Kielet:** [English](../../README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português (Brasil)](README.pt-BR.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Suomi](README.fi.md) · [Hakemisto](README.md)

**Discord:** [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM)

# Guarana

**Integroiduille näytönohjaimille optimoitu Nintendo Switch 2 -emulointi.**

Guarana on avoimen lähdekoodin haara [Lichii](https://github.com/zortos293/Lichii)-projektista (`nx2`-haara) Windows x64 -alustalle. Se on suunnattu kannettaville tietokoneille ja käsikonsolilaitteille, joissa on integroitu näytönohjain: ensin Intel Arc ja seuraavaksi AMD Radeon. Projekti keskittyy tasaisempaan ruudun ajoitukseen, ruutujen generointiin, nopeaan käynnistykseen ja tehobudjetin parempaan hallintaan.

> [!IMPORTANT]
> Guarana **ei sisällä** avaimia, laiteohjelmistoa, pelejä tai järjestelmätiedostoja. Sinun on käytettävä omia tiedostojasi, jotka on dumpattu itse omistamastasi konsolista ja peleistä. Projekti ei ole sidoksissa Nintendoon eikä Lichii-projektiin. Katso oikeudelliset tiedot alta.

## Esittely

- Ilmainen ja avoimen lähdekoodin emulaattori, lisensoitu GPL-3.0-or-later-lisenssillä.
- Optimoitu integroiduille näytönohjaimille, joissa on jaettu muisti ja rajallinen tehobudjetti.
- Vaatii Windows x64 -käyttöjärjestelmän, AVX2-tukea tukevan suorittimen ja ajantasaisen näytönohjainajurin.
- Ei sisällä avaimia, laiteohjelmistoa, pelejä eikä järjestelmätiedostoja.

## Mitattu tulos

Yhdellä kannettavalla tietokoneella (Core Ultra 9 285H, integroitu Arc 140T -näytönohjain, Intelin ajuri 32.0.101.9175 ja 32 GB RAM-muistia) globaalin muistin reitityskorjaus (`EDEN_NX2_GM=11`) nosti *Ocarina of Timen* Hyrule Fieldin suorituskyvyn **14,8:sta 22,8 pelin FPS:ään**. Mittaus tehtiin samassa paikassa asetus päällä ja pois päältä, ruutujen generointi pois käytöstä.

Tämä on yksittäinen mittaus yhdellä laitteella, yhdellä pelillä ja yhdessä pelialueessa. Muita laitteita, ajureita, pelejä tai pelialueita ei ole mitattu. Voit [auttaa mittaamisessa](../../docs/BENCHMARKING.md).

## Yhteensopivuuden tila

Vain Arc 140T on testattu. Muita näytönohjaimia (Arc 140V, Arc B390, Radeon 780M, 880M, 890M ja muut) ei ole vielä testattu. [Laiteraportti](../../docs/DEVICES.md#send-a-device-report) on arvokas apu. Laitteet, joissa on 16 GB jaettua muistia, jäävät suositellun vähimmäistason alapuolelle; katso [DEVICES.md](../../docs/DEVICES.md).

## Pika-aloitus

1. Lataa uusin zip-paketti [Julkaisut-sivulta](https://github.com/karimalsalah/guarana/releases). Kyseinen sivu on ainoa virallinen latauspaikka.
2. Pura koko zip-paketti kansioon, johon sinulla on kirjoitusoikeudet, ja käynnistä **`guarana.exe`**.
3. Asenna omat avaimesi ja laiteohjelmistosi: **Tools → Install Decryption Keys** ja sen jälkeen **Tools → Install Firmware**.
4. Lisää pelikansiosi (kaksoisnapsauta tyhjää peliluetteloa) ja käynnistä peli. Ensimmäinen käynti kullakin alueella kääntää shaderit ja aiheuttaa nykimistä; seuraavat käynnit käyttävät välimuistia.
5. Valinnainen: ota käyttöön **Enable Frame Generation** kohdasta **Emulation → Configure… → Graphics**.

Asetukset, tallennukset ja välimuistit sijaitsevat kansiossa `%APPDATA%\guarana`. Ensimmäisellä käynnistyskerralla asetuksesi kopioidaan automaattisesti vanhasta `%APPDATA%\lichii`-kansiosta (vanha kansio säilyy ennallaan). Kaikki kytkimet on kuvattu tiedostossa [docs/switches.md](../../docs/switches.md) ja yleiset ongelmat tiedostossa [TROUBLESHOOTING.md](../../docs/TROUBLESHOOTING.md).

## Avaimet, laiteohjelmisto ja pelit (oikeudelliset tiedot)

- Käytä omia avaimiasi, laiteohjelmistoasi ja pelejäsi, jotka on dumpattu omistamastasi laitteistosta. Guarana ei tarjoa niitä eikä hyväksy niitä issueissa, pull requesteissa tai keskusteluissa.
- Tekijänoikeudella suojattujen tiedostojen pyyntöjä, linkkejä niihin tai kysymyksiä niiden hankkimisesta ei käsitellä.
- Guarana on riippumaton projekti. Se ei ole sidoksissa Nintendoon eikä Lichii-projektiin, eivätkä ne ole hyväksyneet tai sponsoroineet sitä. "Nintendo" ja "Nintendo Switch" ovat Nintendon tavaramerkkejä, jotka mainitaan vain ohjelmiston yhteensopivuuden kuvaamiseksi. Koko teksti: [LEGAL.md](../../LEGAL.md).

## Osallistuminen

- **Ilman koodia:** aja [vertailutesti](../../docs/BENCHMARKING.md) ja lähetä tuloksesi tai tee [laiteraportti](../../docs/DEVICES.md#send-a-device-report).
- **Koodin kanssa:** valitse `good first issue` -merkinnällä varustettu tehtävä, lue [CONTRIBUTING.md](../../CONTRIBUTING.md) ja seuraa [kääntämisohjetta](../../docs/Build.md).
- **Tekoälyagentit:** lue ensin [AGENTS.md](../../AGENTS.md), [arkkitehtuuri](../../docs/ARCHITECTURE.md) ja [tekoälykäytäntö](../../docs/AI_POLICY.md). Jokaisella pull requestilla on oltava vastuuhenkilö.

## Linkit

- [Englanninkielinen README (referenssi)](../../README.md) · [Verkkosivusto](https://karimalsalah.github.io/guarana/) · [UKK](../../docs/FAQ.md)
- [Kehityssuunnitelma](../../ROADMAP.md) · [Muutosloki](../../CHANGELOG.md) · [Lisenssi](../../LICENSE.txt)

## Lisenssi

Guarana on vapaa ohjelmisto, joka on lisensoitu **GNU General Public License v3.0 -lisenssillä tai sitä uudemmalla versiolla**.

Tämä käännös voi olla jäljessä englanninkielisestä versiosta. Mahdollisissa ristiriitatilanteissa englanninkielinen README on ensisijainen. Käännös on tehty konekäännöksen avulla, mutta ihminen on tarkistanut sen. Teksti on vielä luonnos ja odottaa tarkempaa kielentarkistusta.
