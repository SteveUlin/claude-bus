# SEC-1 research-isolation barrier — DRAFT NixOS module for sulin to apply.
#
# Stands up the kernel-level network cage for claude-bus bypass peers. See
# docs/research-isolation.md for the full design + the GATE result (Claude Code
# HONORS HTTPS_PROXY via cleartext CONNECT, so squid dstdomain enforces the
# allowlist with NO TLS termination).
#
# WHAT THIS ENFORCES
#   - Two persistent network namespaces: `claude-worker`, `claude-research`.
#   - Each reaches the host ONLY through a veth whose sole route is to squid.
#     No default route, no NAT/masquerade => the only packets that leave a
#     cage are TCP to the squid veth IP. Everything else is unroutable, and
#     nftables drops it explicitly as defense-in-depth.
#   - squid, dual-tier by listen IP: worker tier => api.anthropic.com only;
#     research tier => anthropic + a curated read-only research allowlist.
#   - A polkit rule letting `sulin` start ONLY the namespaced launch scope,
#     so bin/agent-launch can enter the cage without a password and nothing
#     else gains privilege.
#
# WHY A WORKER CANNOT ESCAPE
#   Leaving a netns needs CAP_SYS_ADMIN; a uid-1000 --dangerously-skip-
#   permissions shell has zero kernel caps. The cage is created here by root,
#   declaratively — an agent can neither build it nor tear it down. The
#   launcher (§6 of the design) enters the netns as root THEN drops to uid 1000
#   before exec, so the worker holds CAP_SYS_ADMIN at no instant.
#
# DRAFT MARKERS — confirm against the live host before nixos-rebuild:
#   - sulin's uid is assumed 1000 (`id -u sulin`).
#   - The 10.200.0.0/30 + 10.200.0.4/30 link subnets must not collide with an
#     existing route (`ip route`). Pick free /30s if they do.
#   - claude's telemetry hosts: start anthropic-only; if claude visibly
#     degrades, widen `workerAllowed` after checking what it dials (the design
#     §5 open decision). statsig/sentry are the usual suspects.

{ config, lib, pkgs, ... }:

let
  # --- knobs ---------------------------------------------------------------
  workerNs   = "claude-worker";
  researchNs = "claude-research";

  # veth link subnets (host end .1, cage end .2). /30 = exactly the two ends.
  workerHostIp   = "10.200.0.1";
  workerCageIp   = "10.200.0.2";
  researchHostIp = "10.200.0.5";
  researchCageIp = "10.200.0.6";

  squidWorkerPort   = 3128;
  squidResearchPort = 3129;

  # Worker tier: ONLY the Anthropic API. Nothing else — workers do not browse;
  # they ask the scholar over the bus (`bus research`).
  workerAllowed = [ ".anthropic.com" ];

  # Research tier (scholar): anthropic + a TIGHT read-only allowlist. Grows
  # ONLY by sulin's review, never by an agent.
  researchAllowed = [
    ".anthropic.com"
    "github.com" "raw.githubusercontent.com" "objects.githubusercontent.com"
    "docs.rs" "crates.io" "static.crates.io"
    "ziglang.org" "arxiv.org"
    # add hosts here as the scholar's work demands, after review.
  ];

  workerAllowlistFile   = pkgs.writeText "squid-worker-allow.txt"
    (lib.concatStringsSep "\n" workerAllowed);
  researchAllowlistFile = pkgs.writeText "squid-research-allow.txt"
    (lib.concatStringsSep "\n" researchAllowed);

  # One-shot that builds a named netns + a veth to the host, gives the cage a
  # single route (to the squid host IP) and NO default route. Idempotent-ish:
  # tears down a stale netns of the same name first.
  mkCage = { ns, hostIp, cageIp, hostVeth, cageVeth }: ''
    set -eu
    ip netns del ${ns} 2>/dev/null || true
    ip netns add ${ns}
    ip link add ${hostVeth} type veth peer name ${cageVeth}
    ip link set ${cageVeth} netns ${ns}

    # host end
    ip addr add ${hostIp}/30 dev ${hostVeth}
    ip link set ${hostVeth} up

    # cage end: loopback + the veth, a route to the host IP, NOTHING else.
    ip netns exec ${ns} ip link set lo up
    ip netns exec ${ns} ip addr add ${cageIp}/30 dev ${cageVeth}
    ip netns exec ${ns} ip link set ${cageVeth} up
    # No `ip route add default` on purpose: the cage can reach ${hostIp} (the
    # squid host IP, directly connected) and nowhere else.
  '';

  tearCage = ns: "ip netns del ${ns} 2>/dev/null || true";
