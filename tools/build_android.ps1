# Builds figmaplay.apk (no gradle): NDK cross-compile both ABIs, stage
# assets, aapt package + zipalign + apksigner (debug key).
# Prereqs: tools\build_thorvg_android.cmd, Android SDK at D:\devlib\android\sdk.
$ErrorActionPreference = "Stop"

$SDK = "D:\devlib\android\sdk"
$NDK = "$SDK\ndk\27.2.12479018"
$BT = "$SDK\build-tools\35.0.1"
$JAR = "$SDK\platforms\android-34\android.jar"
$CMAKE = "C:\Program Files\CMake\bin\cmake.exe"
$NINJA = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $NINJA) { $NINJA = "C:\WINDOWS\ninja.exe" }
$REPO = Split-Path $PSScriptRoot -Parent
$DESIGN = "$REPO\..\fig2psd\test\figma\wallet.fig.export"
$OUT = "$REPO\build_android"

# 1) Native libs per ABI.
$abis = @{ "arm64-v8a" = "arm64"; "x86_64" = "x64" }
foreach ($abi in $abis.Keys) {
    $bdir = "$OUT\native-$($abis[$abi])"
    if (-not (Test-Path "$bdir\build.ninja")) {
        # cmd /c merges stderr: PS 5.1 + ErrorActionPreference=Stop would
        # otherwise abort on the NDK toolchain's deprecation warnings.
        cmd /c "`"$CMAKE`" -B `"$bdir`" -S `"$REPO`" -G Ninja -DCMAKE_TOOLCHAIN_FILE=`"$NDK\build\cmake\android.toolchain.cmake`" -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=`"$NINJA`" -DFETCHCONTENT_SOURCE_DIR_RAYLIB=`"$REPO\build\_deps\raylib-src`" -DFETCHCONTENT_SOURCE_DIR_QUICKJS=`"$REPO\build\_deps\quickjs-src`" 2>&1"
        if ($LASTEXITCODE) { throw "cmake configure failed ($abi)" }
    }
    cmd /c "`"$NINJA`" -C `"$bdir`" figmaplay 2>&1"
    if ($LASTEXITCODE) { throw "build failed ($abi)" }
}

# 2) Staging: libs + assets (+ manifest.txt — AAssetDir cannot list subdirs).
$stage = "$OUT\stage"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
foreach ($abi in $abis.Keys) {
    New-Item -ItemType Directory -Force "$stage\lib\$abi" | Out-Null
    Copy-Item "$OUT\native-$($abis[$abi])\libfigmaplay.so" "$stage\lib\$abi\"
}
New-Item -ItemType Directory -Force "$stage\assets\scripts", "$stage\assets\fonts" | Out-Null
Copy-Item -Recurse $DESIGN "$stage\assets\assets\wallet"
Copy-Item "$REPO\examples\scripts\wallet.js" "$stage\assets\scripts\"
Copy-Item "$REPO\examples\assets\fonts\*.ttf" "$stage\assets\fonts\"
$assetRoot = "$stage\assets"
$manifest = Get-ChildItem $assetRoot -Recurse -File | ForEach-Object {
    $_.FullName.Substring($assetRoot.Length + 1).Replace("\", "/")
}
Set-Content "$assetRoot\manifest.txt" ($manifest -join "`n") -Encoding ascii

# 3) Package: aapt (manifest + assets), aapt add (libs), align, sign.
$unsigned = "$OUT\figmaplay-unsigned.apk"
Remove-Item $unsigned, "$OUT\figmaplay-aligned.apk", "$OUT\figmaplay.apk" -ErrorAction SilentlyContinue
& "$BT\aapt.exe" package -f -F $unsigned -M "$REPO\apps\figmaplay\android\AndroidManifest.xml" -I $JAR -A $assetRoot
if ($LASTEXITCODE) { throw "aapt package failed" }
Push-Location $stage
& "$BT\aapt.exe" add $unsigned "lib/arm64-v8a/libfigmaplay.so" "lib/x86_64/libfigmaplay.so" | Out-Null
if ($LASTEXITCODE) { Pop-Location; throw "aapt add libs failed" }
Pop-Location
& "$BT\zipalign.exe" -f -p 4 $unsigned "$OUT\figmaplay-aligned.apk"
if ($LASTEXITCODE) { throw "zipalign failed" }

$ks = "$env:USERPROFILE\.android\debug.keystore"
if (-not (Test-Path $ks)) {
    New-Item -ItemType Directory -Force (Split-Path $ks) | Out-Null
    & "$env:JAVA_HOME\bin\keytool.exe" -genkeypair -keystore $ks -alias androiddebugkey `
        -storepass android -keypass android -keyalg RSA -validity 10000 `
        -dname "CN=Android Debug,O=Android,C=US"
}
& "$BT\apksigner.bat" sign --ks $ks --ks-pass pass:android --key-pass pass:android `
    --out "$OUT\figmaplay.apk" "$OUT\figmaplay-aligned.apk"
if ($LASTEXITCODE) { throw "apksigner failed" }
Remove-Item $unsigned, "$OUT\figmaplay-aligned.apk", "$OUT\figmaplay.apk.idsig" -ErrorAction SilentlyContinue
Write-Host "OK -> $OUT\figmaplay.apk"
