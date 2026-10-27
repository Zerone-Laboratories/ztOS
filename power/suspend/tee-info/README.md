# Trustonic status diagnostic

Temporary information-only module for the Galaxy S9's matching
`7.1.0-zboot` kernel. Not installed or loaded by the OS installer.

Loading issues one stock `MC_FC_INFO` (`0xff000002`) request with extended
information ID 18 on CPU0. It exposes raw response, result, state, and
extended fields as read-only module parameters. It does not initialize
Trustonic, switch cores, change watchdog routing, or enter sleep.

The pinned Samsung driver defines status 0 as not initialized, 1 as bad
initialization, 2 as initialized, and 3 as halted. Interpret status only
with a successful result (0); keep failed or unsupported queries unknown.

Verified live query: 2026-10-22 04:01:22 UTC, CPU0,
`response=ff000002 result=0 state=0 extended=0`. Module unloaded afterward;
SSH and the sleep daemon remained available. See
[`../secure-handoff-audit.md`](../secure-handoff-audit.md) for source and
the limits of this finding.
