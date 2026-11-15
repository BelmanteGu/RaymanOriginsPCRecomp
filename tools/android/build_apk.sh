#!/bin/sh
# Builds the Android APK private/android/rayman-origins-pc.apk without Gradle: aapt2 (resources), javac + d8 (our Java and
# SDL3's), the native libraries from tools/android/build_native.sh, zipalign and apksigner (a local debug key).
# The launcher art and the app icon come from Imgs/ (Fundo.png, Logo.png, Icone.png: not in git) when present.
# usage: sh tools/android/build_apk.sh            (run tools/android/build_native.sh first)
set -e
SDK=${SDK:-$HOME/Android/Sdk}
BT=$SDK/build-tools/35.0.0
JAR=$SDK/platforms/android-35/android.jar
JDK=${JDK:-$HOME/Documentos/Projetos/Recomps/Skullgirls/deps/jdk}
SDL3_SRC=${SDL3_SRC:-$HOME/Documentos/Projetos/Recomps/Castlevania/private/third_party/SDL3-3.4.0}   # its Java (zlib)
W=private/android/apk
export PATH="$JDK/bin:$PATH"   # d8 and apksigner run on Java
rm -rf $W; mkdir -p $W/res $W/gen $W/classes $W/compiled

# the art (Ubisoft's: kept out of the repository) and the icon, made into Android resources
.venv/bin/python -I - <<'EOF'
import os
from PIL import Image
out = "private/android/apk/res"
if os.path.exists("Imgs/Icone.png"):
    ico = Image.open("Imgs/Icone.png").convert("RGBA")
    for d, s in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        os.makedirs(f"{out}/mipmap-{d}", exist_ok=True)
        ico.resize((s, s), Image.LANCZOS).save(f"{out}/mipmap-{d}/ic_launcher.png")
else:
    os.makedirs(f"{out}/mipmap-mdpi", exist_ok=True)
    Image.new("RGBA", (48, 48), (250, 170, 30, 255)).save(f"{out}/mipmap-mdpi/ic_launcher.png")
os.makedirs(f"{out}/drawable-nodpi", exist_ok=True)
if os.path.exists("Imgs/Fundo.png"):
    Image.open("Imgs/Fundo.png").convert("RGB").save(f"{out}/drawable-nodpi/launcher_bg.jpg", quality=90)
if os.path.exists("Imgs/Logo.png"):
    logo = Image.open("Imgs/Logo.png").convert("RGBA")
    logo.thumbnail((1000, 1000), Image.LANCZOS)
    logo.save(f"{out}/drawable-nodpi/launcher_logo.png")
EOF
cp -r android/res/* $W/res/

$BT/aapt2 compile --dir $W/res -o $W/compiled/res.zip
$BT/aapt2 link -o $W/base.apk -I $JAR --manifest android/AndroidManifest.xml --java $W/gen \
  --min-sdk-version 29 --target-sdk-version 34 --version-code 1 --version-name 0.1 ${USF_DEBUGGABLE:+--debug-mode} $W/compiled/res.zip

find android/java $SDL3_SRC/android-project/app/src/main/java/org/libsdl $W/gen -name '*.java' > $W/sources.txt
$JDK/bin/javac -nowarn --release 17 -classpath $JAR -d $W/classes @$W/sources.txt 2>&1 | grep -v "^Note:" || true
$BT/d8 --release --min-api 29 --lib $JAR --output $W $(find $W/classes -name '*.class')

cp $W/base.apk $W/unsigned.apk
.venv/bin/python -I - <<'EOF'
import glob, os, zipfile
with zipfile.ZipFile("private/android/apk/unsigned.apk", "a", zipfile.ZIP_DEFLATED) as z:
    z.write("private/android/apk/classes.dex", "classes.dex")
    for so in sorted(glob.glob("private/android/lib/arm64-v8a/*.so")):
        z.write(so, "lib/arm64-v8a/" + os.path.basename(so))
EOF
$BT/zipalign -f -P 16 4 $W/unsigned.apk $W/aligned.apk

KEY=private/android/debug.keystore
[ -f $KEY ] || $JDK/bin/keytool -genkeypair -keystore $KEY -storepass android -keypass android -alias rayman \
  -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Rayman Origins PC Recomp" > /dev/null 2>&1
$BT/apksigner sign --ks $KEY --ks-pass pass:android --key-pass pass:android --out private/android/rayman-origins-pc.apk $W/aligned.apk
$BT/apksigner verify private/android/rayman-origins-pc.apk && ls -la private/android/rayman-origins-pc.apk
