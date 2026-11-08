// Frame by frame check of a loaded GES timeline, run in the page:
//   await ysCheckTimeline()            // every timeline frame, then one past the end
//   await ysCheckTimeline({frames: [0, 44, 45, 59, 60, 104]})
//
// For each timeline frame t it seeks (flush + accurate), waits for the frame
// rendered for that seek and compares the frame-number strips burned in the
// test clips (tools/make_test_clips.sh) with what the GES project says:
//   - clip c is present at t  <=>  start <= T(t) < start + duration
//     (half-open [In, Out) on the timeline);
//   - a present clip shows source frame (T(t) - start + inpoint) * fps;
//   - an absent clip shows no strip; outside overlaps the present clip is
//     fully opaque; inside an overlap the two alphas add up to 1.
// Expected values come only from the "loaded" event (what GES loaded), the
// observed ones only from the pixels of the buffer sent to the canvas sink.

(() => {
  const FPS = 30;
  const STRIP = { "clip-a": "a", "clip-b": "b" };
  const ALPHA_EPSILON = 0.06;

  const frameTime = (t) => Math.ceil((t * 1e9) / FPS); // ges_timeline_get_frame_time
  const lastEvent = (name) => [...ysEvents].reverse().find((e) => e.event === name);

  function expectedAt(t, clips) {
    const time = frameTime(t);
    const result = {};
    for (const clip of clips) {
      const strip = STRIP[clip.name];
      if (!strip) continue;
      const present = clip.start <= time && time < clip.start + clip.duration;
      result[strip] = present
        ? { present, frame: Math.round(((time - clip.start + clip.inpoint) * FPS) / 1e9) }
        : { present };
    }
    return result;
  }

  async function seekAndGetFrame(t, timeoutMs) {
    const from = ysEvents.length;
    ys.seekFrame(t);
    const seek = await ysWaitFor((e) => e.event === "seek" && e.frame === t, timeoutMs, from);
    const done = await ysWaitFor(
      (e) => (e.event === "async-done" && e.seek === seek.seek) || e.event === "eos" || e.event === "error",
      timeoutMs,
      from,
    );
    if (done.event === "error") throw new Error(`seek ${t}: ${done.message}`);
    // The rendered buffer is reported from the streaming thread, async-done
    // from the bus: do not rely on their order
    const frame = await ysWaitFor((e) => e.event === "frame" && e.seek === seek.seek, 3000, from).catch(
      () => null,
    );
    return { seek, done, frame };
  }

  function check(t, expected, frame) {
    const problems = [];
    if (!frame) {
      const anyPresent = Object.values(expected).some((c) => c.present);
      if (anyPresent) problems.push("no frame rendered");
      return problems;
    }
    if (frame.timelineFrame !== t) problems.push(`rendered timeline frame ${frame.timelineFrame}`);
    const present = Object.entries(expected).filter(([, c]) => c.present).map(([s]) => s);
    for (const [strip, exp] of Object.entries(expected)) {
      const obs = frame[strip];
      if (!exp.present) {
        if (obs) problems.push(`${strip} visible (frame ${obs.frame}, alpha ${obs.alpha}) outside [In, Out)`);
        continue;
      }
      if (obs && obs.frame !== exp.frame) problems.push(`${strip} shows ${obs.frame}, expected ${exp.frame}`);
      if (present.length === 1 && (!obs || obs.alpha < 1 - ALPHA_EPSILON))
        problems.push(`${strip} alone but alpha ${obs ? obs.alpha : 0}`);
    }
    if (present.length === 2) {
      const sum = present.reduce((s, strip) => s + (frame[strip] ? frame[strip].alpha : 0), 0);
      if (Math.abs(sum - 1) > 2 * ALPHA_EPSILON) problems.push(`alphas add up to ${sum.toFixed(3)}`);
    }
    return problems;
  }

  window.ysCheckTimeline = async ({ frames, timeoutMs = 60000 } = {}) => {
    const loaded = lastEvent("loaded");
    if (!loaded) throw new Error("No timeline loaded");
    const clips = loaded.clips.filter((c) => c.type === "GESUriClip");
    const total = Math.round((loaded.duration * FPS) / 1e9);
    const list = frames || [...Array(total + 1).keys()]; // includes `total`: one past the end
    const rows = [];
    let previousAlphaB = -1;

    for (const t of list) {
      const expected = expectedAt(t, clips);
      const { frame } = await seekAndGetFrame(t, timeoutMs);
      const problems = check(t, expected, frame);
      if (frame && expected.a && expected.b && expected.a.present && expected.b.present) {
        const alphaB = frame.b ? frame.b.alpha : 0;
        if (alphaB + ALPHA_EPSILON < previousAlphaB) problems.push(`b alpha decreased to ${alphaB}`);
        previousAlphaB = alphaB;
      }
      rows.push({
        t,
        expected,
        observed: frame ? { timelineFrame: frame.timelineFrame, a: frame.a, b: frame.b } : null,
        ok: problems.length === 0,
        problems,
      });
    }

    const failures = rows.filter((r) => !r.ok);
    return {
      clips: clips.map(({ name, start, inpoint, duration }) => ({ name, start, inpoint, duration })),
      totalFrames: total,
      checked: rows.length,
      failures: failures.length,
      rows,
    };
  };
})();
