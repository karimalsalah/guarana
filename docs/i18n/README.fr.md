**Langues :** [English](../../README.md) · [简体中文](README.zh-CN.md) · [Español](README.es.md) · [Português (Brasil)](README.pt-BR.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · [Русский](README.ru.md) · [العربية](README.ar.md) · [Index](README.md)

**Discord:** [discord.gg/urz7vMdRM](https://discord.gg/urz7vMdRM)

# Guarana

**Émulation de Nintendo Switch 2 optimisée pour les GPU intégrés.**

Guarana est une version dérivée open source de [Lichii](https://github.com/zortos293/Lichii) (branche `nx2`) pour Windows x64. Il vise les ordinateurs portables et les consoles portables à graphismes intégrés : d'abord Intel Arc, puis AMD Radeon. Le projet travaille sur une cadence d'images plus régulière, la génération de images, un démarrage rapide et une meilleure répartition du budget énergétique.

> [!IMPORTANT]
> Guarana **ne fournit pas** de clés, de micrologiciel, de jeux ni de fichiers système. Vous devez utiliser les vôtres, extraits d'une console et de jeux que vous possédez. Le projet n'est affilié ni à Nintendo ni au projet Lichii. Consultez les mentions légales ci-dessous.

## Présentation

- Un émulateur gratuit et open source, distribué sous licence GPL-3.0-or-later.
- Optimisé pour les GPU intégrés, avec mémoire partagée et puissance limitée.
- Nécessite Windows x64, un processeur compatible AVX2 et un pilote graphique à jour.
- Ne contient aucune clé, aucun micrologiciel, aucun jeu ni aucun fichier système.

## Résultat mesuré

Sur un ordinateur portable équipé d'un Core Ultra 9 285H, d'un GPU intégré Arc 140T, du pilote Intel 32.0.101.9175 et de 32 Go de RAM, le correctif de routage de la mémoire globale (`EDEN_NX2_GM=11`) a fait passer Hyrule Field, dans *Ocarina of Time*, de **14,8 fps** à **22,8 fps** de jeu. La mesure a été faite au même endroit, option activée puis désactivée, la génération de images étant désactivée.

Il s'agit d'une seule mesure, sur un appareil, un jeu et une scène. Les autres appareils, pilotes, jeux et scènes n'ont pas été mesurés. Vous pouvez [aider à mesurer](../../docs/BENCHMARKING.md).

## État de la compatibilité

Seule la Arc 140T a été testée. Les autres GPU (Arc 140V, Arc B390, Radeon 780M, 880M, 890M et autres) ne l'ont pas encore été. Un [rapport d'appareil](../../docs/DEVICES.md#send-a-device-report) est précieux. Les appareils à 16 Go de mémoire partagée sont en dessous du minimum confortable ; voir [DEVICES.md](../../docs/DEVICES.md).

## Démarrage rapide

1. Téléchargez le zip le plus récent depuis [Releases](https://github.com/karimalsalah/guarana/releases). Cette page est le seul téléchargement officiel.
2. Extrayez tout le zip dans un dossier où vous avez les droits d'écriture, puis lancez **`lichii.exe`**.
3. Installez vos clés et votre micrologiciel : **Tools → Install Decryption Keys**, puis **Tools → Install Firmware**.
4. Ajoutez votre dossier de jeux (double-clic sur la liste vide) et lancez un jeu. La première visite de chaque zone compile les shaders et provoque des saccades ; les visites suivantes utilisent le cache.
5. Facultatif : activez **Enable Frame Generation** dans **Emulation → Configure… → Graphics**.

Les paramètres, sauvegardes et caches se trouvent dans `%APPDATA%\lichii`, le même dossier que Lichii d'origine. Faites une sauvegarde avant de passer de l'un à l'autre. Tous les commutateurs sont décrits dans [docs/switches.md](../../docs/switches.md) et les problèmes courants dans [TROUBLESHOOTING.md](../../docs/TROUBLESHOOTING.md).

## Clés, micrologiciel et jeux (mentions légales)

- Utilisez vos propres clés, votre micrologiciel et vos jeux, extraits de matériel que vous possédez. Guarana ne les fournit pas et ne les accepte pas dans les tickets, les PR ou les discussions.
- Les demandes ou liens vers des fichiers protégés par le droit d'auteur, ainsi que les questions sur la façon de les obtenir, ne sont pas traités.
- Guarana est un projet indépendant. Il n'est ni affilié, ni approuvé, ni parrainé par Nintendo ni par le projet Lichii. « Nintendo » et « Nintendo Switch » sont des marques de Nintendo, citées uniquement pour indiquer la compatibilité du logiciel. Texte complet : [LEGAL.md](../../LEGAL.md).

## Contribuer

- **Sans code :** lancez le [benchmark](../../docs/BENCHMARKING.md) et envoyez vos résultats, ou soumettez un [rapport d'appareil](../../docs/DEVICES.md#send-a-device-report).
- **Avec du code :** choisissez un ticket étiqueté `good first issue`, lisez [CONTRIBUTING.md](../../CONTRIBUTING.md) et suivez le guide de [compilation](../../docs/Build.md).
- **Agents d'IA :** lisez d'abord [AGENTS.md](../../AGENTS.md), l'[architecture](../../docs/ARCHITECTURE.md) et la [politique IA](../../docs/AI_POLICY.md). Chaque PR doit avoir une personne responsable.

## Liens

- [README en anglais (référence)](../../README.md) · [Site web](https://karimalsalah.github.io/guarana/) · [FAQ](../../docs/FAQ.md)
- [Feuille de route](../../ROADMAP.md) · [Journal des modifications](../../CHANGELOG.md) · [Licence](../../LICENSE.txt)

## Licence

Guarana est un logiciel libre distribué sous la **GNU General Public License v3.0 ou ultérieure**.

Cette traduction peut être en retard sur la version anglaise. En cas de différence, le README anglais fait foi. Cette traduction n'a pas encore été relue par un locuteur natif.
