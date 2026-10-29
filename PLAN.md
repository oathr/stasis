# PLAN — VST d'ambient « ultime »

> Nom de travail : **STASIS** (alternatives : LONGFORM, SLOWLIGHT, HALO, DRIFT)
> Objectif : un rack d'effets pour transformer n'importe quel synthé / enregistrement en atmosphère longue, lente, profonde et *vivante*.
> Principes : **qualité DSP maximale · CPU minimal · interface ultra-simple · design sobre**.

---

## 0. Philosophie produit (ce qui nous évite de faire n'importe quoi)

1. **Peu de modules, mais chacun excellent.** 6 modules fixes + 1 rack de modulation. Pas de matrice à câbler, pas de menus profonds.
2. **Un module = 1 sélecteur d'algorithme + 3 à 4 paramètres + MIX.** La complexité est dans le DSP, pas dans l'UI.
3. **Le dry n'est jamais retardé.** Tout ce qui crée de la latence est sur le chemin wet (où un décalage est musicalement invisible).
4. **Chaque module éteint = 0 % CPU** (après extinction naturelle de la queue).
5. **On valide à l'oreille avant de coder l'UI.** Prototypes DSP d'abord, écoutes comparatives, ensuite seulement le design.

---

## 1. Architecture du signal

```
IN ─┬───────────────────────────────────────────────────────────────────────────────────┬─► MIX ─► OUT
    │                                                                                   │
    └► 01 SHAPE ► DEREZ ► 02 PITCH ► 03 STRETCH ► 04 ECHO ► 05 SPACE ► FOCUS ► 06 AIR ► COLOR + TONAL ──┘
          ▲            ▲           ▲          ▲         ▲         ▲        ▲
          └────────────┴──── 07 MOTION (rack de 12 LFO) ┴─────────┴────────┘
MIDI IN ─────────────────────────────────────────────────────────►┘
```
- **FOCUS** et **COLOR** agissent sur le wet uniquement : le dry reste intact.
- **DEREZ se place avant SPACE** : c'est la grosse reverb qui transforme le crunch accordé en texture.
- **TONAL ◂▸ NOISE a deux places au choix** (`PRE` / `POST`) : juste avant SPACE, ou en toute fin du wet (par défaut).
- **Chaîne capture → ping-pong → reverb** (rendus `17a` / `17b`) : STRETCH en SMEAR ou CAPTURE, puis ECHO en ping-pong SYNC, puis SPACE. C'est l'ordre fixe des modules.

