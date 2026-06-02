# Research isolation — the SEC-1 network barrier

**Status:** design — surfaced to auri/sulin for bless before any build.
**Owner:** bast. **Scope:** `bin/agent-launch` (`--profile` wiring), a new
`bus research` routing verb (broker topic), a `roles/scholar.md`, and a
**NixOS module** (nftables + a CONNECT proxy + the isolation primitive) that
**sulin applies** — agents cannot apply system config, which is itself part of
the barrier. **Touches the trust boundary** for every bypass peer, so this is
design-first: commit this doc, get the bless, then build.

## 1. The hard rule and the threat

sulin's rule, non-negotiable: **a bypass-permissions peer must never have
direct internet.** Today `bin/agent-launch:397` execs every agent with
`claude … --dangerously-skip-permissions`. That flag means the agent runs
arbitrary shell with **no per-action gate** — it can `curl`, open sockets, run
any binary, set or unset any env var. Two concrete harms follow if such a peer
can reach the open internet:

- **Exfiltration.** A compromised or misled bypass agent POSTs repo contents,
  credentials, or `~/.claude` tokens anywhere.
- **Web-prompt-injection into bypass actions.** A page the agent fetches
  carries "run this command" text; because the agent is bypass, the injected
  command executes with no human gate. Direct fetch is the delivery vector.

The fix is structural: **all online research routes through a separate, gated,
auto-mode agent (the *scholar*) that runs with *normal* permissions and a
read-only host allowlist.** Bypass *workers* get only what they need to be
Claude Code — `api.anthropic.com` — and nothing else. When a worker needs the
web, it asks the scholar over the bus (`bus research`), never the network.

## 2. Why an env-var proxy is theater

The obvious-but-wrong design: set `HTTPS_PROXY=allowlist-proxy:3128` in the
worker's env and call it contained. It is not. A bypass shell **owns its own
environment** — `unset HTTPS_PROXY; curl https://evil.example` defeats it in
one line. Any control the contained process can rewrite is not a control. The
barrier must live **below** the process, where the bypass shell has no
authority: in the kernel's packet path, keyed on something the process cannot
forge.

That single requirement — *unforgeable from inside a same-uid bypass shell* —
drives the entire mechanism choice in §4. It is worth stating as a law.

## 3. The privilege-boundary law

> An isolation barrier is only as strong as a privilege the contained process
> lacks. If the bypass shell can perform the same operation that *establishes*
> the barrier, it can perform the one that *removes* it.

A bypass agent runs as sulin's uid with no elevated caps but also no
restrictions a same-uid process can't touch. So the keying attribute must be
one of:

- a **different uid** — the shell cannot `setuid()` up or sideways without
  privilege; or
- a **network namespace** — the shell cannot `setns()`/leave its netns without
  `CAP_SYS_ADMIN`; or
- a **root-owned (non-delegated) cgroup** — the shell cannot move itself out
  because it lacks write permission on the sibling `cgroup.procs`.

Each pins enforcement to a boundary uid-1000 cannot cross. Setting any of them
up therefore **requires a privileged step** the agent itself cannot run — which
is exactly why the nftables/proxy/primitive ship as a **NixOS module sulin
applies**, not something an agent wires live.

## 4. Mechanism: which uid/cgroup — and a finding on the brief's "cgroup"

The brief specifies enforcement = **A2 + A3**: an nftables egress-`DROP` keyed
on the bypass peers, plus a hostname-allowlist CONNECT proxy as the only exit.
That shape is right. The open decision the brief delegates to me is *what the
nftables rule keys on*. Three candidates:

### 4a. uid — simplest, but breaks the filesystem

`nft … meta skuid $WORKER_UID drop` is the cleanest unforgeable match. But a
distinct worker uid **cannot read `~/.claude` credentials (mode 600, owned by
sulin) or write sulin's jj workspaces.** claude can't authenticate; jj can't
commit. We'd paper over it with ACLs on every path the fleet touches — leaky
and fragile. **Rejected** unless we accept a credential/ACL redesign.

### 4b. cgroup-by-slice — *forgeable under user delegation* (the finding)

The intuitive read of "cgroup" is: launch workers into a `claude-worker.slice`
via `systemd-run --user --slice=…`, then `nft … socket cgroupv2 … drop`. This
keeps sulin's uid (filesystem intact) — attractive. **But it is forgeable.**

systemd **delegates the user's cgroup subtree to the user**. A uid-1000 process
inside `claude-worker.slice` has write access to `cgroup.procs` across its own
subtree, so it can:

```
mkdir /sys/fs/cgroup/.../user@1000.service/escape.scope
echo $$ > /sys/fs/cgroup/.../user@1000.service/escape.scope/cgroup.procs
```

and **walk itself out of the matched cgroup** into an unmatched sibling. The
nftables rule keyed on `claude-worker.slice` no longer matches; egress flows.
A `--dangerously-skip-permissions` shell does this trivially. So **a
user-delegated cgroup has no privilege boundary against the very process it
contains** — it violates §3. I do **not** recommend it as written.

