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

La première commande a installé Poetry 2.5.1. La seconde est en cours.
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
