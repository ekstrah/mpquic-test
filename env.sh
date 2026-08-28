#!/usr/bin/env bash
# Sourced by every script in this repo.
#
# Carried over from the TQUIC rig (multipath-quic repo) - same physical
# boxes, same wiring. CONFIRM these interface names still match with
# `ip -brief link show` on each box before trusting the shaping scripts -
# don't assume without checking.

# --- assign these yourself, one file per box (see note at bottom) ---
SERVER_IFACE_A=enxf8e43b965cdc   # SERVER NIC wired to CLIENT_IFACE_A (LEO link)
SERVER_IFACE_B=enxf8e43b9eaa06   # SERVER NIC wired to CLIENT_IFACE_B (Mobile link)
SERVER_IFACE_C=enx00e04c041621   # SERVER NIC wired to CLIENT_IFACE_C (Mesh link)

CLIENT_IFACE_A=enx00e04c680170   # CLIENT NIC wired to SERVER_IFACE_A (LEO link)
CLIENT_IFACE_B=enxf8e43b9e8eee   # CLIENT NIC wired to SERVER_IFACE_B (Mobile link)
CLIENT_IFACE_C=enxf8e43b9e8bfa   # CLIENT NIC wired to SERVER_IFACE_C (Mesh link)

# --- fixed addressing, do not change without updating both boxes ---
LINK_A_SUBNET=172.16.1.0/30
LINK_A_SERVER_IP=172.16.1.1
LINK_A_CLIENT_IP=172.16.1.2

LINK_B_SUBNET=172.16.2.0/30
LINK_B_SERVER_IP=172.16.2.1
LINK_B_CLIENT_IP=172.16.2.2

LINK_C_SUBNET=172.16.3.0/30
LINK_C_SERVER_IP=172.16.3.1
LINK_C_CLIENT_IP=172.16.3.2

SERVER_CANONICAL_IP=10.99.0.1
QUIC_PORT=4433

# --- Link A ("LEO") netem/tbf targets ---
LINK_A_RATE_DOWN_MBIT=62   # server->client (applied as egress shaping on SERVER)
LINK_A_RATE_UP_MBIT=18     # client->server (applied as egress shaping on CLIENT)
LINK_A_DELAY_MS=25
LINK_A_JITTER_MS=13
LINK_A_LOSS_PCT=0.17

# --- Link B ("Mobile") netem/tbf targets ---
LINK_B_RATE_MBIT=30        # symmetric static approximation
LINK_B_DELAY_MS=50
LINK_B_JITTER_MS=5
LINK_B_LOSS_PCT=0.006

# --- Link C ("Mesh") netem/tbf targets ---
# PLACEHOLDER: not derived from real mesh-radio measurements. Replace with
# real numbers once a mesh radio is characterized.
LINK_C_RATE_MBIT=15        # symmetric static approximation
LINK_C_DELAY_MS=20
LINK_C_JITTER_MS=8
LINK_C_LOSS_PCT=0.5

# --- sweep: every CC algorithm this picoquic build registers (confirmed
# via `picoquicdemo -h`'s dynamically-generated -G list, not guessed from
# source filenames - scone/dualq_aqm exist as source files but aren't
# actually wired into this build's selectable list). picoquic has no
# swappable scheduler (Phase 0 finding), so unlike the TQUIC sweep this is
# 1-dimensional (CC only), not scheduler x CC. ---
SWEEP_CC_LIST=(newreno cubic dcubic fast bbr prague bbr1 c4)
SWEEP_WINDOW_SEC=60          # SAFETY-NET timeout only (server waits for the
                             # actual connection to close via -1, doesn't
                             # sleep this long normally - see run-server-sweep.sh).
                             # Generous since it's cost-free in the normal path.
SWEEP_CLIENT_DELAY_SEC=5     # CLIENT startup delay, lets the fresh server come up

# NOTE: this file is the same on both boxes at clone time. Only the three
# *_IFACE_* lines for your box differ in practice (net-server.sh only
# reads SERVER_IFACE_*, net-client.sh only reads CLIENT_IFACE_*), so
# leaving the other box's values unconfirmed on your local edit is
# harmless - just don't commit unconfirmed values for the box you're
# about to run a script on.