- Ordre fixe (pensé pour l'ambient), avec 2 options de routage sur ECHO/SPACE : **Série** (echo → reverb) ou **Parallèle**.
- **Feedback global optionnel** (« LOOP ») : la sortie de SPACE réinjectée dans PITCH → shimmer/cascade infinie, avec limiteur doux dans la boucle.
- Boutons globaux : `MIX` · `OUTPUT` · `DUCK` (le wet s'efface quand on joue) · `FREEZE` (gèle tout) · `MOTION` (macro d'intensité de toutes les modulations) · `DRIFT` (multiplicateur de vitesse de tous les LFO).

---

## 2. Les modules et les meilleurs algorithmes

Légende : ★ = cœur de gamme (v1) · ◆ = expérimental / signature unique

### 01 · SHAPE — préparation du son (pré-traitement)
| Élément | Algorithme retenu | Pourquoi |
|---|---|---|
| ★ Filtres LP/HP/Tilt | **SVF TPT / ZDF** (Zavalishin, Cytomic/Simper) | Stable sous modulation rapide, pas de zipper, coût minime |
| ★ Saturation / Drive | Waveshaper **ADAA** (antiderivative antialiasing, Parker 2016) | Quasi pas d'aliasing **sans suréchantillonnage** → zéro latence, CPU bas |
| ★ Harmoniques | **Polynômes de Tchebychev** (contrôle de l'ordre harmonique : 2e, 3e, 5e…) + exciter | Ajoute des harmoniques précises, pas de « fuzz » |
| ◆ Sub / Octaver | Diviseur de fréquence + suivi d'enveloppe | Fond grave sous les pads |

### 02 · PITCH — hauteur, formants, harmonie
| Élément | Algorithme retenu | Pourquoi |
|---|---|---|
| ★ Pitch shift (qualité) | **Signalsmith Stretch** (licence MIT, qualité pro, pitch + formants) | Meilleur rapport qualité/licence aujourd'hui |
| ★ Pitch shift (zéro latence) | **Granulaire temporel** 2–4 grains fenêtrés Hann, crossfade | 0 latence, très peu de CPU, idéal dans les boucles de feedback |
| ★ Formant shift | Enveloppe **cepstrale** (ou LPC) découplée du pitch | Voix « plus grande/plus petite », texture sans changer la note |
| ◆ Frequency shifter | **Transformée de Hilbert** IIR (allpass polyphase) façon Bode | Mouvement « barber-pole » infini quand placé dans un feedback |
| ◆ Harmonizer d'intervalles | Quinte / octave / 9e mélangées, quantifiées à une gamme | Accords qui naissent de la queue |

### 03 · STRETCH — temps étiré, gel
> Point important : en temps réel on ne peut pas « ralentir le futur ». Le module enregistre en continu dans un **buffer circulaire (30 s → 2 min)** et relit le passé à vitesse réduite. Deux modes : **CAPTURE** (on fige un passage et on l'étire à l'infini) et **SMEAR** (lecture continue derrière le direct, recalage par crossfade quand le retard devient trop grand).

| Élément | Algorithme retenu | Pourquoi |
|---|---|---|
| ★ PaulStretch | **Algorithme de Paul Nasca** : très grandes fenêtres FFT (2¹⁴ → 2¹⁸), **randomisation des phases**, overlap-add Hann. Réimplémenté par nous (PaulXStretch est GPL : on ne copie pas son code) | Le son « cathédrale de glace » unique, étirement ×1 à ×10 000 |
| ★ Spectral Freeze | Capture d'un spectre, resynthèse avec phases aléatoires à chaque frame | Gel instantané, infini, sans boucle audible |
| ★ Extras PaulStretch | Lissage spectral, extraction d'harmoniques (garder uniquement les pics), séparation tonal/bruit, spread | Ce qui fait la richesse de PaulXStretch |
| ◆ Granular Cloud | Nuage de grains (position, taille, densité, spray, pitch aléatoire) inspiré de Mutable Clouds/Beads (code MIT) | Textures mouvantes, scintillantes |
| ◆ Phase vocoder « propre » | Phase locking (Laroche & Dolson) | Ralenti sans le côté « flou » du Paul |

**Implémenté en temps réel** (`capture_stretch.h`) :
- Une mémoire circulaire de 30 s et des frames PaulStretch en flux (FFT 32768, overlap-add avec fenêtre sinus).
- **SMEAR** : la tête de lecture suit le direct à 1/stretch de sa vitesse. Quand elle prend trop de retard, elle saute en avant, sans que ça s'entende puisque chaque frame est indépendante.
- **CAPTURE** : `capture()` fige les N dernières secondes et les étire en boucle à l'infini.
- **Latence** : une demi-fenêtre, sur le wet uniquement.
- **Temps réel** ✅ : chaque trame est calculée en 12 étapes réparties sur les blocs audio (FFT découpée par passes, gauche et droite dans une seule FFT complexe), et une capture est copiée par morceaux. Pire bloc mesuré : 0,37 ms, contre 2,9 ms avant. En échange, la sortie arrive un demi-buffer plus tard (~0,34 s à 48 kHz, sur le wet seulement).

### 04 · ECHO — délais
| Élément | Algorithme retenu | Pourquoi |
|---|---|---|
| ★ Tape | Ligne à retard fractionnaire **Hermite cubique**, wow & flutter (bruit filtré + sinus), saturation ADAA dans le feedback, head-bump EQ, souffle, multi-têtes | Le classique ambient, chaud, instable |
| ★ Digital / Ping-pong | Délai propre, sync tempo (noire, pointée, triolet), stéréo croisé | Clarté rythmique |
| ★ Diffuse | Chaîne d'**allpass dans le feedback** → l'écho se transforme progressivement en reverb | Le pont parfait echo → espace |
| ◆ BBD | Filtrage + companding + bruit d'horloge | Couleur analogique sombre |
| ◆ Reverse / Pitch echo | Lecture inversée par fenêtres / pitch shift granulaire dans la boucle | Echos qui montent ou s'inversent |
| Changement de temps | Mode **Tape** (glissement = pitch) ou **Jump** (crossfade sans artefact) | Modulation du temps sans clics |
| ★ **SYNC** (implémenté) | Temps calé sur le tempo de l'hôte : 1/1 à 1/32, en version normale, pointée (×1,5) ou triolet (×2/3). En SYNC, les deux canaux restent sur la grille (pas de décalage L/R). Les changements de tempo glissent comme une bande | Mesuré sur une impulsion : le premier écho tombe pile sur la division (1/8. à 96 BPM = 468,75 ms, 1/4T à 120 BPM = 333,33 ms) |

### 05 · SPACE — les reverbs (cœur du plugin)
Un seul module, un sélecteur `ALGO`, 4 paramètres communs : `SIZE` · `DECAY` (jusqu'à ∞) · `TONE` (amortissement) · `MOD`, + `PRE-DELAY` et `MIX`.

| Algo | Algorithme retenu | Caractère |
|---|---|---|
| ★ ROOM | Premières réflexions (taps / **velvet noise**, Välimäki) + petit FDN 8 lignes | Proche, intime |
| ★ PLATE | **Dattorro (1997)** « figure-8 tank » enrichi : allpass modulés, taps multiples stéréo | Brillant, dense, lisse |
| ★ HALL / CATHEDRAL | **FDN 16 lignes**, matrice **Hadamard** (rapide, O(N log N)), diffuseurs en amont (approche Signalsmith « Let's write a reverb »), **absorption dépendante de la fréquence** (Jot, filtres 3 bandes par ligne), lignes modulées | Immense, profond, decay de 0,5 s à ∞ |
| ★ SHIMMER | FDN + **phase vocoder dans la boucle de feedback**. `DEPTH` 0…1 (jusqu'à la moitié du feedback re-pitchée) et `PITCH` −24…+24 | Halo céleste qui monte, ou cascade sombre qui descend |
| ★ DIGITAL 80s | Allpass loops à modulation « random chorus » façon Lexicon 224/480 + option **fréquence d'échantillonnage interne réduite** (20–34 kHz) et bit-depth vintage | Le grain des reverbs numériques des années 80 (et ça économise du CPU) |
| ◆ SPECTRAL | Reverb dans le domaine STFT : **decay indépendant par bande de fréquence**, phases aléatoires | Les aigus peuvent tenir 60 s, les graves 3 s (ou l'inverse) — irréaliste et magnifique |
| ◆ MODAL / TUNED | Banc de **résonateurs modaux** (Abel et al., « Modal reverb ») accordés sur une gamme / une note | Reverb qui « chante » une tonalité |
| ◆ BLOOM | Cascade de diffuseurs à enveloppe lente : la reverb **gonfle** au lieu de décroître | Swells, effet « respiration » |
| ◆ BLACK / ∞ | FDN non physique : feedback > 1 contrôlé par saturation douce dans la boucle | Effondrement infini, masses sonores |
| (option v2) CONVOLUTION | Convolution partitionnée **non uniforme** (Gardner / Wefers) → zéro latence ; IR de grands espaces + IR générées synthétiquement | Réalisme absolu (lourd en CPU, donc optionnel) |

Commun à tous : **Freeze** (decay = ∞, entrée coupée), **Duck**, **Width**, filtres LP/HP en sortie.

**Moteur SPACE** (`dsp/include/stasis/space_reverb.h`), base des algos Room / Hall / Cathedral / Shimmer :
- **Entrée** : un diffuseur 8 canaux en 4 étages (délais aléatoires stratifiés, mélange, inversions de polarité, Hadamard).
- **Réseau** : un FDN de 16 lignes, chacune avec un **allpass modulé dans la boucle**, pour que la densité continue de croître dans la queue.
- **Matrice de feedback** : Householder, **suivie d'une rotation des lignes**. Chaque ligne se réinjecte dans une autre et pas dans elle-même. Avec une Householder seule, chaque ligne gardait 87 % de son propre signal et sonnait comme un filtre en peigne : c'était le côté métallique du hall.
- **Modulation aléatoire lisse**, sans chorus périodique.
- **Stéréo** : sorties L/R sur deux vecteurs orthogonaux, donc totalement décorrélées et compatibles mono.
- **Anti-boue à la source** : les graves décroissent plus vite par défaut, et l'entrée passe par un passe-haut.
- **Freeze parfaitement stable** (0,01 dB par minute). La modulation s'efface en douceur pendant le freeze : sinon les lectures interpolées rognent un peu d'aigus à chaque passage et vident la queue d'environ 2 dB par minute.
- **Justesse** :
  - La profondeur de modulation se calcule selon la longueur des lignes et le decay. Les petits désaccords de chaque passage s'additionnent au hasard sur une longue queue, donc on fixe l'écart total (quelques cents) au lieu de l'écart par passage.
  - Les premiers réglages s'appliquent sans glissement : c'était ce glissement qui faisait sonner la cathédrale presque un demi-ton trop bas au début.
- **Shimmer** : un phase vocoder (`spectral_pitch_shifter.h`) au lieu du granulaire. Sur une sinusoïde, le granulaire se trompait de +9 à −20 cents, le phase vocoder de ±2 cents.

Mesures (`stasis_render --analyse`, réponse impulsionnelle) :

| Reverb | Densité atteinte après | Résonance métallique | Corrélation L/R |
|---|---|---|---|
| Hall | 90 ms | **0,027** (0,084 avant la rotation) | 0,00 |
| Cathedral | 120 ms | **0,021** | 0,00 |
| Plate | 350 ms | 0,070 | 0,00 |

Justesse de la queue (`--analyse`, bursts de sinus sur 5 notes de C4 à C5) : écart moyen du pic et énergie à ±10 cents, dans trois fenêtres de temps.

| | Début | 1,2 s | 3 s |
|---|---|---|---|
| Hall | 2,7 ct / 85 % | 2,2 ct / 84 % | 5,6 ct / 72 % |
| Cathedral | 5,4 ct / 78 % | 5,9 ct / 60 % | 1,6 ct / 85 % |
| Shimmer +12 | 1,5 ct / 83 % | 3,1 ct / 83 % | 2,0 ct / 82 % |
| Shimmer +7 | 5,2 ct / 66 % | 3,9 ct / 78 % | 5,1 ct / 64 % |
| Shimmer −12 | 3,1 ct / 62 % | 3,1 ct / 77 % | 7,3 ct / 54 % |

Avant les corrections, la cathédrale et le shimmer +12 étaient décalés de −70 à −96 cents. La mesure est maintenant moyennée sur 5 notes, car une seule sinusoïde ne reflète que le mode de la pièce le plus proche. Le « ±1 cent » mesuré auparavant sur 440 Hz seul était un cas favorable.

Pistes suivantes vers le « top du top » : early reflections dédiées par taille de pièce, absorption à 4 bandes, densité adaptative selon la taille, calibration du niveau de sortie entre algos.

### FOCUS — le wet qui suit le MIDI (`harmonic_focus.h`)
Le plugin écoute le MIDI entrant et garde dans le wet uniquement les fréquences qui appartiennent à la musique jouée. Les notes hors accord sont atténuées, en particulier la queue de l'accord précédent quand on change d'accord.

| Bouton | Rôle |
|---|---|
| `MODE` | **KEY** : les notes jouées dans toutes les octaves (ou une gamme fixe si rien n'est joué) · **HARMONIC** : la série harmonique des notes jouées (garde le timbre naturel) |
| `FOCUS` | Précision : douce (±0,6 demi-ton) → laser (±0,1 demi-ton) |
| `MIX` | Dry/wet du filtre |

- **Deux moteurs** au choix :

| Moteur | Latence (wet) | Note hors accord atténuée* | CPU |
|---|---|---|---|
| PRECISE : masque FFT 8192 (`harmonic_focus.h`) | 170 ms | −19,9 dB | 2,3 % |
| PRECISE : masque FFT 4096 | 85 ms | −17,3 dB | 2,3 % |
| **ZERO** : banc de résonateurs SVF / peignes accordés (`resonant_focus.h`) | **0** | **−21,3 dB** | **1,8 %** |

\* La note B de l'accord précédent, dans la queue de la cathédrale. Sans FOCUS : −12,9 dB.

- **Choix par défaut : ZERO.** Il est à la fois plus précis, sans latence et moins coûteux.
  - En mode KEY, c'est un passe-bande à Q constant par demi-ton, de C1 à 5 kHz. Les notes autorisées sont additionnées.
  - En mode HARMONIC, c'est un filtre en peigne accordé par note tenue : un résonateur qui garde le timbre de la note.
  - Au-dessus de 5 kHz, l'air passe intact.
- **Limite physique** : séparer deux demi-tons dans le grave demande 100 à 200 ms d'observation. Le moteur ZERO ne retarde rien, mais ses résonateurs mettent un peu de temps à s'établir et à s'éteindre. Sur des queues de reverb, c'est inaudible.
- **Mode AUTO** (`key_detector.h`, sans MIDI) : un chroma calculé sur le dry, en parallèle du signal. Il ne retarde jamais l'audio et donne la priorité aux fondamentales. Deux mémoires :
  - CHORD (1,5 s) : suit l'harmonie.
  - SCALE (6 s) : accumule les notes d'une mélodie.
  
  Sur le pad, les deux accords sont détectés exactement. Sur une mélodie clairsemée, il peut manquer des notes, mais il n'en invente jamais. Avec le moteur ZERO : −20,9 dB, presque aussi bien qu'avec le MIDI.

### DEREZ accordé (`pitched_derez.h`)
Réduction de fréquence d'échantillonnage et de bits façon DeRez d'Airwindows, mais **juste**.
- **Pourquoi le DeRez classique sonne faux** : chaque fréquence f se replie en |k·R ± f|, sans rapport avec la note.
- **La solution** : verrouiller la fréquence de réduction sur la note, avec R = n × la fondamentale visée. Toutes les fréquences repliées tombent alors sur des harmoniques.

| Bouton | Rôle |
|---|---|
| `INTERVAL` | −24…+24 demi-tons (−24, −12, −1, +1, +2, +7, +12…), pitch shift avant la réduction |
| `CRUSH` | n de 32 (léger) à 2 (brutal) |
| `BITS` | 16 → 2, continu |
| `ROOT` | Note la plus grave du MIDI, sinon détection AUTO ou note fixe |
| `FREE` | Mode classique non verrouillé, pour garder le chaos si on le veut |

- **Pitch shift synchrone** : la fenêtre des grains vaut un nombre entier de périodes de la note, donc les deux têtes de lecture restent en phase.
- **Marches de réduction lissées (polyBLEP)** : le seul repliement est celui qu'on veut.
- **Coût** : latence d'un échantillon, CPU environ 0,1 %.
- **Mesure** sur une dent de scie à 110 Hz, à réduction égale (part de l'énergie sur les harmoniques de la note visée) :

| Intervalle | Classique | Accordé |
|---|---|---|
| 0 | 0,77 | **1,00** |
| +7 | 0,14 | **1,00** |
| −12 | 0,11 | **1,00** |
| +12 | 0,09 | **1,00** |

- **Limite** : sur un accord, la réduction se cale sur la note la plus grave. Les autres notes restent consonantes sans être parfaitement verrouillées.

### COLOR — le mix en trois mots (`color.h`)
| Bouton | Plage | Effet |
|---|---|---|
| `BASS` | −1…+1 | Low shelf ±9 dB à 120 Hz. En négatif, un passe-haut monte en plus (20 → 120 Hz) |
| `MUD` | 0…1 | Coupe 200–600 Hz. Une partie de la coupe est fixe, l'autre **dynamique** : elle se creuse seulement quand cette bande domine. Un son propre reste plein |
| `AIR` | −1…+1 | High shelf ±8 dB à 6 kHz |

Mesuré avec BASS −0,3, MUD 0,8, AIR +0,3 : le rapport boue / clarté s'améliore de 3,5 à 4 dB. L'intensité reste à régler à l'oreille.

### TONAL ◂▸ NOISE — avant ou après la reverb (`tonal_noise.h`)
Inspiré du réglage « tonal vs noise » de PaulXStretch. Un curseur bipolaire, placé dans la carte COLOR :
- **à droite (TONAL)** : il ne reste que les notes et leurs partiels, pour un ambient pur ;
- **à gauche (NOISE)** : il ne reste que l'air, le souffle et la texture diffuse ;
- **au centre** : le son passe sans changement.

**Moteur** : un masque STFT (FFT 4096, latence de 85 ms sur le wet uniquement, compensée pour le MIX).
- Chaque fréquence est comparée à la moyenne de ses voisines, sur ±0,5 à ±4 demi-tons.
- **Deux détections**, parce que les deux sens ont des besoins opposés :
  - stricte pour **garder** les notes, pour que peu de bruit passe avec elles ;
  - large pour les **effacer**, en excluant le pic de la moyenne pour que même une note grave ne se cache pas derrière elle-même.
- Le masque est lissé dans le temps pour éviter le scintillement (« musical noise »).
- Les phases sont conservées : c'est un filtrage, pas une resynthèse.

**Mesuré** sur un accord de sinusoïdes noyé dans du bruit (rapport notes / bruit de +7,6 dB en entrée) :

| Curseur | Rapport notes / bruit |
|---|---|
| TONAL +1 | **+17,1 dB** (+9,5 dB) |
| TONAL +0,5 | +11,3 dB |
| NOISE −0,5 | +4,2 dB |
| NOISE −1 | **−1,8 dB** (−9,4 dB) |

**Placement, au choix** (sélecteur `PRE` / `POST` à côté du curseur) :
- **POST** (par défaut) : en toute fin du wet. On filtre la queue elle-même : TONAL donne une queue faite uniquement de notes, NOISE une brume sans hauteur.
- **PRE** : juste avant SPACE. La reverb reconstruit une queue à partir de ce qui reste : TONAL envoie des notes propres dans la reverb, qui ajoute son propre halo diffus ; NOISE envoie uniquement le souffle, et la reverb en fait un nuage.

Mesuré avec une cathédrale de 6 s (wet seul, rapport notes / bruit de +9,1 dB sans le module) :

| Curseur | PRE | POST |
|---|---|---|
| TONAL +1 | +17,2 dB | +16,7 dB |
| NOISE −1 | **+3,0 dB** | +5,6 dB |

TONAL agit pareil aux deux places. NOISE efface mieux les notes en PRE : après la reverb, les partiels sont étalés et légèrement modulés, donc moins faciles à repérer.

**Coût** : environ 0,9 % d'un cœur. **À écouter** : `18a` / `18b` (chaîne → TONAL / NOISE en POST), `18d` / `18e` (les mêmes en PRE), `18c` (cathédrale → TONAL à fond).

### 06 · AIR — souffle, matière, stéréo, finition
| Élément | Algorithme retenu | Pourquoi |
|---|---|---|
| ★ Breath / souffle | Bruit coloré (rose/brun/filtré par bande) **piloté par l'enveloppe du signal** → le souffle « respire » avec le pad | Ajoute de l'air et de la vie organique |
| ★ Width / stéréo | M/S + **décorrélation par allpass** + rotation lente | Image large sans problème de mono |
| ★ Tape / Lo-fi | Hiss, saturation, sample-rate/bit reduction | Patine |
| ★ EQ final | Tilt + low/high shelf (SVF) | Ajustement final |
| ◆ Formant filter | Banc de passe-bandes SVF morphant entre voyelles (a-e-i-o-u) | Le pad « parle » lentement |
| ◆ Comb / résonateur | Filtres en peigne accordés | Métallique, harmonique |

---

## 3. 07 · MOTION — le rack de modulation (rendre les pads vivants)

### Principe ultra-simple
**12 lignes fixes**, chacune pré-assignée à une cible. Chaque ligne = **3 boutons** :

| Bouton | Rôle | Plage |
|---|---|---|
| `RATE` | Fréquence du LFO | **0,001 Hz** (un cycle ≈ 17 min) → 20 Hz, courbe exponentielle ; option sync tempo (de 64 mesures à la double-croche) |
| `SHAPE` | Type de LFO — **bouton continu qui morphe** entre les formes (pas un menu) | voir liste ci-dessous |
| `DEPTH` | Quantité de modulation (le « dry/wet » de la modulation) | 0 → 100 %, bipolaire en option |

> Remarque sur la demande « dry/wet » : on propose `DEPTH` pour la modulation (quantité d'effet du LFO sur la cible), et chaque module garde son propre `MIX`. Si tu préfères vraiment un dry/wet par ligne, c'est faisable : ce serait la même chose dans les faits pour la plupart des cibles.

Option cachée (clic droit / mode avancé) : changer la cible d'une ligne, inverser, décaler la phase L/R.

### Formes de LFO (ordre du bouton SHAPE)
1. **Sine**
2. **Triangle**
3. **Saw ↑** / **Saw ↓**
4. **Square** / **Pulse**
5. **Random Square** (Sample & Hold, marches)
6. **Random Slew** (S&H lissé, marches arrondies)
7. **Random Sine** (aléatoire interpolé cosinus/cubique — mouvement organique)
8. **Drunk** (marche aléatoire brownienne, ne revient jamais au même endroit)
9. **Perlin / Simplex noise** (dérive naturelle, très « vivant »)
10. ◆ **Chaos** (attracteur de Lorenz — quasi-périodique, jamais identique)
11. ◆ **Breath** (cycle asymétrique inspiration/expiration)
12. ◆ **Envelope follower** (suit le volume joué)

### Les 12 cibles proposées (ordre du rack)
| # | Cible | Ce que ça fait sur un pad |
|---|---|---|
| 01 | **PITCH** (detune fin, ±50 cents) | Dérive analogique, chorus lent |
| 02 | **FORMANT** | Le timbre « change de corps » |
| 03 | **HARMONICS** | Brillance qui va et vient |
| 04 | **FILTER / EQ** (cutoff + tilt) | Ouverture / fermeture lente |
| 05 | **STRETCH** (facteur / position de lecture) | Texture qui se déplace dans le temps |
| 06 | **ECHO** (temps + feedback) | Wow tape, echos qui s'étirent |
| 07 | **SPACE SIZE** | La pièce respire, grandit, rétrécit |
| 08 | **SPACE DEPTH** (decay + mix) | Profondeur qui avance / recule |
| 09 | **SHIMMER** (niveau des harmoniques hautes) | Halo qui apparaît par vagues |
| 10 | **BREATH** (niveau + couleur du souffle) | Air organique |
| 11 | **WIDTH / PAN** | Mouvement dans l'espace stéréo |
| 12 | **VOLUME** (swell / trémolo très lent) | Vagues, pulsation lente |

Autres cibles possibles (mode avancé) : pré-delay, diffusion, frequency shift, saturation, lo-fi, densité des grains, spray, résonance, intervalle de shimmer, voyelle du formant filter.

### Qualité de la modulation
- LFO calculés à **taux de contrôle** (tous les 16–32 échantillons) puis interpolés → précision + économie CPU.
- **Phase L/R décalable** : chaque LFO peut être légèrement différent à gauche et à droite → stéréo vivante.
- Bouton global `MOTION` (intensité de tout) et `DRIFT` (vitesse de tout) → contrôle d'un seul geste.
- Visualisation : chaque ligne affiche sa trace en temps réel (mini oscilloscope).

---

## 4. Stack technique

| Choix | Recommandation | Raison |
|---|---|---|
| Langage / framework | **C++20 + JUCE 8**, build **CMake** | Standard de l'industrie, VST3/AU/AAX, rendu GPU (Metal/Direct2D) |
| Interface | **WebView de JUCE 8** (`WebBrowserComponent`) qui charge l'interface HTML/CSS de la maquette, **visualiseur en WebGL** | Le design existe déjà en HTML : on le garde au pixel près. Les paramètres passent par les relais de JUCE 8 (automation et undo de l'hôte gérés). Le spectre est envoyé ~60 fois/s depuis un FIFO sans verrou, jamais depuis le thread audio. Plan B si la WebView pose problème (mémoire ~40 Mo par instance, WebView2 sur Windows) : réécrire en `juce::Graphics` natif + visualiseur Metal/OpenGL |
| Formats | **VST3 + AU** (macOS/Windows), **CLAP** via `clap-juce-extensions`, AAX plus tard | Couvre tous les DAW |
| FFT | **Apple vDSP/Accelerate** (mac) + **pffft** (Windows, BSD) | Les plus rapides, licences permissives |
| SIMD | `xsimd` ou intrinsics NEON/SSE/AVX | Le FDN 16 lignes se vectorise parfaitement (4 ou 8 lignes par instruction) |
| Pitch/stretch HQ | **Signalsmith Stretch** (MIT) | Qualité pro, licence libre |
| Tests | Catch2 (unit tests DSP) + **pluginval** + tests de null + analyse d'IR | Fiabilité |
| CI | GitHub Actions : build mac (universal arm64/x86) + Windows, signature + notarisation Apple | Livraison pro |

**Licences** : ✅ **usage perso, puis open source si le résultat est bon.**
- Licence cible pour la publication : **GPLv3** (ou AGPLv3), compatible avec la licence open source de JUCE (AGPLv3) et avec le SDK VST3.
- Le code GPL (Rubber Band, PaulXStretch) est donc **autorisé**. Seule condition : éviter le code « GPLv2 uniquement », incompatible avec la v3.
- Le code sous MIT ou BSD (Signalsmith, Mutable Instruments, pffft) reste compatible sans contrainte.
- Dès le début : un fichier `THIRD_PARTY.md` liste chaque dépendance et sa licence, pour que la publication se fasse sans surprise.
- Seul cas qui forcerait à remplacer le code GPL : une version **commerciale fermée**.

---

## 5. Optimisation CPU & latence

### Règles CPU
- **Zéro allocation, zéro lock** sur le thread audio ; tout préalloué pour la fréquence d'échantillonnage max (192 kHz).
- Traitement par **blocs**, paramètres lissés (pas de zipper).
- **FTZ/DAZ** (anti-denormals) systématique — critique pour les queues longues qui tendent vers 0.
- **Détection de silence** : un module dont la queue est éteinte s'endort.
- Matrices **Hadamard rapides** (butterflies) au lieu de multiplications N×N.
- LFO : accumulateur de phase + approximations polynomiales, random **xorshift** (pas de `sin()`/`rand()` par échantillon).
- **ADAA** au lieu de suréchantillonnage pour la saturation.
- Algo DIGITAL 80s à fréquence interne réduite → moins de calcul *et* plus de caractère.
- Modules spectraux : tailles FFT adaptatives, calcul réparti sur plusieurs blocs.
- Cible : **chaîne complète < 5 % d'un cœur** (M1, 48 kHz, buffer 256), reverb Hall seule < 1 %.

### Latence — 2 modes
| Mode | Comportement |
|---|---|
| **ZERO LATENCY** (défaut) | Latence déclarée = 0. Tous les traitements temporels (reverbs, échos, saturation, Hilbert, pitch granulaire) sont naturellement sans latence. Les modules spectraux (Paul, freeze, spectral, pitch HQ) ajoutent leur délai **uniquement sur le wet**, ce qui est inaudible dans un contexte ambient. Le dry reste parfaitement calé. |
| **ALIGNED** | Latence reportée au DAW (PDC) pour un alignement parfait du wet, utile en mix serré. |

---

## 6. Direction artistique

> **Maquette interactive** : [design/mockup.html](design/mockup.html). Elle s'ouvre dans un navigateur : potards déplaçables, 5 thèmes, FREEZE, LFO animés, visualisation animée.

### Règle d'or : s'inspirer, ne jamais copier
Le moodboard (affiches et visuels de Marathon, signalétique, cartographie) est sous licence. On en extrait des **principes**, jamais des motifs.

| On prend (principe) | On ne prend pas (motif) |
|---|---|
| Impression 2 couleurs + noir, aplats, **un seul accent acide** par écran | Les silhouettes de profil à œil blanc, les mains, les personnages |
| Grotesque **très grasse et condensée**, souvent **verticale** | La mise en page « SAFETY », la pastille ronde dans un carré noir, les codes de série existants |
| Micro-typographie technique (codes, révisions, unités) | Le soleil à chiffre romain, les symboles octogonaux en croix |
| Matière d'imprimé : grain, croix de repérage, légère défocalisation | Le logo, les formes bulbeuses vertes, toute typographie propriétaire |
| Pictogrammes : réduction géométrique, angles vifs, peu de formes | Toute reproduction ou décalque d'une image du moodboard |
| Glitch en balayage (slit-scan) utilisé comme un **geste** | Le glitch décoratif sans rapport avec le son |

### Système STASIS
- **Grille** : 12 colonnes. Chaque module est une carte avec une **tranche verticale** : numéro, nom en grotesque condensée tournée à 90°, interrupteur carré.
- **Typographie subtile, sans aucune ombre** (licences libres OFL) :
  - **Archivo** en graisse 500–600 et largeur normale, très espacée. Le logo STASIS est à +0,3 em, les noms de modules et de cibles en capitales espacées.
  - **IBM Plex Mono** 400/500, pour les valeurs et les codes.
- **Icônes originales en trait fin** : trait de 1,25 sur une grille de 24, dans un cadre fin. Elles sont dérivées du son :

| Module | Icône |
|---|---|
| SHAPE | Une courbe de filtre sur des axes |
| DEREZ | Un escalier sur une sinusoïde en pointillés |
| PITCH | Deux flèches |
| STRETCH | Un cercle qui devient une capsule |
| ECHO | Des cercles décroissants |
| SPACE | Des arcs concentriques |
| FOCUS | Un viseur |
| AIR | Des particules |
| COLOR | Trois curseurs |
| MOTION | Une onde |

- **Thème par défaut : SIGNAL bleu.** Fond noir chaud, accent bleu cobalt électrique, pêche en 3ᵉ couleur discrète.
- **En-tête** :
  - En SIGNAL, il est **sombre et intégré** au fond, séparé des modules par un filet fin. Le preset et FREEZE ont un fond translucide et un contour fin, et FREEZE passe en bleu quand il est actif.
  - Dans les autres thèmes, c'est une bande **blanche** (#FAFAF9) pleine largeur avec un filet marine, et le même grain que le reste de la page.
- **Haut clair, bas marine** (dans le thème SKY) :
  - La rangée du haut (SPACE, STRETCH, ECHO, FOCUS) est sur le papier bleu clair.
  - Les contrôles du bas (SHAPE, DEREZ, PITCH, AIR, COLOR et le rack MOTION) sont sur des panneaux **bleu marine**.
  - Les tranches des modules, la visualisation, le preset et FREEZE sont aussi en marine.
  - Le pêche est rare : valeur des potards, notes actives, intervalle choisi.
- **Lisibilité** : texte courant à 11 px, labels et codes à 8,5–9,5 px en graisse 500. Les codes sont plus contrastés sur le bleu.
- **Potards « imprimés »** : un disque d'encre plein, un secteur pêche qui se remplit avec la valeur, 11 graduations imprimées et une encoche d'index. Dans les lignes de MOTION, ce sont des mini-disques sans graduations.
- **Plus d'air** : 14 px entre les blocs et une seule ligne de code par module. Le choix d'algorithme de SPACE est une barre au-dessus de la visualisation.
- **Matière vintage** :
  - Un **dégradé très léger**, comme une feuille éclairée de biais depuis le haut à gauche, plus un vignettage doux.
  - Un **grain d'impression** marqué (bruit fractal à 3 octaves, fusion overlay).
  - Des croix de repérage sur une grille de 64 px.
- **Visualisation SPACE** : un **spectrogramme en trame demi-teinte de points ronds**, sur une grille décalée, qui défile **de droite à gauche**.
  - Chaque colonne est le spectre du moment, avec les fréquences en échelle log et les graves en bas.
  - Le passé s'éloigne vers la gauche et s'efface.
  - SIZE élargit les partiels, DECAY allonge la mémoire, SHIMMER fait monter des harmoniques (points lilas).
- **Glitch vivant**, lié aux gestes :
  - **FREEZE** fige le défilement et étire la trame en traînées horizontales façon slit-scan.
  - **Changer de preset ou de thème** déclenche un balayage rayé de droite à gauche, une secousse en escalier de la grille, et le nom du preset se brouille avant de s'afficher.
- **MOTION** : chaque ligne a sa trace en temps réel. Un clic sur la forme passe à la suivante.

### Thèmes (palettes originales)
| Thème | Fond | Encre | Accent | Esprit |
|---|---|---|---|---|
| **`SIGNAL` (défaut)** | noir chaud #121210 | blanc cassé #ECEADF, panneaux noirs | bleu cobalt électrique #3D6DFF, pêche #F2B686 en 3ᵉ couleur | technique, nuit, froid précis |
| `SKY` | bleu ciel pâle #B4C8E1 (dégradé #C6D7EC → #A1B7D3) | panneaux marine #13244B, encre #0F1B36 | pêche #F2B686 (rare), lilas #C9A6FF | affiche claire, contrôles sur marine |
| `PRINT` | bleu cobalt imprimé #2D5BCB (dégradé #3C6CDB → #2249AC) + zones noires #0A0E18 | noir | pêche #F2B686 (rare), lilas #C9A6FF en 3ᵉ couleur | affiche vintage, chaud sur froid |
| `OCHRE` | papier kaki #D7D6C9 | olive foncé | ocre #E1A92D | ésotérique, chaud |
| `PEACH` | crème #EEE2D2 | brun | violet #A56BF0 | tendre, glitch |

Les films de référence (Gattaca, Her) restent présents dans l'esprit : rigueur clinique pour SIGNAL et PRINT, chaleur pour OCHRE et PEACH.

### Mise en page (1280 × 840, redimensionnable)
```
┌ STASIS  codes ─────────── ‹ 014 SLOW CATHEDRAL › ───────── themes  MIX OUT  [■ FREEZE] ┐
│06 SPACE  [visualisation slit-scan]  [algos] SIZE DECAY TONE MOD PRE MIX │04 STRETCH│05 ECHO│07 FOCUS│
│          shimmer: DEPTH PITCH WIDTH                                    │          │       │ clavier│
│01 SHAPE │02 DEREZ [-24 … +24] CRUSH BITS MIX │03 PITCH │08 AIR │09 COLOR BASS MUD AIR   │
│10 MOTION  12 lignes : # CIBLE RATE SHAPE DEPTH ~trace~ (2 colonnes)  │ MOTION  DRIFT  CPU │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 7. Presets (livrés en v1)
Catégories : `PADS` · `DRONES` · `SHIMMER` · `FROZEN` · `TAPE` · `SPECTRAL` · `80s DIGITAL` · `EXPERIMENTAL`.
~60 presets de qualité, plus un bouton **RANDOMIZE intelligent** (aléatoire contraint à des plages musicales).

---

## 8. Feuille de route

| Phase | Contenu | Livrable | Critère de sortie |
|---|---|---|---|
| **0 · Recherche & prototypes** | Prototypes rapides des algos clés (FDN Hall, Dattorro, shimmer, PaulStretch, tape echo) dans un sandbox JUCE en ligne de commande (fichier WAV in → WAV out) | Banque de rendus audio | Écoutes comparatives validées par toi |
| **1 · Fondations** | Projet CMake/JUCE, système de paramètres, moteur de modulation, bypass/silence, tests, CI, pluginval | Plugin vide mais robuste dans le DAW | pluginval niveau 10 OK |
| **2 · MVP sonore** | SPACE (Room, Plate, Hall, Shimmer) + ECHO (Tape, Digital, Diffuse) + MOTION (12 lignes) + UI brute | **v0.1 jouable** | Utilisable en vraie session |
| **3 · Temps & matière** | STRETCH (Paul, Freeze, Granular), PITCH (pitch, formant, freq shift), SHAPE, AIR | v0.5 | Tous les modules ★ en place |
| **4 · Expérimental** | Spectral, Modal, Bloom, Black ∞, Digital 80s, BBD, Reverse | v0.8 | Sons uniques validés |
| **5 · Design final** | UI complète, 3 thèmes, visualisation, redimensionnement | v0.9 | Maquettes validées puis intégrées |
| **6 · Optimisation & release** | Profilage SIMD, presets, beta-test, installeurs, signature/notarisation | **v1.0** | Cibles CPU atteintes, 0 crash en beta |

Règle : **chaque phase se termine par des écoutes et des mesures** (CPU, IR, null tests) avant de passer à la suivante.

---

### Phase 1 — état (v0.1)
- **Cibles** : ✅ **Windows en priorité, VST3, testé dans Ableton**. AU, CLAP et le reste viendront plus tard.
- **Plugin JUCE 8** (`plugin/`) :
  - `Params.h` : la table des 44 paramètres, partagée par l'hôte, l'interface et le moteur ;
  - `Engine.h` : la chaîne audio, sans JUCE ;
  - processeur et éditeur WebView.
- **Chaîne branchée** : DEREZ ► STRETCH ► ECHO ► [TONAL pre] ► SPACE (Room / Plate / Hall / Cathedral, shimmer) ► FOCUS (zéro latence, MIDI ou AUTO) ► COLOR ► [TONAL post].
  - Chaque module a son interrupteur, avec un fondu de 30 ms à l'activation et à la coupure.
  - Le tempo de l'hôte pilote l'ECHO en SYNC, le MIDI entrant pilote FOCUS et DEREZ.
  - Latence déclarée : 0 (le dry n'est jamais retardé).
- **Interface** : `design/mockup.html` est l'interface du plugin, embarquée dans le binaire.
  - Chaque contrôle porte l'`id` de son paramètre ; dans un navigateur seul, la page fonctionne toujours.
  - Le visualiseur affiche le vrai spectre du wet ; le statut affiche le CPU et les notes suivies par FOCUS.
  - SHAPE, PITCH, AIR et MOTION sont grisés : pas encore dans le moteur.
- **Tests** : `stasis_hosttest` charge le VST3 comme un DAW, à 44,1 / 48 / 96 kHz et avec des buffers de 64 à 2048. Il lance de l'automation aléatoire, envoie du MIDI, puis sauve et recharge l'état.
  - CPU moyen : 1 à 3 % d'un cœur.
  - Pire bloc en automation : 41 % du temps réel à 64 échantillons / 48 kHz.
  - Un changement de tous les paramètres d'un coup (preset) coûte environ 2 ms dans le moteur.
- **Build Windows** : `.github/workflows/build.yml` (GitHub Actions) compile le VST3 Windows et macOS, lance le test et met les plugins en téléchargement.
- **v0.2** :
  - ordre des modules libre (glisser la tranche, ou ◂ ▸), fil de chaîne numéroté, visualiseur séparé ;
  - COLOR : BASS, BODY, MUD, HARSH, AIR + LOW CUT / HIGH CUT ;
  - MOTION dans le moteur : 12 LFO, cibles CRUSH, STRETCH, FEEDBACK, REPEATS, SIZE, DECAY, TONE, SHIMMER, WIDTH, FILTER, TONAL, VOLUME (±25 % de la course au maximum) ;
  - 30 presets (ECHO / REVERB / AMBIENT), aussi exposés comme programmes VST3 ;
  - STRETCH à 4 trames superposées, bouton ⟳ de recapture ;
  - ECHO sans wow ni flutter.
- **v0.3** :
  - SHAPE (tilt, drive ADAA calé à −12 dBFS, harmoniques paires), PITCH (phase vocoder ±24 st + décaleur de fréquence ±50 Hz, réjection 43–49 dB), AIR (souffle qui suit l'enveloppe, largeur M/S, lo-fi) : tous les modules sont actifs ;
  - DEREZ : curseur en demi-tons au lieu des boutons ;
  - DRY/WET global à 100 % par défaut, grand et étiqueté ;
  - limiteur de sécurité invisible (−0,3 dBFS, attaque instantanée) et garde anti-NaN qui coupe et réinitialise le moteur.
- **v0.3.1** :
  - **bug STRETCH corrigé** : depuis le découpage en étapes, la moitié basse du spectre (< 12 kHz) gardait ses phases d'origine, donc chaque trame rejouait l'entrée, ce qui donnait un écho. Test ajouté : un clic doit ressortir en souffle (facteur de crête 8, contre 121 avec le bug). La mesure « 0,45 dB de fluctuation » publiée avant était faussée par ce bug ; avec des phases vraiment aléatoires, le niveau de chaque note fluctue d'environ 4,5 dB, comme dans tout PaulStretch ;
  - modules éteints = zéro calcul (dont la détection de notes de FOCUS et les LFO de MOTION) ;
  - MOTION : amplitude ±50 %, toutes les lignes à DEPTH 0 % par défaut (une ligne à 0 ne calcule rien) ; arc et point orange sur chaque potard modulé, survol d'une ligne = potard cible entouré.
- **Reste à faire** :
  - embarquer les polices (aujourd'hui chargées depuis Google Fonts : sans réseau, l'interface passe sur des polices de secours).

## 9. Décisions à prendre avant de coder
1. ~~**Usage**~~ → ✅ **personnel, puis open source (GPLv3) si le résultat est bon**
2. ~~**Plateformes**~~ → ✅ **Windows en priorité**
3. ~~**Formats**~~ → ✅ **VST3 d'abord**, le reste plus tard
4. ~~**DAW**~~ → ✅ **Ableton**
5. **Nom** du plugin.
6. Validation du principe « 12 lignes de modulation fixes » vs cibles assignables.
