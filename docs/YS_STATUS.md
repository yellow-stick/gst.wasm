# Yellow Stick — gst.wasm : preuves d’acceptation

## Périmètre et état

Branche de travail : `remymenard/gst-wasm-full-port`. Remote de publication :
`origin` (`git@github.com:yellow-stick/gst.wasm.git`). Aucun changement à `main`
ni à `/home/admin/code/flipedit`.

Un critère n’est marqué validé qu’après réussite de son scénario réel dans
Chromium (headless, sans GPU, 8 cœurs) ; les preuves sont sous
[`evidence/`](evidence/).

| Critère | État | Preuve |
| --- | --- | --- |
| 1. Construction et exemples locaux | ✅ Validé | `cerbero build gst.wasm` → `All done!` (29 recettes) ; `videotestsrc-example` anime la bille dans Chromium |
| 2. GES inspectable et timeline chargée | ✅ Validé | `gst-inspect` navigateur liste `gessrc gesdemux nlesource nlecomposition nleoperation nleurisource` ; projet `.xges` chargé (clips, transition, durée) et prérollé |
| 3. Deux clips H.264, fondu, WebCodecs, canvas | ✅ Validé, ⚠️ pas temps réel aux transitions | Lecture jusqu’à EOS, décodage `webcodecsviddech264sw`, 135 tracés canvas dont le fondu (mode `renderAll`) ; blocages de 0,4 s et 1 s aux changements de pile NLE, qui font sauter le fondu en mode QoS par défaut |
| 4. Seeks exacts et [In, Out) | ✅ Validé | 136/136 images (fondu + trou + queue) et 121/121 (coupe franche), lues dans les pixels rendus et recoupées sur les captures du canvas (0 écart) |
| 5. Flutter Web et pont Emscripten | ✅ Validé | App Flutter (`flutter/ys_gst_web`) : Load, 7 seeks et Play pilotés par clics sur les contrôles Flutter ; canvas décodé conforme ; choix : pont JS/messages, pas FRB |

## 1 — Construction

Références : `README.md:104-130`, `.github/workflows/build.yaml:28-52`,
`pyproject.toml`, `build/base.inc`, `build/recipes/gst.wasm.recipe`.

Commandes exécutées :

```sh
uv tool install poetry
poetry install
```

Poetry 2.5.1 et les dépendances Python ont été installés ; un `poetry.lock`
a été généré. Le bootstrap initial a échoué sur la confirmation interactive
d’apt. `--non-interactive` ne transmet pas `-y` à ce bootstrap ; l’option
correcte, confirmée par `cerbero bootstrap --help`, est :

```sh
env GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=protocol.file.allow \
  GIT_CONFIG_VALUE_0=always poetry run cerbero -c build/gst.wasm.cbc bootstrap -y -j 8
```

Le bootstrap a terminé avec `All done!` après construction des 14 recettes
d’outils. L’autorisation des sous-modules locaux est limitée à la commande,
sans changer la configuration Git globale.

```sh
poetry run cerbero -c build/gst.wasm.cbc run emcc --clear-cache
poetry run cerbero -c build/gst.wasm.cbc build gst.wasm
```

Résultat : `Building the following recipes: libffi zlib pcre2 … gst-libav-1.0
gst.wasm`, 29 recettes, 8 jobs, fin `All done!` en 1458 s (≈ 24 min) sur
8 cœurs. Emscripten 5.0.7 signale `LLVM version … seeing "22.0", expected
"23"` (fork LLVM de Fluendo) : avertissement seulement. Les plugins installés
comprennent `libgstcompositor.a`, `libgstisomp4.a`, `libgstlibav.a`,
`libgstvideoconvertscale.a`, `libgstvideoparsersbad.a`. Aucun fichier GES
(`gst-editing-services-1.0.pc`, `libgstnle`, `libgstges`) n’est produit.

### Page d’exemples exécutée en local

```sh
python3 tools/serve_samples.py \
  build/gst.wasm_web_wasm32/dist/web_wasm32/share/gst.wasm/samples --port 8082
# Chromium : http://127.0.0.1:8082/videotestsrc-example/videotestsrc-example.html
```

