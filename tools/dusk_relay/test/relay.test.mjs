/**
 * Local end-to-end test for the dusk_relay Durable Object, using Node's built-in
 * global WebSocket (Node >= 22). Run the relay first, then this:
 *
 *   npx wrangler dev --port 8787      # terminal 1
 *   node test/relay.test.mjs          # terminal 2
 *
 * Exits 0 if all checks pass, 1 otherwise. Override the target with RELAY_URL.
 */

const BASE = process.env.RELAY_URL || "ws://127.0.0.1:8787";
const ROOM = "test-" + Math.random().toString(36).slice(2, 8);

let failures = 0;
function check(cond, label) {
  console.log(`${cond ? "PASS" : "FAIL"}  ${label}`);
  if (!cond) failures++;
}

const allSockets = [];

// Wrap a WebSocket in a small async message queue.
function wrap(room) {
  const ws = new WebSocket(`${BASE}/room/${room}`);
  ws.binaryType = "arraybuffer";
  const q = [];
  const waiters = [];
  const push = (m) => (waiters.length ? waiters.shift()(m) : q.push(m));
  ws.addEventListener("message", (e) =>
    push(typeof e.data === "string" ? { text: e.data } : { binary: new Uint8Array(e.data) }));
  ws.addEventListener("close", (e) => push({ closed: e.code }));
  const handle = {
    ws,
    open: () =>
      new Promise((res, rej) => {
        if (ws.readyState === 1) return res();
        ws.addEventListener("open", () => res(), { once: true });
        ws.addEventListener("error", () => rej(new Error("ws error")), { once: true });
      }),
    next: (ms = 3000) =>
      new Promise((res, rej) => {
        if (q.length) return res(q.shift());
        const t = setTimeout(() => rej(new Error("timeout waiting for message")), ms);
        waiters.push((m) => { clearTimeout(t); res(m); });
      }),
    send: (d) => ws.send(d),
    close: () => ws.close(),
  };
  allSockets.push(handle);
  return handle;
}

const json = (o) => JSON.stringify(o);

async function main() {
  // --- Peer A: host opens the room with protocol 3, rules "seedABC" ---
  const a = wrap(ROOM);
  await a.open();
  a.send(json({ v: 1, protocol: 3, rules: "seedABC", role: "host", name: "Host" }));
  const aAck = await a.next();
  check(aAck.text && JSON.parse(aAck.text).ok === true, "host join accepted");
  check(aAck.text && JSON.parse(aAck.text).peerId === 0, "host gets peerId 0");

  // --- Peer B: client joins same room with matching protocol+rules ---
  const b = wrap(ROOM);
  await b.open();
  b.send(json({ v: 1, protocol: 3, rules: "seedABC", role: "client", name: "Client" }));
  const bAck = await b.next();
  check(bAck.text && JSON.parse(bAck.text).ok === true, "matching client join accepted");

  // Host should be told a peer joined.
  const aJoin = await a.next();
  check(aJoin.text && JSON.parse(aJoin.text).type === "peer_join", "host notified of peer_join");

  // --- Binary relay: A -> B ---
  const payload = new Uint8Array([1, 0, 0, 0, 0, 42, 7, 9]);
  a.send(payload);
  const got = await b.next();
  check(got.binary && got.binary.length === payload.length &&
        got.binary.every((v, i) => v === payload[i]), "binary frame relayed host -> client");

  // --- Binary relay back: B -> A ---
  const reply = new Uint8Array([2, 1, 1, 1]);
  b.send(reply);
  const gotA = await a.next();
  check(gotA.binary && gotA.binary.every((v, i) => v === reply[i]), "binary frame relayed client -> host");

  // --- Rejection: wrong rules hash ---
  const c = wrap(ROOM);
  await c.open();
  c.send(json({ v: 1, protocol: 3, rules: "seedXYZ", role: "client", name: "BadRules" }));
  const cAck = await c.next();
  check(cAck.text && JSON.parse(cAck.text).ok === false &&
        JSON.parse(cAck.text).error === "rules mismatch", "mismatched rules hash rejected");

  // --- Rejection: wrong protocol ---
  const d = wrap(ROOM);
  await d.open();
  d.send(json({ v: 1, protocol: 2, rules: "seedABC", role: "client", name: "OldBuild" }));
  const dAck = await d.next();
  check(dAck.text && JSON.parse(dAck.text).ok === false &&
        JSON.parse(dAck.text).error === "protocol mismatch", "mismatched protocol rejected");

  // --- Leave notice: close B, host should hear peer_leave ---
  b.close();
  const aLeave = await a.next();
  check(aLeave.text && JSON.parse(aLeave.text).type === "peer_leave", "host notified of peer_leave");

  a.close();
}

// Shut down cleanly. undici keeps connection-pool timers alive so the loop won't
// drain on its own; but calling process.exit() while a WebSocket handle is still
// CLOSING trips a libuv assertion on Windows. So: close every socket, wait until
// each reaches CLOSED, then a short grace before a hard exit.
async function finish(code) {
  await Promise.all(allSockets.map((s) => new Promise((res) => {
    if (s.ws.readyState === 3 /* CLOSED */) return res();
    s.ws.addEventListener("close", () => res(), { once: true });
    try { s.close(); } catch { res(); }
    setTimeout(res, 500); // safety net if no close event arrives
  })));
  setTimeout(() => process.exit(code), 150);
}

main()
  .then(() => {
    console.log(failures === 0 ? "\nALL PASS" : `\n${failures} FAILURE(S)`);
    return finish(failures === 0 ? 0 : 1);
  })
  .catch((e) => {
    console.error("test error:", e.message);
    return finish(1);
  });
