"""Local build using pinned Google SDK 35 / NDK r25c; no Gradle dependency."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, zipfile

ROOT = Path(__file__).resolve().parent
VERSION = '0.2.8-experimental'
VERSION_CODE = 18
TOOLS = ROOT.parent / '.analysis' / 'tools'
SDK = TOOLS / 'android-sdk'
BUILD = ROOT / 'build'
JDK = Path(os.environ.get('JAVA_HOME', r'C:\Program Files\Microsoft\jdk-17.0.20.8-hotspot'))
BT = SDK / 'build-tools' / '35.0.0'
ANDROID = SDK / 'platforms' / 'android-35' / 'android.jar'
NDK = TOOLS / 'android-ndk-r25c' / 'toolchains' / 'llvm' / 'prebuilt' / 'windows-x86_64'
ENV = dict(os.environ, JAVA_HOME=str(JDK), PATH=str(JDK / 'bin') + os.pathsep + os.environ.get('PATH',''))

def run(*args):
    subprocess.run([str(a) for a in args], check=True, env=ENV, cwd=ROOT)

def native():
    BUILD.mkdir(exist_ok=True)
    run(NDK / 'bin' / 'clang.exe', '--target=aarch64-linux-android29',
        '--sysroot=' + str(NDK / 'sysroot'), '-O2', '-Wall', '-Wextra', '-Werror',
        '-fPIE', '-pie', '-Wl,-z,relro,-z,now', '-o', BUILD / 'libcelllock.so',
        ROOT / 'native' / 'main.c', ROOT / 'native' / 'transport.c', ROOT / 'native' / 'efs.c',
        ROOT / 'native' / 'item_probe.c', ROOT / 'native' / 'legacy_pci.c',
        ROOT / 'native' / 'profile.c', ROOT / 'native' / 'modem_sync.c', '-ldl')

def build():
    native()
    for name in ['generated','classes','dex']:
        # Only reset these fixed build output directories, never sources or signing keys.
        directory = BUILD/name
        if directory.is_symlink() or directory.resolve().parent != BUILD.resolve():
            raise RuntimeError('Unsafe build output directory: ' + str(directory))
        if directory.exists(): shutil.rmtree(directory)
        directory.mkdir()
    run(BT/'aapt2.exe','compile','--dir',ROOT/'app/src/main/res','-o',BUILD/'resources.zip')
    run(BT/'aapt2.exe','link','-o',BUILD/'base.apk','-I',ANDROID,
        '--manifest',ROOT/'app/src/main/AndroidManifest.xml','--java',BUILD/'generated',
        '--min-sdk-version','29','--target-sdk-version','32','--version-code',str(VERSION_CODE),
        '--version-name',VERSION,BUILD/'resources.zip')
    sources=sorted((ROOT/'app/src/main/java').rglob('*.java'))+sorted((BUILD/'generated').rglob('*.java'))
    argfile=BUILD/'javac-args.txt'
    argfile.write_text('\n'.join('"'+p.as_posix()+'"' for p in sources),encoding='utf-8')
    run(JDK/'bin/javac.exe','-encoding','UTF-8','--release','8','-classpath',ANDROID,
        '-d',BUILD/'classes','@'+str(argfile))
    run(JDK/'bin/jar.exe','--create','--file',BUILD/'classes.jar','-C',BUILD/'classes','.')
    run(BT/'d8.bat','--min-api','29','--lib',ANDROID,'--output',BUILD/'dex',BUILD/'classes.jar')
    with zipfile.ZipFile(BUILD/'base.apk') as original, zipfile.ZipFile(BUILD/'unsigned.apk','w') as apk:
        for member in original.infolist(): apk.writestr(member,original.read(member))
        apk.write(BUILD/'dex/classes.dex','classes.dex',compress_type=zipfile.ZIP_DEFLATED)
        apk.write(BUILD/'libcelllock.so','lib/arm64-v8a/libcelllock.so',compress_type=zipfile.ZIP_DEFLATED)
    run(BT/'zipalign.exe','-f','-p','4',BUILD/'unsigned.apk',BUILD/'aligned.apk')
    signing=ROOT/'.signing'
    signing.mkdir(exist_ok=True)
    keystore=signing/'development.p12'
    if not keystore.exists():
        run(JDK/'bin/keytool.exe','-genkeypair','-keystore',keystore,'-storetype','PKCS12',
            '-storepass','android','-keypass','android','-alias','celllock-development',
            '-keyalg','RSA','-keysize','3072','-validity','3650','-dname','CN=Local CellLock Development')
    output=ROOT/'dist'
    output.mkdir(exist_ok=True)
    apk=output/('LteCellLock-'+VERSION+'.apk')
    run(BT/'apksigner.bat','sign','--ks',keystore,'--ks-key-alias','celllock-development',
        '--ks-pass','pass:android','--key-pass','pass:android','--out',apk,BUILD/'aligned.apk')
    run(BT/'apksigner.bat','verify','--verbose','--print-certs',apk)
    run(BT/'zipalign.exe','-c','-p','4',apk)
    metadata={'apk':apk.name,'version':VERSION,'version_code':VERSION_CODE,
              'sha256':hashlib.sha256(apk.read_bytes()).hexdigest(),
              'sdk':35,'build_tools':'35.0.0','ndk':'r25c','min_sdk':29,'target_sdk':32,
              'signing':'local development key (not a production distribution identity)'}
    (output/'build-info.json').write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    (output/('build-info-'+VERSION+'.json')).write_text(json.dumps(metadata,indent=2),encoding='utf-8')
    print(json.dumps(metadata,indent=2))

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--native-only',action='store_true')
    opts=parser.parse_args()
    native() if opts.native_only else build()