`tools/serve_samples.py` envoie COOP `same-origin` et COEP `require-corp` ;
la page rapporte `crossOriginIsolated: true`, aucune erreur JS. Deux captures
du canvas 320×240 à 700 ms d’intervalle diffèrent et montrent la bille du
motif `ball` à deux positions :
[`videotestsrc-t0.png`](evidence/videotestsrc-t0.png),
[`videotestsrc-t1.png`](evidence/videotestsrc-t1.png).
Remarque : relire le canvas par `drawImage` donne du noir, car le contexte
WebGL de SDL2 ne conserve pas son tampon (`preserveDrawingBuffer` faux). Les
preuves visuelles passent donc par des captures du compositeur.

## 2 — GES dans le navigateur

### Ce qui compile

- Nouvelle recette `build/recipes/gst-editing-services-1.0.recipe`
  (surcharge de celle de Cerbero) : sans `gst-devtools-1.0` (GstValidate,
  qui tire json-glib et gst-rtsp-server), options meson `tools`,
  `validate`, `python`, `introspection`, `bash-completion`, `xptv`
  désactivées. `ges-launch-1.0` repose sur un exécutable en ligne de
  commande et le chargement dynamique de plugins : son équivalent web est le
  sample statique `ges-timeline`. GES et NLE sont des bibliothèques
  statiques : `libges-1.0.a`, `libgstges.a`, `libgstnle.a`, leurs `.pc`.
  `cerbero buildone gst-editing-services-1.0` → `All done!` en 47 s.
- `gst.wasm.recipe` dépend de `gst-editing-services-1.0`. `gstinspect`
  enregistre statiquement `nle` et `ges`.

### Bloquants de portage trouvés et corrigés

1. **GIO incomplet dans le fork GLib wasm.** GES utilise `GFile`, donc
   GIO. Le fork exclut les sources Unix de GIO pour Emscripten
   (`gio/meson.build:357`), mais `giomodule.c`, `glocalfile.c`,
   `gdbusprivate.c`… les référencent toujours sous `G_OS_UNIX`. Toute
   édition de liens d’un utilisateur de `GFile` échoue (aucun sample
   n’utilisait GIO jusqu’ici). Deux traitements :
   - symbole **de données** `g_io_watch_funcs` (défini par `giounix.c`,
     non compilé) référencé par `gobject/gsourceclosure.c` : vrai correctif,
     patch `build/recipes/glib/0013-gobject-gsourceclosure-no-g_io_watch_funcs-on-wasm.patch`
     (mêmes gardes `G_PLATFORM_WASM` que le fork applique déjà aux sources
     fd/signal Unix) ;
   - 30 **fonctions** Unix restantes (`g_unix_mount_*`,
     `_g_unix_volume_monitor_get_type`, backends de notification et
     portails, `g_content_type_*`, `g_unix_fd_message_*`, `res_query`) :
     les exécutables GES sont liés avec `-sERROR_ON_UNDEFINED_SYMBOLS=0`
     (`gio_wasm_link_args`, `samples/meson.build`). L’éditeur de liens les
     liste en avertissement ; un appel effectif avorte avec
     `missing function: <nom>`, sans échec silencieux.
   - Le premier appel réel est arrivé au chargement du projet :
     `Aborted(missing function: g_desktop_app_info_lookup_get_type)`.
     `giomodule.c` enregistre au premier `GFile` des types Unix non
     compilés pour Emscripten. Patch
     `build/recipes/glib/0014-gio-giomodule-do-not-register-Unix-only-types-on-wasm.patch`
     (gardes `G_PLATFORM_WASM` ; les types D-Bus compilés restent). Il
     reste 21 symboles non définis (montages Unix, content-type,
     fd-message, `res_query`). Aucun n’est atteint par les scénarios
     ci-dessous ; leur correction relève du fork GLib (à remonter vers
     fluendo/glib).
2. **Appels indirects de signature incompatible.** Premier lancement de
   `gstinspect` : `RuntimeError: function signature mismatch` dans
   `g_once_impl ← ges_track_type_get_type ← ges_clip_class_intern_init`.
   `ges-enums.c` passe à `g_once` six fonctions `void f (GType *)` castées
   en `GThreadFunc` (`gpointer f (gpointer)`) : comportement indéfini en C,
   piège fatal en wasm (`call_indirect` vérifie le type). C’est la même
   classe de bug que la branche amont `RDI-3208-thread-func-return`, qui ne
   couvrait pas GES. Patch
   `build/recipes/gst-editing-services-1.0/0001-ges-enums-give-g_once-callbacks-the-GThreadFunc-sign.patch`.
   Revue des autres casts de pointeurs de fonction de GES/NLE (`GFunc`,
   `GCompareFunc`, `GHFunc`, `GSourceFunc`, `GstIteratorFoldFunction`…) :
   aucun autre écart de **type de retour**.
