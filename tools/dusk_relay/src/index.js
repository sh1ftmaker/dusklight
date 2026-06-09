/**
 * dusk_relay — Cloudflare Worker + Durable Object message relay for Dusk Online.
 *
 * Each "room" is one Durable Object that fans out messages between the peers
 * connected to it over WebSocket. This is the relay/NAT layer (the analogue of
 * Valve's Steam Datagram Relay): it does NOT run the game — Dusk Online stays
 * host-authoritative, with one player as the sim authority and the rest mirroring.
 * The DO just forwards bytes, so it also gives NAT traversal (only outbound WS from
 * every peer) and N>2 fan-out for free.
 *
 * Protocol over the WebSocket:
 *   - The FIRST message is a TEXT join handshake (JSON):
 *       { "v":1, "protocol":<int>, "rules":"<hash>", "role":"host"|"client", "name":"..." }
 *     The first peer to join fixes the room's (protocol, rules). Later peers must
 *     match or they are rejected — this is how incompatible builds (e.g. different
 *     randomizer seeds/rules) are kept from mixing. `rules` is an opaque hash;
 *     "" means vanilla.
 *   - The DO replies TEXT { "ok":true, "peerId":N, "peers":[...] } or
 *     { "ok":false, "error":"..." } then closes.
 *   - After joining, BINARY messages are game frames, relayed verbatim to every
 *     other peer. TEXT messages after join are relayed as control notices.
 *   - The DO also pushes TEXT { "type":"peer_join"|"peer_leave", ... } notices.
 *
 * NOTE (prototype): this uses the classic in-memory WebSocket API for clarity. For
 * production on the free plan, switch to the WebSocket Hibernation API
 * (ctx.acceptWebSocket) so idle rooms don't burn Durable Object duration. Binary
 * frames are relayed un-tagged today (fine for 2 peers); N>2 will want a sender-id
 * prefix so the game can tell peers apart.
 */

export class Room {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.peers = new Map(); // ws -> { peerId, name, role }
    this.nextId = 0;
    this.protocol = null;   // set by the first peer to join
    this.rules = null;
  }

  async fetch(request) {
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("expected a WebSocket upgrade", { status: 426 });
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.accept(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  accept(ws) {
    ws.accept();
    let me = null; // set once joined

    ws.addEventListener("message", (evt) => {
      const isText = typeof evt.data === "string";

      // --- join handshake (first message, must be text) ---
      if (!me) {
        if (!isText) {
          ws.close(4003, "binary before join");
          return;
        }
        let msg;
        try {
          msg = JSON.parse(evt.data);
        } catch {
          ws.send(JSON.stringify({ ok: false, error: "bad json" }));
          ws.close(4000, "bad json");
          return;
        }
        const protocol = msg.protocol | 0;
        const rules = String(msg.rules ?? "");
        const role = msg.role === "host" ? "host" : "client";
        const name = String(msg.name ?? "player").slice(0, 32);

        if (this.peers.size === 0) {
          this.protocol = protocol;
          this.rules = rules;
        } else if (protocol !== this.protocol) {
          ws.send(JSON.stringify({ ok: false, error: "protocol mismatch",
            roomProtocol: this.protocol, yours: protocol }));
          ws.close(4001, "protocol mismatch");
          return;
        } else if (rules !== this.rules) {
          ws.send(JSON.stringify({ ok: false, error: "rules mismatch" }));
          ws.close(4002, "rules mismatch");
          return;
        }

        const peerId = this.nextId++;
        me = { peerId, name, role };
        this.peers.set(ws, me);

        const others = [...this.peers.values()].filter((p) => p.peerId !== peerId);
        ws.send(JSON.stringify({
          ok: true, peerId, name, role,
          protocol: this.protocol, rules: this.rules, peers: others,
        }));
        this.broadcastText({ type: "peer_join", peerId, name, role }, ws);
        return;
      }

      // --- post-join traffic ---
      if (isText) {
        // Control passthrough (e.g. app-level signalling); relay to others tagged.
        this.broadcastText({ type: "text", from: me.peerId, data: evt.data }, ws);
      } else {
        // Game frame — relay verbatim to every other peer.
        this.relayBinary(evt.data, ws);
      }
    });

    const drop = () => {
      if (me && this.peers.delete(ws)) {
        this.broadcastText({ type: "peer_leave", peerId: me.peerId }, null);
        if (this.peers.size === 0) {
          // Room emptied — reset so the next occupant defines fresh rules.
          this.protocol = null;
          this.rules = null;
          this.nextId = 0;
        }
      }
    };
    ws.addEventListener("close", drop);
    ws.addEventListener("error", drop);
  }

  relayBinary(data, except) {
    for (const ws of this.peers.keys()) {
      if (ws === except) continue;
      try { ws.send(data); } catch { /* peer going away */ }
    }
  }

  broadcastText(obj, except) {
    const s = JSON.stringify(obj);
    for (const ws of this.peers.keys()) {
      if (ws === except) continue;
      try { ws.send(s); } catch { /* peer going away */ }
    }
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/" || url.pathname === "/health") {
      return new Response("dusk_relay ok — connect to /room/<id> via WebSocket\n");
    }
    const m = url.pathname.match(/^\/room\/([A-Za-z0-9_.\-]{1,64})$/);
    if (!m) {
      return new Response("usage: ws /room/<id>  (id: 1-64 chars [A-Za-z0-9_.-])", { status: 404 });
    }
    const stub = env.ROOM.get(env.ROOM.idFromName(m[1]));
    return stub.fetch(request);
  },
};