in
{
  ##########################################################################
  # 1. The cages (netns + veth, no default route)
  ##########################################################################
  systemd.services."claude-cage-worker" = {
    description = "claude-bus SEC-1 worker network cage";
    wantedBy = [ "multi-user.target" ];
    before = [ "squid.service" ];
    serviceConfig = {
      Type = "oneshot";
      RemainAfterExit = true;
      ExecStart = pkgs.writeShellScript "claude-cage-worker-up" (mkCage {
        ns = workerNs; hostIp = workerHostIp; cageIp = workerCageIp;
        hostVeth = "vbus-wkr-h"; cageVeth = "vbus-wkr-c";
      });
      ExecStop = pkgs.writeShellScript "claude-cage-worker-down" (tearCage workerNs);
      Path = [ pkgs.iproute2 ];
    };
  };

  systemd.services."claude-cage-research" = {
    description = "claude-bus SEC-1 research network cage";
    wantedBy = [ "multi-user.target" ];
    before = [ "squid.service" ];
    serviceConfig = {
      Type = "oneshot";
      RemainAfterExit = true;
      ExecStart = pkgs.writeShellScript "claude-cage-research-up" (mkCage {
        ns = researchNs; hostIp = researchHostIp; cageIp = researchCageIp;
        hostVeth = "vbus-rsh-h"; cageVeth = "vbus-rsh-c";
      });
      ExecStop = pkgs.writeShellScript "claude-cage-research-down" (tearCage researchNs);
      Path = [ pkgs.iproute2 ];
    };
  };

  ##########################################################################
  # 2. nftables — defense-in-depth egress drop on the host veths
  #
  # The missing default route already confines each cage to its squid IP. This
  # makes it explicit and survives any accidental host-side routing/NAT: from a
  # cage IP, allow ONLY tcp to its squid port; drop the rest. (Runs in the host
  # netns, where the host-side veths live.)
  ##########################################################################
  networking.nftables.enable = true;
  networking.nftables.tables."claude-cage" = {
    family = "inet";
    content = ''
      chain forward {
        type filter hook forward priority 0; policy accept;
        # cages have no default route, so they never hit forward for the
        # internet; this is belt-and-suspenders if a route ever appears.
        ip saddr ${workerCageIp}   ip daddr != ${workerHostIp}   drop
        ip saddr ${researchCageIp} ip daddr != ${researchHostIp} drop
      }
      chain input {
        type filter hook input priority 0; policy accept;
        # from the worker cage, ONLY squid worker port is reachable on the host
        ip saddr ${workerCageIp}   tcp dport ${toString squidWorkerPort}   accept
        ip saddr ${workerCageIp}   ip daddr ${workerHostIp}   drop
        ip saddr ${researchCageIp} tcp dport ${toString squidResearchPort} accept
        ip saddr ${researchCageIp} ip daddr ${researchHostIp} drop
      }
    '';
  };

  ##########################################################################
  # 3. squid — dual-tier CONNECT allowlist, bound per-cage host IP
  #
  # No TLS termination: the GATE proved claude sends `CONNECT <host>:443` in
  # cleartext, so dstdomain matches the real host before TLS. squid binds each
  # tier to the host veth IP its cage can reach, so the listen address itself
  # is the tier selector (a worker physically cannot reach the research IP).
  ##########################################################################
  services.squid = {
    enable = true;
    extraConfig = ''
      acl anthropic    dstdomain ${lib.concatStringsSep " " workerAllowed}
      acl research_dst dstdomain "${researchAllowlistFile}"
      acl worker_dst   dstdomain "${workerAllowlistFile}"

      acl tier_worker   localip ${workerHostIp}
      acl tier_research localip ${researchHostIp}

      acl CONNECT_method method CONNECT
      acl ssl_port port 443

      http_port ${workerHostIp}:${toString squidWorkerPort}
      http_port ${researchHostIp}:${toString squidResearchPort}

      # Only CONNECT to :443 for allowlisted hosts on the matching tier.
      http_access allow tier_worker   CONNECT_method ssl_port worker_dst
      http_access allow tier_research CONNECT_method ssl_port research_dst
      http_access deny all

      # squid resolves on the cage's behalf (cages have no DNS); keep it quiet.
      dns_v4_first on
      access_log stdio:/var/log/squid/access.log
    '';
  };

  ##########################################################################
  # 4. polkit — let sulin start ONLY the namespaced launch scope
  #
  # bin/agent-launch --profile worker runs:
  #   systemd-run --system --scope \
  #     -p NetworkNamespacePath=/run/netns/claude-worker -p User=sulin \
  #     -- claude ...
  # PID 1 enters the netns and drops to sulin BEFORE exec, so the worker never
  # holds CAP_SYS_ADMIN. This rule lets sulin do that one thing passwordless.
  ##########################################################################
  security.polkit.extraConfig = ''
    polkit.addRule(function(action, subject) {
      if (action.id == "org.freedesktop.systemd1.manage-units" &&
          subject.user == "sulin") {
        var u = action.lookup("unit") || "";
        if (u.indexOf("claude-worker") === 0 || u.indexOf("claude-research") === 0)
          return polkit.Result.YES;
      }
    });
  '';

  # /run/netns persists named namespaces; nothing else to do — `ip netns add`
  # in §1 creates /run/netns/claude-worker that NetworkNamespacePath references.
}