3. **Gestionnaires de signaux d’arité incorrecte.** Au chargement du
   projet : `function signature mismatch` dans
   `g_cclosure_marshal_VOID__OBJECTv ← uridecodebin_source_changed_cb`
   (GstDiscoverer::source-setup). `ges-discoverer-manager.c` y connecte
   `source_setup_cb (self, source)` en mode *swapped*, alors que le
   marshaller passe 3 arguments.
   - Expérience sur la vraie toolchain (`/tmp/ys-bitcast/probe.c`,
     `EMCC_CFLAGS=-fwasm-fix-function-bitcasts`) : un cast **explicite**
     avec un argument en trop (`(GCopyFunc) gst_object_ref`) fonctionne ;
     un gestionnaire passé par `G_CALLBACK` puis appelé avec 3 arguments
     déclenche `null function or function signature mismatch`. Le fork LLVM
     de Fluendo ne peut pas corriger ce cas : la signature réelle n’est
     connue que dans le marshaller.
   - Voie écartée : `-sEMULATE_FUNCTION_POINTER_CASTS`. Elle impose `-O1`
     (en `-O3`, Binaryen échoue à la validation après `--fpcast-emu`) et
     fait passer le binaire de 15 à 37 Mo. Surtout, elle casse libffi
     (`TypeError: Cannot convert … to a BigInt` dans `ffi_call_js`, appelé
     par `g_cclosure_marshal_generic`) : incompatible avec GObject.
   - Audit statique de chaque `g_signal_connect*` : arité du gestionnaire
     comparée à `n_params + 2` du signal, extrait de chaque `g_signal_new`
     de GLib, du cœur, des libs, de plugins-base, de GES/NLE et de
     gst-plugins-web (252 signaux). GES/NLE : 68 connexions, 5 erreurs
     (`duration_changed_cb` ×2, `proxy_load_serialized_info_cb`,
     `source_setup_cb`, `commited_cb`). Cœur, libs, playback, pbutils,
     compositor, videorate, videoconvertscale, typefind, isomp4,
     videoparsers, gst-plugins-web : 0. Patch
     `build/recipes/gst-editing-services-1.0/0002-ges-give-signal-handlers-the-arity-their-marshaller-.patch`
     (ajout des paramètres manquants, `G_GNUC_UNUSED`).

### Preuve : `gst-inspect` dans Chromium liste GES et NLE

```sh
python3 tools/serve_samples.py \
  build/gst.wasm_web_wasm32/dist/web_wasm32/share/gst.wasm/samples --port 8082
# Chromium : http://127.0.0.1:8082/gstinspect-example/gstinspect-example.html
```

Sortie complète capturée depuis la console de la page :
[`evidence/gst-inspect-browser.txt`](evidence/gst-inspect-browser.txt)
(1839 lignes, `Total count: 5 plugins, 19 features`). Fabriques listées :
`gessrc gesdemux nlesource nlecomposition nleoperation nleurisource`
(plus `webcodecsviddech264sw`, `webcodecsauddecaacsw`, `webcanvassink`, …).
Seul le H.264 **logiciel** est enregistré ; le scan WebCodecs rapporte
`No H.264 decoder found for H264HW`, cohérent avec la sonde
`prefer-hardware: false`.

### Chaîne de décodage pour GES

`webcodecsviddec*` (`gst-plugins-web`) n’était pas utilisable dans une
timeline. Modifications de
`gst-plugins-web/gst/web/codecs/gstwebcodecsvideodecoder.cpp/.h` :

- **Appariement par timestamp** : `on_output` prenait
  `gst_video_decoder_get_oldest_frame`. WebCodecs sort les images dans
  l’ordre de présentation, la liste GStreamer est dans l’ordre de décodage :
  avec des B-frames, les PTS étaient permutés. Les chunks portent désormais
  le PTS en µs (au lieu de ms tronquées), et chaque VideoFrame retrouve sa
  `GstVideoCodecFrame` par ce timestamp.
