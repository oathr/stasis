# STASIS — contexte pour Claude Code

Ce fichier est lu automatiquement par Claude Code. Il passe le relais d'une session
menée sur Mac à une session sur le PC Windows de l'utilisateur.

## Le projet
STASIS est un rack d'effets ambient en VST3 : reverbs profondes, écho à bande, stretch
PaulStretch, grains, DeRez accordé, filtrage piloté par MIDI, saturation, couleur et
modulation (MOTION). Il est développé pour un usage personnel, avec une publication open
source envisagée plus tard (GPLv3). La vue d'ensemble est dans [README.md](README.md),
le plan, les choix et toutes les mesures dans [PLAN.md](PLAN.md).

**Cible prioritaire : Windows, VST3, testé dans Ableton Live.** AU, CLAP et la
signature viendront plus tard.

## L'utilisateur
- Il écrit en français : réponds en français, simplement, sans jargon inutile.
- Il juge à l'oreille et à l'œil. Il envoie souvent plusieurs demandes à la suite pendant
  le travail : traite-les toutes.
- Design : sobre, suisse, imprimé années 80, inspirations Gattaca, *Her* et Marathon.
  Le moodboard est sous licence : **s'en inspirer, ne jamais le copier**.
- Thème par défaut : SIGNAL (fond presque noir, accent bleu #3D6DFF, pêche #F2B686).
  Pas d'ombres portées. On reste dans les trois couleurs de la charte (papier, bleu, pêche).
- Il veut que tout soit optimisé : un module éteint ne calcule rien.

## Façon de travailler (à garder)
- **Mesurer, pas supposer.** Chaque algorithme a une mesure objective (justesse,
  repliement, niveau, CPU, test du clic de STRETCH…). Quand une mesure contredit ce qu'on
  a dit, on le dit franchement à l'utilisateur.
- Rendus et mesures DSP : `stasis_render` (`--analyse` pour les mesures).
- Test d'hôte : `stasis_hosttest <STASIS.vst3>` charge le VST3 comme un DAW (44,1 / 48 /
  96 kHz, buffers de 64 à 2048, automation aléatoire, MIDI, état, les 60 presets).
  `--no-timing` pour une machine chargée.
- Un commit par changement cohérent, message en anglais, terminé par la ligne
  `Co-Authored-By` habituelle. Dépôt : https://github.com/oathr/stasis (privé),
  branche `main`.

## Architecture
| Fichier | Rôle |
|---|---|
| `dsp/include/stasis/*.h` | Cœur DSP, C++20, header-only, sans dépendance |
| `plugin/Source/Params.h` | Table de tous les paramètres (hôte, interface, moteur) |
| `plugin/Source/Engine.h` | Chaîne audio sans JUCE : ordre libre, MOTION, limiteur de sécurité |
| `plugin/Source/Presets.h` | 60 presets ECHO / REVERB / AMBIENT (20 chacun), avec leurs modulations MOTION |
| `plugin/Source/Plugin*.cpp` | Processeur JUCE 8 et éditeur WebView |
| `design/mockup.html` | **L'interface** : page HTML embarquée dans le plugin (et maquette dans un navigateur) |
| `plugin/HostTest/` | Le testeur d'hôte |
| `.github/workflows/build.yml` | CI : VST3 Windows + macOS, test, artefacts téléchargeables |

Points à connaître :
- L'interface parle au plugin via les relais Web de JUCE 8 : chaque contrôle HTML porte
  l'`id` de son paramètre. Fonctions natives : ordre des modules, presets, cibles MOTION,
  PANIC. Sous Windows, c'est **WebView2** (SDK NuGet lié en statique, `NEEDS_WEBVIEW2`).
- Latence déclarée : 0. Le dry n'est jamais retardé ; STRETCH, TONAL et PITCH ne
  retardent que le wet.
- État sauvegardé : paramètres + ordre des modules + cibles MOTION + preset courant.
  Les anciennes sessions sont migrées (MOTION remis à zéro, GRAIN inséré après STRETCH).

## Compiler sous Windows
Prérequis : Visual Studio 2022 ou plus récent (charge de travail *Développement Desktop
en C++*), CMake ≥ 3.22, Git, et le SDK WebView2 :

```
nuget install Microsoft.Web.WebView2 -OutputDirectory C:\webview2
cmake -S . -B build -A x64 -DSTASIS_BUILD_RENDER=OFF -DJUCE_WEBVIEW2_PACKAGE_LOCATION=C:\webview2
cmake --build build --config Release --target Stasis_VST3 stasis_hosttest
build\plugin\stasis_hosttest_artefacts\Release\stasis_hosttest.exe build\plugin\Stasis_artefacts\Release\VST3\STASIS.vst3
```

Pour Ableton : copier `build\plugin\Stasis_artefacts\Release\VST3\STASIS.vst3` dans
`C:\Program Files\Common Files\VST3\`, activer *Use VST3 Plug-in System Folders* dans
les préférences Plug-ins de Live, puis *Rescan*. Pour le MIDI (FOCUS, DEREZ) : une piste
MIDI dont la sortie *MIDI To* vise la piste de STASIS, puis « STASIS ».

## Ce qui reste à vérifier sur Windows
- **L'interface WebView2 dans Ableton** : elle n'a été vue que dans le Standalone macOS
  et dans un navigateur. Vérifier qu'elle s'affiche, que les potards pilotent bien les
  paramètres (et l'automation de Live dans l'autre sens), le redimensionnement, la
  fermeture et la réouverture de la fenêtre, et plusieurs instances.
- Le MIDI vers un effet audio dans Live, le tempo et la position de lecture (ECHO SYNC,
  GRAIN SYNC en PULSE calé sur la grille).
- Le CPU dans Live avec des buffers courts.
- Le dossier de données WebView2 (`%TEMP%\STASIS`) avec plusieurs instances.

## Points ouverts connus
- Les polices (Archivo, IBM Plex Mono) viennent de Google Fonts : sans réseau,
  l'interface passe sur des polices de secours. À embarquer (licence OFL).
- SHAPE / SLEW : léger sous-son sur des aigus forts (à 3,1 kHz −3 dBFS, −12 dB).
- Un changement de preset coûte environ 2 ms dans le moteur (pic possible en buffer 64).
- Changer l'ordre des modules pendant que le son passe peut faire un petit clic.
