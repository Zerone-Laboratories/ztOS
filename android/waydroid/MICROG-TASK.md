# Waydroid: make WhatsApp see an official environment, like ATL does (microG)
Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Don't delete WhatsApp data (ATL or Waydroid). Never send messages.
Other agents: Superscan (ztmetro/browser), Bluetooth, keyboard (KEYS-TASK.md) — don't touch.
Read android/waydroid/WA-LINK.md (history: framework.jar attestation patches were RESTORED to stock;
build props now spoof a stock Samsung S21 Android 13 user/release-keys build; Instagram is
disabled-user inside Waydroid for the test).

Why ATL works and Waydroid doesn't (orchestrator, from ATL source
android/atl/... api-impl/android/atl/ATLGStub.java, ATLSigHelper, ContextImpl, AndroidKeyStore):
ATL fakes com.google.android.gms / gsf / com.android.vending packages WITH Google's real signing
certificates and microG version metadata, intercepts c2dm/FCM REGISTER, and its KeyStore returns
EMPTY certificate chains (no attestation). Waydroid VANILLA has no GMS at all
("com.whatsapp requires the Google Play Store, but it is missing") -> WhatsApp: "Download the
official WhatsApp" on phone-number registration and "Couldn't link this device" for companion.

Do:
1. Install microG into the Waydroid image reproducibly (script in android/waydroid/): GmsCore,
   GsfProxy if needed, FakeStore (com.android.vending), as privileged system apps with their
   permissions xml, plus SIGNATURE SPOOFING so they present Google's signature (LineageOS 20 has no
   spoofing: patch framework.jar/services.jar the way microG/LineageOS-for-microG or
   waydroid_script (casualsnek) do it for Android 13; keep DEX entries stored + 4-byte aligned —
   an earlier unaligned repack made every app lag). Verify with microG Self-Check.
2. Make key attestation behave like ATL (no attestation chain) only if needed after step 1.
3. Set WhatsApp's install source to the Play Store (`pm set-installer com.whatsapp
   com.android.vending` / install-source) without reinstalling (keep data).
4. Re-enable Instagram afterwards only if the user's next test passes without it (keep disabled for
   the test).
5. Write /var/tmp/wa-link/RETRY when ready; keep a logcat capture running for the user's attempt.
Report whether WhatsApp's welcome screen still shows the "custom ROM" alert. Update WA-LINK.md.