- **Plus de push depuis le runner** : `finish_frame` était appelé sur le
  thread du `GstWebRunner`, partagé par tous les éléments web du pipeline.
  Pendant un fondu, le compositor bloque A en attendant B, qui a besoin du
  runner : interblocage. Le runner ne fait plus que mettre l’image en file
  (`output_queue`) ; elle est poussée depuis le thread de streaming.
- **`flush` (seek)** : `reset()` puis `configure()` ne produisait plus
  aucune sortie dans Chromium. Le VideoDecoder est donc **recréé**
  (constructeur + `configure`). S’y ajoutent un compteur d’époque
  (`flush_epoch`) pour rejeter les images soumises pendant le flush, et une
  garde « key frame requise » (`key_required`).
- **`drain`/`finish`** : `VideoDecoder.flush()` non attendu sur le runner
  (pas d’`await` : la réentrance Asyncify corrompait les piles
  suspendues) ; fin signalée par `on_flushed` et une condition. Sans ce
  flush, les dernières images retenues pour le réordonnancement des
  B-frames ne sortaient jamais.
- **Mémoire système RGBA** quand l’aval ne préfère pas
  `memory:WebVideoFrame` (`choose_memory`, convertisseurs, compositor de
  GES) : lecture synchrone dans `on_output`, via `drawImage` sur un
  OffscreenCanvas, `getImageData`, `HEAPU8.set`, puis `frame.close()`.
  `VideoFrame.copyTo()` est asynchrone et demandait un `await` sur le
  runner partagé. Le chemin rapide `webcodecsviddec ! webcanvassink` garde
  la VideoFrame.
- **Erreurs** : le callback `error` de WebCodecs remonte en
  `GST_ELEMENT_ERROR` au lieu d’être ignoré.

Autres correctifs du runtime web nécessaires au même scénario :

- `gstwebutils.cpp` : `handle_context_query` libérait le contexte reçu en
  *transfer none* (`gst_context_unref`), ce qui provoquait un double free
  au deuxième élément web. `set_context` libère désormais l’ancien canvas.
- `gstwebrunner.c` : sur wasm, `g_main_loop_run` du runner ne rend jamais
  la main. `finalize` attendait sa fin, d’où un interblocage à chaque
  destruction de décodeur (seek dans GES). Le runner reçoit maintenant un
  `_quit_and_notify`.
- `gstwebcanvassink.cpp` : `draw_raw` synchrone (`putImageData`, mise à
  l’échelle par un OffscreenCanvas intermédiaire), sans `await`.
- **Bus sans descripteur pollable** : sur wasm, le GSource du bus n’était
  jamais réveillé. Les messages (`async-done`, `eos`, erreurs) n’arrivaient
  pas à la boucle principale. Patch
  `build/recipes/gstreamer-1.0/0001-bus-make-bus-watches-work-without-a-pollable-fd.patch`
  (`prepare`/`check` via `gst_bus_have_pending`, réveil explicite).

### Sample `ges-timeline`

`gst.wasm/subprojects/samples/ges-timeline/` : `ges-timeline-example.c`,
page HTML autonome (COEP interdit les scripts CDN du template),
`two-clips-crossfade.xges`, `two-clips-cut.xges`, et un vérificateur
`ges-timeline-check.js`. La boucle GLib tourne sur le thread principal
proxifié, qui possède `#canvas`. Le contexte `GstWebCanvas` `#canvas` est
posé sur le `GESPipeline` avant la première transition d’état, afin que
décodeurs et puits partagent le runner qui possède le canvas. Puits vidéo :
`videoconvert ! video/x-raw,format=RGBA ! webcanvassink`. libav n’est pas
enregistré : `decodebin` ne peut choisir que les décodeurs WebCodecs. Les
plugins statiques requis par GES sont `coreelements playback
typefindfunctions isomp4 videoparsersbad web compositor videoconvertscale
videorate videotestsrc nle ges deinterlace imagefreeze videofilter smpte`.

Événements JSON vers `Module.ysOnEvent` : `ready`, `loaded` (clips de la
timeline), `decoder` (fabrique réellement instanciée), `state`, `seek`,
`async-done`, `frame` (code-barres lu dans le buffer reçu par le puits),
`eos`, `error`. Commandes : `ys_load`, `ys_play`, `ys_pause`,
`ys_seek_frame`, `ys_set_render_all`, `ys_set_debug`, `ys_fatal_criticals`.

