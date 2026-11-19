# Script do PlatformIO: descarrega a biblioteca OpenDroneID (Apache 2.0) se ainda não existir.
Import("env")  # noqa: F821
import os
import urllib.request

COMMIT = "849c03adbe0cdd68997a462fafab397edcfe53f3"
BASE = f"https://raw.githubusercontent.com/opendroneid/opendroneid-core-c/{COMMIT}/"
FILES = ["libopendroneid/opendroneid.c", "libopendroneid/opendroneid.h", "LICENSE"]

dest = os.path.join(env.subst("$PROJECT_DIR"), "lib", "opendroneid")  # noqa: F821
os.makedirs(dest, exist_ok=True)
for f in FILES:
    out = os.path.join(dest, os.path.basename(f))
    if not os.path.exists(out):
        print(f"[odid] a descarregar {f}")
        urllib.request.urlretrieve(BASE + f, out)
