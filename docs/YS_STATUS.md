# Yellow Stick — gst.wasm : preuves d’acceptation

## Périmètre et état

Branche de travail : `remymenard/gst-wasm-full-port`. Remote de publication :
`origin` (`git@github.com:yellow-stick/gst.wasm.git`). Aucun changement à `main`
ni à `/home/admin/code/flipedit`.

Les critères restent non validés tant que leur scénario réel n’a pas réussi.

| Critère | État | Preuve actuelle |
| --- | --- | --- |
| 1. Construction et exemples locaux | ✅ Validé | `cerbero build gst.wasm` → `All done!` (29 recettes) ; `videotestsrc-example` anime la bille dans Chromium headless |
| 2. GES inspectable et timeline chargée | Non validé | Dépend de la toolchain |
| 3. Deux clips H.264, fondu, WebCodecs, canvas | Non validé | Aucun résultat d’exécution |
| 4. Seeks exacts et [In, Out) | Non validé | Aucun résultat image par image |
| 5. Flutter Web et pont Emscripten | Non validé | Coexistence wasm-bindgen + Emscripten prouvée dans une page ; pilotage Flutter à faire |

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

## 2 — GES dans le navigateur (en cours)

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

### Chaîne de décodage pour GES (préparée pour le critère 3)

`webcodecsviddec*` (`gst-plugins-web`) n’était pas utilisable dans une
timeline. Modifications de `gstwebcodecsvideodecoder.cpp/.h` :

- **Appariement par timestamp** : `on_output` prenait
  `gst_video_decoder_get_oldest_frame`. WebCodecs sort les images dans
  l’ordre de présentation, la liste GStreamer est dans l’ordre de décodage :
  avec des B-frames, les PTS étaient permutés. Les chunks portent désormais
  le PTS en µs (au lieu de ms tronquées), et chaque VideoFrame retrouve sa
  `GstVideoCodecFrame` par ce timestamp.
- **Plus de push depuis le runner** : `finish_frame` était appelé sur le
  thread du `GstWebRunner`, partagé par tous les éléments web du pipeline.
  Pendant un fondu, le compositor bloque A en attendant B, qui a besoin du
  runner : interblocage. Le runner ne fait plus que mettre la VideoFrame en
  file ; elle est poussée depuis le thread de streaming (`handle_frame`,
  `drain`).
- **`flush` (seek)** : `reset()` puis `configure()` du VideoDecoder,
  abandon des sorties de l’ancien segment, compteur d’époque pour les
  images soumises pendant un flush, garde « key frame requise ».
- **`drain`/`finish`** : `VideoDecoder.flush()` attendu, sinon les
  dernières images retenues pour le réordonnancement des B-frames ne
  sortaient jamais, ce qui faisait perdre la fin de chaque segment.
- **Mémoire système** quand l’aval ne préfère pas `memory:WebVideoFrame`
  (convertisseurs, compositor de GES) : `VideoFrame.copyTo()` selon le
  layout `GstVideoInfo` (offsets/strides, `gstwebvideoframe.cpp`). Le
  template de sortie accepte maintenant I420/NV12/… en mémoire système. Le
  chemin rapide `webcodecsviddec ! webcanvassink` garde la VideoFrame.
- **Erreurs** : le callback `error` de WebCodecs remonte en
  `GST_ELEMENT_ERROR` au lieu d’être ignoré.

### Sample `ges-timeline`

`gst.wasm/subprojects/samples/ges-timeline/` : `ges-timeline-example.c`,
page HTML autonome (COEP interdit les scripts CDN du template),
`two-clips-crossfade.xges`. La boucle GLib tourne sur le thread principal
proxifié, qui possède `#canvas`. Le contexte `GstWebCanvas` `#canvas` est
posé sur le `GESPipeline` avant la première transition d’état, afin que
décodeurs et puits partagent le runner qui possède le canvas. libav n’est
pas enregistré : `decodebin` ne peut choisir que les décodeurs WebCodecs.
Événements JSON vers `Module.ysOnEvent` : `ready`, `loaded` (clips de la
timeline), `decoder` (fabrique réellement instanciée), `state`, `seek`,
`async-done`, `frame` (code-barres lu dans le buffer rendu), `eos`, `error`.
Commandes : `ys_load`, `ys_play`, `ys_pause`, `ys_seek_frame`.

Preuve dans le navigateur : en cours (voir ci-dessous dès qu’exécutée).

## Contraintes et licences

- La recette amont `build/recipes/ffmpeg.recipe:21-26` active explicitement
  les décodeurs logiciels H.264 et AAC. La recette `gst.wasm` dépend de
  `gst-libav-1.0`. Une construction amont ne prouve donc pas un décodage
  WebCodecs. Aucun binaire de codec n’est publié par ce chantier à ce stade.
- GStreamer est LGPL ; les licences des dépendances et les droits éventuels
  sur les codecs restent distincts. Les sorties de compilation ne seront
  pas ajoutées au dépôt.
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

## 5 — Vérification précoce du pont

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

Choix d’intégration : frontière JavaScript/messages, sans échange de pointeurs
Rust/GStreamer ni de vues de mémoire wasm entre les runtimes. Les réponses
de seek devront attester la présentation de l’image, pas seulement le retour
de `gst_element_seek`. Aucun pilotage Flutter du pipeline n’est encore validé.
