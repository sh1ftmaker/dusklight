# dusk_relay — Cloudflare Worker + Durable Object room relay

The **relay / NAT-traversal layer** for Dusk Online (the analogue of Valve's Steam
Datagram Relay). Each room is one Durable Object that fans out WebSocket messages
between its peers. It does **not** run the game — Dusk Online stays
host-authoritative; one player is the sim authority and the rest mirror. The relay
just forwards bytes, which also gives NAT traversal (only outbound WebSockets from
every peer) and N>2 fan-out.

This is **discovery-independent**: it complements the `dusk_directory` phone book
(which lists rooms) and the direct-TCP transport (still intact). It's an *optional*
path the game reaches via a `wss://` switch.

## Layers (why this is separate)

| Layer | Component | Analogue in Garry's Mod |
|---|---|---|
| Authority (runs the sim) | the host player's game | listen server |
| Discovery (find rooms) | `tools/dusk_directory` | master server / server browser |
| Relay / NAT (move bytes) | **this** | Steam Datagram Relay |

## Protocol (WebSocket)

Connect to `wss://<host>/room/<id>` (id: 1–64 of `[A-Za-z0-9_.-]`).

1. **Join** — first message is TEXT JSON:
   ```json
   { "v": 1, "protocol": 3, "rules": "<hash>", "role": "host", "name": "Tom" }
   ```
   The first peer fixes the room's `(protocol, rules)`. Later peers must match or
   they're rejected — this is how incompatible builds (e.g. different randomizer
   seeds/rules) are kept apart. `rules` is an opaque hash; `""` = vanilla.
2. **Ack** — TEXT `{ "ok": true, "peerId": N, "peers": [...] }`, or
   `{ "ok": false, "error": "protocol mismatch" | "rules mismatch" | ... }` then close.
3. **Data** — after join, BINARY messages are game frames, relayed verbatim to every
   other peer. The DO also pushes TEXT `{ "type": "peer_join" | "peer_leave", ... }`.

## Develop & test locally (no Cloudflare account needed)

```sh
npm install                 # optional; npx fetches wrangler on demand
npx wrangler dev            # Durable Objects run locally on http://127.0.0.1:8787
node test/relay.test.mjs    # end-to-end checks (join, relay both ways, rejections)
```

`test/relay.test.mjs` uses Node's built-in global WebSocket (Node ≥ 22) and exits 0
on success. Override the target with `RELAY_URL=ws://host:port`.

## Deploy (one-time login)

```sh
npx wrangler login          # browser OAuth — uses your Cloudflare account
npx wrangler deploy         # publishes to <name>.<subdomain>.workers.dev
```

> Deploy auth is **wrangler** (Workers), which is a *different* credential store
> from `cloudflared` (Tunnel). Being logged into cloudflared does **not** authorize
> a Worker deploy.

## Status / TODO

- [x] Worker + `Room` Durable Object, join handshake with protocol + rules-hash
      gating, binary relay, peer join/leave notices. Verified locally.
- [ ] **Game-side WebSocket transport backend** (C++) behind `DUSK_ONLINE_RELAY=wss://…`
      — the substantive remaining half; keeps direct-TCP intact.
- [ ] Switch the DO to the **WebSocket Hibernation API** so idle rooms don't burn
      free-plan Durable Object duration.
- [ ] N>2: prefix relayed binary frames with a sender peer-id so the game can tell
      remotes apart (today frames are relayed un-tagged, which is fine for 2 peers).
- [ ] Optional `/rooms` lobby endpoint so the relay can also do discovery (a
      singleton lobby DO rooms register into), folding in the directory.
