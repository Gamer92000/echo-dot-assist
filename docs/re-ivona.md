# Ivona: Fire OS's offline voice (checkers)

The Echo Show 5's image carries Amazon's offline speech engine as an Android TTS app (`priv-app/IvonaTTS`, JNI glue
`lib/arm/libttsivona.so`, SDK 1.6.126) plus `priv-app/IvonaTtsOrchestrator` and `priv-app/ContentSupportProvider`
(Amazon's "content support" language packs), but no voice: `/etc/ivona/addons/eng.usa.nina_*/ivona.addon` only names
`vox_en_us_nina`. Alexa's own replies are made in the cloud; this is the voice Fire OS uses offline (VoiceView and
the like). Whether "Nina" is the voice Alexa started from is not known.

## Getting a voice (2026-11-02)

- ContentSupportProvider asks DAVS V2 for the pack list: artifactType `humanio`, key `manifest`, filters
  `name ["manifest-arm32"]` (`manifest-` + ABI, armeabi-v7a -> arm32) and `language ["all"]`, token in
  `x-amz-access-token` (`DavsManifestDownloadManager`, `ServerCall`). For the Show 5's device type DAVS answers
  HTTP 403 "Request not allowlisted for device Type A4ZP7ZC4PI6TO for artifact type humanio, artifact key manifest".
- The image has the list anyway: `/system/etc/contentsupport.json` (2021-09-03, 16 voices: Nina en-US, Abbey en-GB,
  Silke de-DE, Yumiko ja-JP, Ira en-IN and hi-IN, Evelyn en-CA, Ella en-AU, Chloe fr-FR, Giulia it-IT, Alicia es-ES,
  Fernanda es-MX, Charlotte fr-CA, Mariana pt-BR, Candela es-US, Sara ar-SA). Every pack has `storage_provider` "s3"
  and no `bucket_name`, which `langpackage-client`'s `LangPackage.getFullSourceUrl` turns into
  `https://s3.amazonaws.com/<source_url>`: public, no token. Nina: 289 248 046 bytes, as the JSON says.
- `tools/davs-fetch.py tts [voice|list] [outdir]` downloads and unpacks one. Proprietary: `device-logs/` only.
- A pack is a jar (zip, signed `JAVASPEC`): `libttsivonavoice.so` (the engine, exported C API `tts_create`,
  `tts_load_certificate`, `tts_load_language`, `tts_load_voice`, `tts_streamer_start`/`_synth`/`_stop`,
  `tts_wave_delete`, `tts_voice_sample_rate`, `tts_errmsg`...), `libtiver.so` (exports `v` and `h`, which the glue
  `dlsym`s first: a check of the voice package), `lang_en_us.dat`, `libvoice_en_us_nina.so` (160 MB, exports
  `register_voice`, `language_data`) and `vox_en_us_nina22i-ta` (264 MB). 431 MB unpacked; the Show 5 has 3.3 GB free.
  ContentSupportProvider unpacks it to `tts/ivona/<install location>_<version ms>/` under
  `/data/securedStorageLocation/com.amazon.providers.contentsupport/`.

## Running it without Java

`tts_init(cert)` = `tts_create()` + `tts_load_certificate(tts, cert)`, which refuses NULL, and no certificate is in
plain text anywhere. Not needed: the glue does all of it. A fake JNIEnv in a C program (experiment, kept in
`device-logs/ivona/harness/jnihost.c`) that `dlopen`s `libttsivona.so` (+ the app's `libc++_shared.so`) and calls, as
`IVONATextToSpeechService` and `IvonaPackagesInfo` do:

1. `JNI_OnLoad` (looks up classes and `getClassLoader`/`findClass`; harmless to answer with stubs).
2. `NativeIVONAEngine.setEnvRuntime(dataParent, sdcard)`: the folder holding `com.ivona.tts.oem/files/`
   (normally `/data/data/`).
3. `ConfigXMLDataSet.getPackagesCfg()` -> `com.ivona.tts.oem.voicebeta.eng.usa.nina`, and
   `IvonaPackagesInfo.getOEMpackages()` -> `2;2;IVONA Nina;ivona_sdk-1.6.118.1;<voice dir>/;vox_en_us_nina22i-ta`.
   The glue's built-in config searches `/data/securedStorageLocation/com.amazon.providers.contentsupport/tts/ivona/`
   (as `/etc/ivona/ivona_config.xml`); `/data/tts/ivona/` was not scanned.
4. Java then writes `<dataParent>com.ivona.tts.oem/files/ivona_pkgs.csv`: a line with the time in ms, then per voice
   `<package cfg>;0;<OEM package line>`. Without it: logcat "Voices vector is empty", `onLoadLanguage` -100.
5. `create()`, `onLoadLanguage("eng", "USA", "")` -> 0, `getSampleRate()` -> 22050.
6. `NativeIVONAEngineICS.synthesizeText(text, byte[] buf)`: fills `buf` and calls the object's
   `audioAvailableClbk(int bytes, int status)` (status 1 while it speaks, 0 with 0 bytes at the end). 16-bit mono.

On the Show 5 (2026-11-02, as root, voice in Amazon's folder): "Hello! I am your Echo Show. The kitchen light is now
on, and your timer has five minutes left." -> 6.5 s at 22 050 Hz, peak -1.4 dBFS. logcat only "could not set default
lang code" and the orchestrator's download intent (not installed) as warnings.

Speed on the Show 5 (MT8163): `create` ~0 ms, `onLoadLanguage` 4.4 s (the voice loaded; once per process), then
"The kitchen light is now on." first audio after 192 ms, 1.49 s of speech in 258 ms; a 26-word sentence in 936 ms.
So the voice is loaded at start and kept.

Open: as the daemon's user (system) instead of root; memory held while loaded; `setProperty`
rate/pitch; the glue under hassmic (dlopen at run time, feature off without the voice; the fake JNIEnv as a small C
file); a download on the settings page.
