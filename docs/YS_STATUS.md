# Yellow Stick — gst.wasm : preuves d’acceptation

## Périmètre et état

Branche de travail : `remymenard/gst-wasm-full-port`. Remote de publication :
`origin` (`git@github.com:yellow-stick/gst.wasm.git`). Aucun changement à `main`
ni à `/home/admin/code/flipedit`.

Les critères restent non validés tant que leur scénario réel n’a pas réussi.

| Critère | État | Preuve actuelle |
| --- | --- | --- |
| 1. Construction et exemples locaux | En cours | Installation de Poetry réussie ; installation des dépendances lancée |
| 2. GES inspectable et timeline chargée | Non validé | Dépend de la toolchain |
| 3. Deux clips H.264, fondu, WebCodecs, canvas | Non validé | Aucun résultat d’exécution |
| 4. Seeks exacts et [In, Out) | Non validé | Aucun résultat image par image |
| 5. Flutter Web et pont Emscripten | Non validé | Vérification de coexistence à effectuer |

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
sans changer la configuration Git globale. La compilation de `gst.wasm`
est lancée après `run emcc --clear-cache`.
Le disque indiquait 99 Go disponibles avant construction.

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
