# WhatsApp (Waydroid): "Couldn't link this device"

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Do not delete WhatsApp data in either runtime; never send messages.
Read android/waydroid/README.md, PHASE2.md, RAM-BOOT.md (Android was just trimmed: launcher,
systemui, background-app restrictions, memory settings — any of these may be the cause).

User: logging into WhatsApp in Waydroid ("WhatsApp (Waydroid)" tile) by linking it as a
companion device fails with "Couldn't link this device", while the same linking works in the
ATL (ztOS runtime) WhatsApp. The user's primary phone does the linking.

Do:
1. Start a persistent capture before the user's next attempt: `waydroid logcat` (or adb/logcat
   inside the container) filtered to WhatsApp + network + Play services, plus host-side network
   (tcpdump on waydroid0 for DNS/TLS failures if available), written to
   /var/tmp/wa-link/. Make the capture survive container freeze/thaw.
2. Write /var/tmp/wa-link/READY with instructions, and keep polling the logs (max 45 min) for the
   failure; the orchestrator tells the user to try linking now.
3. Static checks meanwhile: Waydroid network (DNS, IPv6, MTU on waydroid0, captive/HTTP 204,
   time/date sync inside Android vs host — TLS and linking are time-sensitive), the RAM-BOOT
   restrictions (background restrictions / standby bucket / disabled components WhatsApp needs:
   JobScheduler, WorkManager, its push/FCM fallback, KeyStore/Keymaster — Waydroid's keystore
   may be broken, Android Keystore errors would break key generation for companion linking),
   SafetyNet/Play-Integrity absence (no GMS in Waydroid vs ATL behaviour), device time zone,
   clock skew, and whether the container is frozen mid-link (freeze policy while WhatsApp is the
   front window must not freeze it).
4. Compare with how ATL's WhatsApp gets these (ATL's KeyStore implementation, network path).
5. Fix the cause, then ask for another attempt (write /var/tmp/wa-link/RETRY) and confirm from
   logs. Write android/waydroid/WA-LINK.md with evidence.