It *can* be made sound by hosting the worker cgroup in a **root-owned,
non-delegated** subtree (`systemd-run --system`, i.e. PID-1-managed), where
uid-1000 cannot write sibling `cgroup.procs`. That restores the boundary — at
the cost of a per-launch privileged `systemd-run --system` (polkit-gated) and
the same filesystem question as §4a if it also changes uid (it need not — a
`--system` scope can keep `User=sulin`). This is a viable variant; see §4d.

### 4c. netns cage — same uid, filesystem intact, unforgeable (recommended)

Run all bypass workers inside one shared **network namespace** ("the cage").
The cage holds a `veth` pair to the host; the host end carries the nftables
`DROP`-except-to-proxy rule. Properties:

- **Unforgeable:** leaving a netns needs `CAP_SYS_ADMIN` + a held namespace fd.
  A bypass shell has neither. §3 satisfied.
- **Filesystem intact:** netns namespaces *only* IP networking. The worker
  still runs as sulin — `~/.claude`, jj workspaces, the broker **UNIX** socket
  (`$STATE/broker.sock` is filesystem, not IP — crosses netns freely), zellij
  pane/TTY all keep working untouched.
- **A2 + A3 honored:** the nftables drop and the CONNECT proxy are exactly the
  brief's two enforcement points; the *keying* is the cage veth rather than a
  uid/cgroup. Same family, strictly stronger boundary.

Cost: the cage (netns + veth + its nftables) is created **declaratively at boot
by the NixOS module**; entering it per-launch needs one privileged hop
(`ip netns exec` / `nsenter`, polkit-allowed for sulin to this one namespace).
That privileged hop *is* the boundary — by construction the agent can't set it
up or tear it down.

### 4d. Recommendation and the seam to bless

I recommend **4c (netns cage)** as the soundest match to "same uid + full
filesystem + unforgeable," with **4b-hardened (`systemd-run --system` scope,
root-owned cgroup, `User=sulin`)** as the fallback if sulin prefers to stay
entirely within systemd's cgroup machinery and avoid netns plumbing.

**Surfacing the seam, not re-litigating:** the brief's A2+A3 architecture
stands — kernel-level egress DROP + CONNECT-proxy allowlist. The *only* thing I
am flagging for the bless is that the **plain user-cgroup keying is forgeable**
(§4b) and must be replaced by netns (4c) or a root-owned cgroup (4d-hardened).
This is a soundness correction to one knob, not a redesign. **Decision needed
from sulin/auri before build.**

## 5. The proxy: dual-tier CONNECT allowlist

The only exit from a contained cgroup/cage is a **CONNECT proxy** that
allowlists by **hostname**. CONNECT carries the target host in cleartext
(`CONNECT api.anthropic.com:443`) *before* TLS, so the proxy enforces the
allowlist **without** terminating TLS — no MITM, no cert games. The proxy opens
the upstream socket to the host it validated; the client cannot redirect it.

**Two tiers, keyed on listen port** (nftables pins each contained group to its
own proxy port, so a worker physically cannot reach the research port):

| Tier      | Listen | Allowlist                                  | Who           |
|-----------|--------|--------------------------------------------|---------------|
| worker    | :3128  | `api.anthropic.com` (+ statsig/sentry if claude needs them) | bypass workers |
| research  | :3129  | `api.anthropic.com` + curated research hosts | scholar only  |

**Proxy choice: squid**, via `services.squid` in the NixOS module. It is the
battle-tested CONNECT-allowlist proxy; `acl … dstdomain` + `http_access` +
`myportname` give exactly the per-port tiering above. Config shape:

```
acl tier_worker   myportname worker
acl tier_research myportname research
acl anthropic     dstdomain  .anthropic.com
acl research_dst  dstdomain  "/etc/squid/research-allowlist.txt"
http_port 127.0.0.1:3128 name=worker
http_port 127.0.0.1:3129 name=research
http_access allow tier_worker   anthropic CONNECT
http_access allow tier_research anthropic CONNECT
http_access allow tier_research research_dst CONNECT
http_access deny all
```