### Preuve : timeline chargée

[`evidence/ges-timeline-loaded.json`](evidence/ges-timeline-loaded.json) :
clip-a, `transitionclip0` (GESTransitionClip crossfade, 1,5 s → 0,5 s),
clip-b, durée, GES 1.29.1. Le préroll rend l’image 0 de la timeline
(source A, image 10), décodée par `webcodecsviddech264sw`. Le chargement
est stable (5/5). Ce fichier date de la première version du projet
(inpoints `333333333`/`666666666`, sans trou ni queue). Le chargement du
projet actuel figure dans `evidence/flutter-ui-scenario.json` (`loaded`).

Deux autres patchs GES corrigent des pièges `function signature mismatch`
rencontrés à l’exécution :
`0003-ges-container-use-the-generic-marshaller-for-child-s.patch`. Les
gestionnaires de classe `child_added`/`child_removed` prennent
`(container, element)`, alors que le marshaller `VOID__OBJECT` passe un
`user_data` final ; le marshaller générique (libffi) les appelle avec les
arguments déclarés, sans casser l’ABI.
`0004-nle-composition-make-_initialize_stack_func-return-v.patch` : une
action de composition invoquée par `g_cclosure_marshal_VOID__VOID`
renvoyait un `gboolean`.

### Bugs de pointeurs de fonction restants dans GLib

Le fork LLVM de Fluendo (`-fwasm-fix-function-bitcasts`) remplace chaque
site d’appel casté par un *wrapper* d’exécution. Chaque site dispose d’un
pool de 64 emplacements dont la cible est stockée à une adresse fixe : deux
threads qui utilisent le même site se réaffectent mutuellement la cible.
GES appelle `g_list_sort`/`g_sequence` en parallèle (discoverer, NLE) :
appels vers la mauvaise fonction et corruption de la mémoire. Patchs
GLib :

- `build/recipes/glib/0015-glib-free_full-call-the-GDestroyNotify-without-a-GFu.patch` ;
- `build/recipes/glib/0016-glib-sort-without-GCompareFunc-to-GCompareDataFunc-c.patch`
  (trampoline `compare_func_trampoline` dans glist/gslist/garray/gtree).

Le même mécanisme reste présent dans le cœur GStreamer
(gstpad/gstelement/gstbin…). Aucun échec observé dans les scénarios
ci-dessous ; à corriger dans le fork LLVM ou site par site en amont.

## 3 — Deux clips H.264 avec fondu, WebCodecs, canvas

Clips de test (`tools/make_test_clips.sh build/ys-media`, libx264,
**non commités**) : 320×240, 30 i/s, 90 images, GOP 30 avec B-frames.
Chaque image porte son numéro en code-barres de 12 bits (bande haute pour
A, bande basse pour B), lu par `tools/framecode.py` (validé 90/90 par clip)
ou par la sonde du puits.

Projet `two-clips-crossfade.xges` (30 i/s) : A sur `[0, 60)`, B sur
`[45, 105)`, fondu automatique sur `[45, 60)`, trou `[105, 120)`, clip test
noir `[120, 135)`.

Lecture, mesurée par instrumentation de `putImageData` dans le worker qui
dessine le canvas. Les bandes sont décodées depuis l’`ImageData` réellement
tracée :

| Mode | Tracés canvas pendant la lecture | Tracés du fondu (A et B visibles) | Blocages > 100 ms | Durée de la lecture |
| --- | --- | --- | --- | --- |
| QoS par défaut | 74 | 0 (A 54 puis directement B 70) | 1697 ms à l’entrée du fondu | 4,4 s |
| `renderAll` (`qos=false`, `max-lateness=-1`) | 135 | 6 (A 58→63, alpha 0,635→0,216 ; B 23→28, alpha 0,196→0,525) | 420 ms (entrée), 990 ms (sortie), 126 ms, 192 ms | 5,3 s pour 4,5 s de timeline |

Détails : [`evidence/ges-playback-render-all-draws.json`](evidence/ges-playback-render-all-draws.json),
[`evidence/ges-playback-qos-draws.json`](evidence/ges-playback-qos-draws.json).
Le décodeur instancié est `webcodecsviddech264sw` (événements `decoder`).
La lecture se termine par `eos`, sans erreur.

