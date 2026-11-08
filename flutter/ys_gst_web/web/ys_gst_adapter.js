// Message adapter between the Flutter app and the gst.wasm Emscripten module
// (ges-timeline-example.js). The two runtimes share no memory: Flutter calls
// the commands below and receives plain JS event objects via onEvent.
// Mirrors gst.wasm/subprojects/samples/ges-timeline/ges-timeline-example.html.
(() => {
  const callbacks = [];
  const waiters = [];
  let started = false;

  window.ysEvents = [];

  // Resolves with the first event (already received or future) matching
  // predicate, after index `from` of ysEvents.
  window.ysWaitFor = (predicate, timeoutMs = 30000, from = 0) =>
    new Promise((resolve, reject) => {
      const found = ysEvents.slice(from).find(predicate);
      if (found) return resolve(found);
      const waiter = { predicate, resolve };
      waiters.push(waiter);
      setTimeout(() => {
        const i = waiters.indexOf(waiter);
        if (i >= 0) {
          waiters.splice(i, 1);
          reject(new Error("Timed out waiting for event"));
        }
      }, timeoutMs);
    });

  const dispatch = (event) => {
    ysEvents.push(event);
    for (const waiter of [...waiters]) {
      if (waiter.predicate(event)) {
        waiters.splice(waiters.indexOf(waiter), 1);
        waiter.resolve(event);
      }
    }
    for (const callback of callbacks) callback(event);
  };

  const ccall = (name, argTypes, args) => {
    if (!window.Module || !Module.ccall) throw new Error(`ysGst.${name}: module not started`);
    return Module.ccall(name, null, argTypes, args);
  };

  window.ysGst = {
    // canvas: the element object itself (it may live outside document lookup
    // reach); baseUrl: directory holding ges-timeline-example.{js,wasm},
    // two-clips-crossfade.xges and media/, ending with "/".
    start(canvas, baseUrl) {
      if (started) throw new Error("ysGst.start: already started");
      started = true;
      const files = {
        "/media/clip-a.mp4": baseUrl + "media/clip-a.mp4",
        "/media/clip-b.mp4": baseUrl + "media/clip-b.mp4",
        "/media/two-clips-crossfade.xges": baseUrl + "two-clips-crossfade.xges",
      };
      window.Module = {
        canvas,
        locateFile: (path) => baseUrl + path,
        print: (text) => console.log(text),
        printErr: (text) => console.warn(text),
        preRun: [
          () => {
            Module.addRunDependency("ys-media");
            Promise.all(
              Object.entries(files).map(async ([path, url]) => {
                const response = await fetch(url);
                if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
                return [path, new Uint8Array(await response.arrayBuffer())];
              }),
            )
              .then((entries) => {
                Module.FS_createPath("/", "media", true, true);
                for (const [path, data] of entries) {
                  const name = path.slice("/media/".length);
                  Module.FS_createDataFile("/media", name, data, true, false, false);
                }
                Module.removeRunDependency("ys-media");
              })
              .catch((error) => Module.ysOnEvent({ event: "error", where: "media", message: String(error) }));
          },
        ],
        ysOnEvent: dispatch,
      };
      const script = document.createElement("script");
      script.src = baseUrl + "ges-timeline-example.js";
      script.onerror = () =>
        dispatch({ event: "error", where: "adapter", message: `failed to load ${script.src}` });
      document.body.appendChild(script);
    },
    load(uri = "file:///media/two-clips-crossfade.xges") {
      ccall("ys_load", ["string"], [uri]);
    },
    play() {
      ccall("ys_play", [], []);
    },
    pause() {
      ccall("ys_pause", [], []);
    },
    seekFrame(frame) {
      ccall("ys_seek_frame", ["number"], [frame]);
    },
    onEvent(callback) {
      callbacks.push(callback);
    },
  };
})();