Alternatives noted, not chosen: `tinyproxy` (lighter, `Filter`/`ConnectPort`
but clumsier per-port tiers), or a ~120-line custom CONNECT proxy (zero deps,
but reinvents squid's hardened parser). squid wins on lowest-effort +
well-trodden for this exact job. The research allowlist starts **tight**
(e.g. `.anthropic.com`, `docs.rs`, `github.com`, `raw.githubusercontent.com`,
`arxiv.org`) and grows by sulin's review — never by an agent.

## 6. agent-launch gains `--profile worker|research`

Today the launcher ends at one unconditional line (`agent-launch:397`):

```
exec claude --name "$name" … --dangerously-skip-permissions
```

`--profile` forks that tail into two shapes (default `worker`, so the existing
fleet is unchanged):

- **`--profile worker`** (bypass + contained):
  - enter the cage / root-owned scope (the privileged hop, §4);
  - export `HTTPS_PROXY=http://127.0.0.1:3128` / `HTTP_PROXY` as the *cooperative*
    path (claude is honest; the nftables drop is the *enforcement* backstop if
    it isn't — see §7);
  - `exec claude … --dangerously-skip-permissions` (unchanged otherwise).
- **`--profile research`** (non-bypass + research allowlist):
  - enter the research-tier cage/scope (proxy :3129);
  - export `HTTPS_PROXY=http://127.0.0.1:3129`;
  - `exec claude … --permission-mode default` (**no** `--dangerously-skip-permissions`)
    in auto-mode so the scholar moves without check-ins, but every *action*
    still gates — it is not a bypass peer.

The flag selects the **profile object** (uid/cgroup/netns target + proxy port +
bypass-or-not). Wiring is mechanical once §4 is blessed; the launcher already
has the arg-parse loop (`agent-launch:50-82`) to extend.

## 7. The GATE — does claude honor HTTPS_PROXY? (step 3, pre-build)

The whole barrier's *delivery path* assumes claude reaches `api.anthropic.com`
**through the proxy**, because direct egress is dropped. If claude ignores
`HTTPS_PROXY`, a contained worker can't reach the API → **the barrier breaks the
fleet, not just the attacker.** This must be verified **before** building, and
if it fails we **stop and surface** (per the brief).

Probe (run a contained shell with only the proxy reachable, confirm claude
still talks to the API):

```
# with egress DROP active for the cage and squid on :3128:
HTTPS_PROXY=http://127.0.0.1:3128 HTTP_PROXY=http://127.0.0.1:3128 \
  claude -p 'say ok'      # succeeds ⇒ claude honors the proxy
# then prove the drop bites the bypass path:
curl -s https://example.com    # must hang/fail inside the cage
```

If claude does **not** honor `HTTPS_PROXY`, the fallback is a **transparent**
proxy: nftables `DNAT`/`tproxy` the cage's :443 into a **SNI-sniffing**
allowlist proxy, removing the dependency on the app reading an env var
altogether. More robust, more plumbing — only if the gate fails. **Either way,
the gate is a hard checkpoint: no build until it's answered.**

## 8. `bus research` — the worker→scholar routing verb

The worker's *only* sanctioned path to the web. It does not touch the network;
it asks a peer who may.

- `bus research "<query>"` (run by a worker) enqueues a request onto a broker
  topic (a `work-queue` kind, `topic=research`), tagged with the asking agent.
- The **scholar** (auto-mode, research-tier network) consumes it, does the
  actual web work (WebSearch/WebFetch land on its allowlisted, gated process),
  and returns a result to the asking agent's `agent-inbox` via the normal
  broker delivery path.
- The worker receives the answer as an ordinary bus message — same shape as any
  peer reply. No new transport: it reuses the broker's topic registry, cursors,
  and delivery loop wholesale.

This keeps the bus as the single mediation point (one human gate over the whole
thing) and means a worker that *tries* to fetch directly simply hits the
nftables wall — `bus research` is the only door, and it leads through a gated
peer.

## 9. The NixOS module (drafted for sulin to apply)

A single module wires the three system-level pieces the agents cannot
(deliberately — §3):

1. **The primitive** — the cage netns + veth (4c) *or* the root-owned worker
   slice (4d), created at boot.
2. **nftables** — drop egress from the contained group except to the proxy
   port(s); allow loopback + the broker socket path (filesystem, already fine).
3. **squid** — the dual-tier CONNECT allowlist of §5, plus
   `/etc/squid/research-allowlist.txt`.
4. **polkit** — let sulin (and thus agent-launch) enter the cage / start the
   `--system` scope for *this one* unit without a password, and nothing else.

bast **drafts** this module; **sulin applies** it (`nixos-rebuild`). The fact
that applying it needs root sulin-side, and that no agent can, is the barrier
working as designed.

## 10. Failure modes and rollout

- **Proxy down ⇒ workers can't reach the API.** The barrier is fail-closed by
  construction (drop is the default; proxy is the only hole). squid is a
  supervised systemd service; if it dies the fleet stalls on API calls rather
  than leaking — the safe direction. Monitor squid liveness alongside the
  broker.
- **Allowlist too tight ⇒ scholar can't fetch a needed host.** Expected and
  desired: the failure surfaces to sulin, who widens the list by review. Never
  auto-widen.
- **Rollout:** land this doc → bless §4 decision → draft the NixOS module +
  `--profile` + `roles/scholar.md` → run the §7 GATE → sulin applies the module
  → flip the fleet to `--profile worker`, spawn one `--profile research`
  scholar → verify a worker's `curl` to the open net fails while `bus research`
  round-trips. Default stays `worker`, so unflagged launches inherit the
  barrier the moment the module is live.

## 11. Open decisions for the bless

1. **§4 mechanism:** netns cage (4c, recommended) vs root-owned cgroup scope
   (4d-hardened). *Plain user-cgroup is off the table — forgeable (§4b).*
2. **§5 worker allowlist:** strictly `api.anthropic.com`, or does claude need
   statsig/sentry telemetry hosts to function? (Determined empirically during
   the §7 gate.)
3. **§7 gate outcome** gates everything after it — env-proxy path vs the
   transparent-SNI fallback.