Limite : le fondu n’est pas joué **en temps réel**. À chaque changement de
pile NLE, GES reconstruit les sources (`uridecodebin`, typefind, qtdemux)
et recrée les VideoDecoder, puis décode depuis la keyframe. Ce coût
(0,4 à 1 s ici) dépasse le budget d’une image. En mode QoS, les images en
retard sont jetées et le fondu disparaît. En dehors des transitions,
l’intervalle médian entre tracés est de 24 ms (< 33 ms). Pistes, non
mesurées : garder les décodeurs entre piles, précharger la pile suivante,
chemin `memory:WebVideoFrame` jusqu’au compositor (WebGL) au lieu de la
relecture RGBA.

Sans GPU, seul le décodeur H.264 **logiciel** existe
(`prefer-hardware: false`).

## 4 — Seeks exacts à l’image et intervalles [In, Out)

`ys_seek_frame(t)` lance un seek flush + accurate à
`ges_timeline_get_frame_time(t)` (arrondi supérieur). Le vérificateur
`ysCheckTimeline()` (`ges-timeline-check.js`) parcourt chaque image et
compare :

- les valeurs attendues, tirées **uniquement** de l’événement `loaded` :
  clip présent ⇔ `start ≤ T(t) < start + duration`, image source
  `floor((T(t) − start + inpoint) · 30)` ;
- les valeurs observées dans les pixels du buffer reçu par le puits :
  présence, numéro d’image source, alpha. Dans un chevauchement, les deux
  clips doivent être translucides, A décroissant et B croissant.

| Projet | Images vérifiées | Échecs | Rapport |
| --- | --- | --- | --- |
| Fondu + trou + queue (`two-clips-crossfade.xges`) | 136 (t = 0…135) | 0 | [`ges-seek-crossfade-report.json`](evidence/ges-seek-crossfade-report.json) |
| Coupe franche (`two-clips-cut.xges`) | 121 (t = 0…120) | 0 | [`ges-seek-cut-report.json`](evidence/ges-seek-cut-report.json) |

Contrôle indépendant de l’affichage : une capture du canvas par seek
(136), décodée par `tools/framecode.py`, est comparée à la sonde et aux
valeurs attendues. 0 écart :
[`ges-seek-crossfade-canvas.json`](evidence/ges-seek-crossfade-canvas.json),
planches [`ges-seek-crossfade-sheet.png`](evidence/ges-seek-crossfade-sheet.png)
et [`ges-seek-cut-sheet.png`](evidence/ges-seek-cut-sheet.png).

Bornes observées directement :

| t | Fondu | Coupe franche |
| --- | --- | --- |
| 44 | A 54 | A 54 (dernière image de A) |
| 45 | A 55, B à alpha 0 (début de rampe) | B 20 (In de B, A absent) |
| 59 / 89 | B 34 (A à alpha ≈ 0) | B 64 (dernière image de B) |
| 60 / 90 | B 35 seul (Out de A) | rien (Out de B) |
| 104 / 105 | B 79 / rien (Out de B) | — |

Le fondu masque l’Out de A et l’In de B (alpha ≈ 0 aux extrémités). Le
projet en coupe franche les rend visibles. Dans le fondu, l’alpha de A suit
(1 − x)² et celui de B suit x : GES applique l’alpha de la transition au
clip sortant, puis le compositor le multiplie à nouveau. Le contrôle porte
donc sur la monotonie des alphas, pas sur une somme égale à 1.

### Constat : bornes d’image ambiguës à 1 ns près

Premier passage avec des inpoints alignés sur le **début** d’image
(`floor(k·10⁹/30)`) : 63/106 images correctes, les autres décalées de −1.
Deux arrondis se cumulent (trace `videodecoder:5` d’un seek) :

1. Dans ces MP4 avec B-frames, l’image k est à `(k+2)/30` s (liste
   d’éditions), et GES ajoute ce décalage de flux à l’inpoint :
   `floor(20/30) + floor(2/30)` = `733333332`, soit 1 ns sous
   `floor(22/30)` = `733333333`.
2. qtdemux arrondit au-dessus la durée des images placées sur un multiple
   de 0,1 s (`33333334` ns) : leur fin déborde de 1 ns sur l’image
   suivante.

