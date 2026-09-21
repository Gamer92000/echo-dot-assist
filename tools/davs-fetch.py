#!/usr/bin/env python3
"""Fetch a Pryon wake-word model set from Amazon's DAVS, the way the Echo's assetmgrd does (seen with src/tools/curlspy.c):
  GET https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=<url-quoted base64 of the request JSON>
  Authorization: Bearer <access token of a registered device>
The JSON answer carries a signed CloudFront downloadUrl that expires within minutes, so the request is the thing to keep.
  tools/davs-fetch.py <map.db> <key> [locale] [outdir]      key: alexa echo computer amazon ziggy; locale default de-DE
map.db is /data/ace/kvstorage/map.db of the registered Echo; its access token lasts an hour after the device fetched it.
"""
import base64, json, pathlib, sqlite3, sys, tarfile, urllib.parse, urllib.request

ENGINE_IDS = [str(i) for i in (1, 10, 11, 12, 13, 14, 15, 16, 17, 19, 2, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
                               34, 35, 36, 37, 4, 5, 6, 7, 8, 9)]      # what NS65741's PuffinApp sends

def main():
    if len(sys.argv) < 3: sys.exit(__doc__)
    db, key = sys.argv[1], sys.argv[2]
    locale = sys.argv[3] if len(sys.argv) > 3 else "de-DE"
    out = pathlib.Path(sys.argv[4] if len(sys.argv) > 4 else "device-logs/models")
    token = sqlite3.connect(db).execute("select cast(value as text) from deviceData where key='access_token'").fetchone()[0]
    req = {"artifactType": "wakeword", "artifactKey": key,
           "filters": {"engineCompatibilityIdList": ENGINE_IDS, "locale": [locale], "modelClass": ["B"]}}
    enc = urllib.parse.quote(base64.b64encode(json.dumps(req, separators=(",", ":")).encode()).decode(), safe="")
    url = "https://api.amazonalexa.com/v2/deviceArtifacts/?artifactFilter=" + enc
    with urllib.request.urlopen(urllib.request.Request(url, headers={"Authorization": "Bearer " + token}), timeout=30) as r:
        info = json.load(r)
    dest = out / f"{key}-{locale}"
    dest.mkdir(parents=True, exist_ok=True)
    (dest / "davs.json").write_text(json.dumps(info, indent=1))
    tgz = dest / "artifact.tar.gz"
    with urllib.request.urlopen(info["downloadUrl"], timeout=120) as r: tgz.write_bytes(r.read())
    with tarfile.open(tgz) as t: t.extractall(dest / "unpacked", filter="data")
    print(f"{key} {locale}: {tgz.stat().st_size} bytes, id {info.get('artifactIdentifier', '?')} -> {dest}")

if __name__ == "__main__":
    main()
