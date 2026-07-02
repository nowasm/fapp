# Builds a figoplay APK (no gradle): NDK cross-compile both ABIs, stage
# assets, aapt package + zipalign + apksigner (debug key).
# Prereqs: tools\build_thorvg_android.cmd, Android SDK at D:\devlib\android\sdk.
#
# No args -> the wallet demo (build_android\figoplay.apk), as before.
# Driven by figmapack: -AppDir points at a staged standard app dir (app.json +
# design + app.js + fonts), preloaded into the APK at assets/app and read by the
# runtime; -PackageId/-AppName/-VersionName/-VersionCode/-OutApk set metadata.
param(
    [string]$AppDir = "",
    [string]$PackageId = "com.figo.play",
    [string]$AppName = "figoplay",
    [string]$VersionName = "1.0",
    [int]$VersionCode = 1,
    [string]$OutApk = "",
    [string]$ResDir = ""   # res/ with mipmap-*/ic_launcher.png -> launcher icon
)
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
if (-not $OutApk) { $OutApk = "$OUT\figoplay.apk" }

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
    cmd /c "`"$NINJA`" -C `"$bdir`" figoplay 2>&1"
    if ($LASTEXITCODE) { throw "build failed ($abi)" }
}

# 2) Staging: libs + assets (+ manifest.txt — AAssetDir cannot list subdirs).
$stage = "$OUT\stage"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
foreach ($abi in $abis.Keys) {
    New-Item -ItemType Directory -Force "$stage\lib\$abi" | Out-Null
    Copy-Item "$OUT\native-$($abis[$abi])\libfigoplay.so" "$stage\lib\$abi\"
}
$assetRoot = "$stage\assets"
New-Item -ItemType Directory -Force $assetRoot | Out-Null
if ($AppDir) {
    # A packaged app: the runtime reads assets/app/app.json.
    Copy-Item -Recurse $AppDir "$assetRoot\app"
} else {
    # Legacy wallet demo layout.
    New-Item -ItemType Directory -Force "$assetRoot\scripts", "$assetRoot\fonts" | Out-Null
    Copy-Item -Recurse $DESIGN "$assetRoot\assets\wallet"
    Copy-Item "$REPO\examples\scripts\wallet.js" "$assetRoot\scripts\"
    Copy-Item "$REPO\examples\assets\fonts\*.ttf" "$assetRoot\fonts\"
}
$manifest = Get-ChildItem $assetRoot -Recurse -File | ForEach-Object {
    $_.FullName.Substring($assetRoot.Length + 1).Replace("\", "/")
}
Set-Content "$assetRoot\manifest.txt" ($manifest -join "`n") -Encoding ascii

# 2b) AndroidManifest with the app's package id / version / label / icon / splash.
$iconAttr = if ($ResDir) { ' android:icon="@mipmap/ic_launcher"' } else { "" }
# figmapack writes a splash theme (windowBackground) into the res dir alongside.
$themeAttr = if ($ResDir -and (Test-Path "$ResDir\values\styles.xml")) { ' android:theme="@style/AppSplash"' } else { "" }
$manifestXml = @"
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="$PackageId" android:versionCode="$VersionCode" android:versionName="$VersionName">
  <uses-sdk android:minSdkVersion="28" android:targetSdkVersion="34"/>
  <uses-permission android:name="android.permission.INTERNET"/>
  <application android:label="$AppName"$iconAttr android:hasCode="false" android:extractNativeLibs="true">
    <activity android:name="android.app.NativeActivity" android:exported="true"$themeAttr
              android:configChanges="orientation|keyboardHidden|screenSize">
      <meta-data android:name="android.app.lib_name" android:value="figoplay"/>
      <intent-filter>
        <action android:name="android.intent.action.MAIN"/>
        <category android:name="android.intent.category.LAUNCHER"/>
      </intent-filter>
    </activity>
  </application>
</manifest>
"@
# aapt requires the -M file to be literally named AndroidManifest.xml; write the
# generated one into a dedicated dir (not the source apps/figoplay/android one).
# UTF-8 *without* BOM — aapt fails ("No AndroidManifest.xml file found") on a BOM.
$genDir = "$OUT\gen"
New-Item -ItemType Directory -Force $genDir | Out-Null
$manifestPath = "$genDir\AndroidManifest.xml"
[System.IO.File]::WriteAllText($manifestPath, $manifestXml, (New-Object System.Text.UTF8Encoding $false))

# 3) Package: aapt (manifest + assets), aapt add (libs), align, sign.
$unsigned = "$OUT\figoplay-unsigned.apk"
$aligned = "$OUT\figoplay-aligned.apk"
Remove-Item $unsigned, $aligned, $OutApk -ErrorAction SilentlyContinue
$aaptArgs = @("package", "-f", "-F", $unsigned, "-M", $manifestPath, "-I", $JAR, "-A", $assetRoot)
if ($ResDir) { $aaptArgs += @("-S", $ResDir) }
& "$BT\aapt.exe" @aaptArgs
if ($LASTEXITCODE) { throw "aapt package failed" }
Push-Location $stage
& "$BT\aapt.exe" add $unsigned "lib/arm64-v8a/libfigoplay.so" "lib/x86_64/libfigoplay.so" | Out-Null
if ($LASTEXITCODE) { Pop-Location; throw "aapt add libs failed" }
Pop-Location
& "$BT\zipalign.exe" -f -p 4 $unsigned $aligned
if ($LASTEXITCODE) { throw "zipalign failed" }

$ks = "$env:USERPROFILE\.android\debug.keystore"
if (-not (Test-Path $ks)) {
    New-Item -ItemType Directory -Force (Split-Path $ks) | Out-Null
    & "$env:JAVA_HOME\bin\keytool.exe" -genkeypair -keystore $ks -alias androiddebugkey `
        -storepass android -keypass android -keyalg RSA -validity 10000 `
        -dname "CN=Android Debug,O=Android,C=US"
}
New-Item -ItemType Directory -Force (Split-Path $OutApk) | Out-Null
& "$BT\apksigner.bat" sign --ks $ks --ks-pass pass:android --key-pass pass:android `
    --out $OutApk $aligned
if ($LASTEXITCODE) { throw "apksigner failed" }
Remove-Item $unsigned, $aligned, "$OutApk.idsig" -ErrorAction SilentlyContinue
Write-Host "OK -> $OutApk"