Dans les deux cas, le segment commence dans les 2 dernières ns de l’image
précédente. `GstVideoDecoder` la garde alors, coupée à 1–2 ns, et elle
remplit la première sortie. Règle retenue pour Yellow Stick : adresser
l’image source k **au milieu de son intervalle**, `(k + ½)/30` s (inpoints
`350000000` et `683333333`). Les positions de timeline restent celles de
`ges_timeline_get_frame_time`. Correctif amont possible : arrondis cohérents
dans qtdemux/GES.

Seek exactement à la fin de la timeline : NLE limite la position et
affiche la dernière image. Le trou et la queue noire du projet rendent l’Out
de B mesurable.

## 5 — Flutter Web pilote le pipeline

Application `flutter/ys_gst_web` (Flutter 3.47.7) : `lib/main.dart`
(canvas `#canvas` en `HtmlElementView`, boutons Load/Play/Pause, champ
Frame + Seek, affichage du dernier événement). Le pont
`web/ys_gst_adapter.js` expose `window.ysGst`
(`start/load/play/pause/seekFrame/renderAll/onEvent`), crée `Module` comme
la page du sample et renvoie les événements JSON. Construction et service :
voir `flutter/ys_gst_web/README.md`
(`flutter build web --release --no-web-resources-cdn`, module et médias
copiés sous `build/web/gst/`, `tools/serve_samples.py … --port 8090`).

Scénario exécuté par **clics souris et saisie clavier sur les contrôles
Flutter**, sans appel JS direct : Load, puis Seek 44, 45, 50, 59, 60, 104,
105, puis Play. Résultat
([`evidence/flutter-ui-scenario.json`](evidence/flutter-ui-scenario.json)) :

- `loaded` : 4 clips, durée 4,5 s ;
- les 7 seeks rendent exactement les images attendues (t=50 : A 60 @ 0,435
  + B 25 @ 0,329 ; t=105 : rien) ;
- les captures du canvas Flutter, décodées par `framecode.py`, sont
  identiques à la sonde
  ([`flutter-seeks-sheet.png`](evidence/flutter-seeks-sheet.png),
  page complète [`flutter-seek50.png`](evidence/flutter-seek50.png)) ;
- Play jusqu’à `eos` (54 images rendues en mode QoS, comme dans la page du
  sample) ; aucune exception.

Constat : sans GPU, Chromium rend CanvasKit en WebGL logiciel
(SwiftShader). Chaque `setState` déclenché par un événement `frame` coûte
donc du CPU au pipeline. Un éditeur devra limiter la fréquence des mises à
jour de l’interface.

## Contraintes et licences

- La recette amont `build/recipes/ffmpeg.recipe:21-26` active explicitement
  les décodeurs logiciels H.264 et AAC, et `gst.wasm` dépend de
  `gst-libav-1.0` : ces décodeurs sont compilés. Le sample `ges-timeline`
  n’enregistre pas libav ; le décodage prouvé passe par WebCodecs. Les
  binaires construits (wasm, avec ou sans libav) ne sont **pas publiés** :
  leur diffusion poserait la question des brevets H.264/AAC, à trancher
  avant toute mise en ligne.
- Les clips de test H.264 sont générés localement avec libx264
  (`tools/make_test_clips.sh`, sous `build/ys-media/`, ignoré par Git) et
  ne sont pas commités.
- GStreamer est LGPL ; les licences des dépendances et les droits éventuels
  sur les codecs restent distincts. Les sorties de compilation ne sont pas
  ajoutées au dépôt.
- Les pthreads Emscripten nécessitent SharedArrayBuffer et l’isolation
  cross-origin (COOP/COEP). Le serveur de preuve doit envoyer ces en-têtes.
- WebCodecs est l’interface de décodage navigateur visée ; sa présence ne
  prouve pas l’accès à un décodeur matériel sur cette machine sans GPU.
- wasm32 impose un espace d’adressage limité ; ni Vulkan ni VA-API ne sont
  des backends disponibles directement dans ce port navigateur.

## Sonde navigateur préalable

Commande et page réellement exercées :

```sh
python3 tools/serve_samples.py . --port 8080
# Ouvrir http://127.0.0.1:8080/tools/browser-capabilities.html
```

Résultat enregistré dans [`evidence/browser-capabilities.json`](evidence/browser-capabilities.json).
Le navigateur Chromium piloté rapporte l’isolation cross-origin active,
SharedArrayBuffer et VideoDecoder disponibles. Pour H.264 `avc1.42E01E`,
320×240, `isConfigSupported` répond vrai pour `no-preference` et
`prefer-software`, faux pour `prefer-hardware`. Ce n’est ni une preuve de
décodage effectif, ni une validation du critère 3. L’accélération matérielle
n’est pas démontrable avec cette configuration.

## Annexe — vérification précoce du pont (critère 5)

Outils exécutés : Flutter 3.47.7, Dart 3.13.5,
rustc 1.98.0-nightly (9e2abe0c6). La cible Rust
`wasm32-unknown-unknown` est installée.

La référence FlipEdit utilise FRB 2.13.0 et son chargeur web `wasm_bindgen`.
Le sample existant `gstlaunch` utilise au contraire `Module.cwrap` et le
runtime Emscripten. Ces lectures ne prouvent pas une incompatibilité de
coexistence : il ne faut pas confondre deux modules dans la même page avec
une ABI/mémoire partagée.

Compilation réellement exécutée dans `/tmp/ys-frb-web-probe` :

```toml
[package]
name = "ys-frb-web-probe"
version = "0.1.0"
edition = "2021"
[lib]
crate-type = ["cdylib"]
[dependencies]
flutter_rust_bridge = "=2.13.0"
wasm-bindgen = "0.2"
```

Source `src/lib.rs` :

```rust
use wasm_bindgen::prelude::*;
#[flutter_rust_bridge::frb(sync)]
#[wasm_bindgen]
pub fn frame_timestamp(frame: u32, fps: u32) -> u64 {
    u64::from(frame) * 1_000_000_000 / u64::from(fps)
}
```

`cargo build --target wasm32-unknown-unknown` termine avec
`Finished dev profile ... in 45.06s`. Un avertissement FRB
`unexpected cfg condition name: frb_expand` subsiste. Cela prouve la
compilation de cette dépendance et de cet export, pas celle de FlipEdit,
ni un échange effectif avec Emscripten.

### Coexistence réellement exécutée dans Chromium

`wasm-bindgen-cli 0.2.130` a produit le chargeur web de ce module Rust :

```sh
wasm-bindgen target/wasm32-unknown-unknown/debug/ys_frb_web_probe.wasm \
  --target web --out-dir pkg
```

Un second module, compilé avec le véritable Emscripten de Cerbero, exporte :

```c
#include <stdint.h>
#include <emscripten.h>
EMSCRIPTEN_KEEPALIVE
uint32_t frame_milliseconds(uint32_t frame, uint32_t fps) {
  return frame * 1000 / fps;
}
```

Compilation :

```sh
poetry run cerbero -c build/gst.wasm.cbc run emcc \
  /tmp/ys-frb-web-probe/emscripten.c \
  -o /tmp/ys-frb-web-probe/emscripten.mjs \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web \
  -sEXPORTED_FUNCTIONS=_frame_milliseconds --no-entry
```

Emscripten émet un avertissement de version LLVM (22.0 vu, 23 attendu),
mais cette compilation réussit. Une même page isolée charge les deux
modules avec leurs initialisateurs distincts, puis appelle
`frame_timestamp(45, 30)` et `emscripten._frame_milliseconds(45, 30)`.
Résultats réels : `1500000000n` et `1500`, sans erreur navigateur.
Voir [`evidence/wasm-coexistence.json`](evidence/wasm-coexistence.json).

Conclusion limitée : wasm-bindgen avec la dépendance FRB et Emscripten
peuvent cohabiter dans la même page. Aucune incompatibilité générale n’est
démontrée. Cette sonde n’utilise pas le code Dart généré par FRB et ne
prouve pas le pilotage GStreamer depuis Flutter.

Choix d’intégration : frontière JavaScript/messages, sans échange de
pointeurs Rust/GStreamer ni de vues de mémoire wasm entre les runtimes.
FRB n’apporte rien entre Dart et un module Emscripten : il génère des
liaisons vers du Rust compilé par wasm-bindgen, alors que GStreamer est en
C compilé par Emscripten, avec sa propre mémoire partagée et ses pthreads.
Un éditeur qui garde du Rust côté FRB peut coexister dans la page, mais il
parle au pipeline par le même adaptateur JS. Les commandes sont asynchrones
(invocation sur la boucle GLib) ; l’accusé de seek est l’événement `frame`
de ce seek, lu dans les pixels, pas le retour de `gst_element_seek`. Mise en
œuvre et preuve : section 5.
