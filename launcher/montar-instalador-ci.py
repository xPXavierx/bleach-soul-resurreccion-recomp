"""Builds "Bleach Soul Resurrección - Setup.exe" from this repository, on GitHub Actions (or any Windows PC).

Same package as montar-instalador.py, but it only uses what is in the repository plus the official
third-party builds, downloaded here and checked against the files shipped in the v1.1 setup:
  - RPCS3 v0.0.30-16061-85f4c38b (GPL v2), from github.com/RPCS3/rpcs3-binaries-win
  - FFmpeg essentials 2026-09-02-git-9fc8c785e2 (GPL v3), gyan.dev build from github.com/GyanD/codexffmpeg
It skips the hand-edit check of montar-instalador.py (that one needs the game's EBOOT, which never
goes to GitHub); the edits used are the committed bleach-recomp/edicoes-recompilado.json.
Usage: python launcher/montar-instalador-ci.py --rpcs3 <rpcs3 folder> --ffmpeg <ffmpeg folder> [--saida <folder>]"""
import argparse, hashlib, shutil, struct, subprocess, zipfile
from pathlib import Path

aqui = Path(__file__).resolve().parent          # launcher/
raiz = aqui.parent                              # repository root
NOME = 'Bleach Soul Resurrección - Setup.exe'
csc = Path('C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe')
HOST = ['CMakeLists.txt', 'main.cpp', 'stubs.cpp', 'sail_bridge.cpp', 'generate_spu_variants.py',
        'generate_native_kernels.py', 'patch_game_timing.py', 'generate_fonts.py', 'spu-manifest.json',
        'font-manifest.json', 'construir.py', 'gerar_edicoes.py', 'edicoes-recompilado.json']
FORA_SDK = {'.git', 'scratch', '__pycache__', 'build', 'out'}
# SHA-256 of the third-party files inside the published v1.1 setup: the CI build must ship the same ones.
ESPERADO = {
    'rpcs3.exe': '4a878bd38d19931d8e6c046be1552bc9e818503fc8ad57deb7229a83a4718105',
    'ffmpeg.exe': '60b074d4cf4f553d4ea8291bce393552549eae6894b0ab0a6346be878c5f7b09',
}


def sha256(f):
    h = hashlib.sha256()
    with open(f, 'rb') as s:
        for b in iter(lambda: s.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def conferir(f):
    h = sha256(f)
    if h != ESPERADO[f.name]:
        raise SystemExit('%s: SHA-256 %s, expected %s' % (f, h, ESPERADO[f.name]))
    print('ok  %s  %s' % (f.name, h))


def copiar(origem, destino):
    destino.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origem, destino)


def compilar_cs(saida_exe, fontes, extras):
    subprocess.run([str(csc), '/nologo', '/target:winexe', '/codepage:65001', '/win32icon:icone-recomp.ico', '/define:ULTRAWIDE',
                    '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll',
                    '/r:System.Web.Extensions.dll', '/lib:' + str(csc.parent / 'WPF'), '/r:UIAutomationClient.dll', '/r:UIAutomationTypes.dll'] + extras + ['/out:' + str(saida_exe)] + fontes,
                   check=True, cwd=aqui)


def kit(destino):
    sdk = raiz / 'ps3recomp'
    for f in sdk.rglob('*'):
        rel = f.relative_to(sdk)
        if not f.is_file() or set(rel.parts) & FORA_SDK or f.suffix.lower() == '.pdf':
            continue
        copiar(f, destino / 'ps3recomp' / rel)
    host = raiz / 'bleach-recomp'
    for nome in HOST:
        copiar(host / nome, destino / 'bleach-recomp' / nome)
    for h in host.glob('native_*.h'):
        copiar(h, destino / 'bleach-recomp' / h.name)


def montar(rpcs3, ffmpeg, saida):
    conferir(rpcs3 / 'rpcs3.exe')
    conferir(ffmpeg / 'bin' / 'ffmpeg.exe')
    obra = raiz / 'build-setup'
    if obra.exists(): shutil.rmtree(obra)
    stage = obra / 'stage-setup'
    stage.mkdir(parents=True)
    compilar_cs(stage / 'Bleach Launcher.exe', ['Launcher.cs', 'Nucleo.cs', 'Visual.cs', 'Idiomas.cs', 'Traducao.cs'], [])
    stub = obra / 'Instalador-setup.exe'
    compilar_cs(stub, ['Instalador.cs', 'Compilacao.cs', 'Nucleo.cs', 'Visual.cs', 'Idiomas.cs'],
                ['/win32manifest:instalador.manifest', '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll'])
    copiar(aqui / 'configuracao.json', stage / 'configuracao.json')
    copiar(ffmpeg / 'bin' / 'ffmpeg.exe', stage / 'bin' / 'ffmpeg.exe')
    for f in rpcs3.iterdir():
        if f.is_file() and f.suffix.lower() in ('.exe', '.dll'): copiar(f, stage / 'rpcs3' / f.name)
    shutil.copytree(rpcs3 / 'qt6', stage / 'rpcs3' / 'qt6')
    for idioma in ('pt', 'fr', 'es'):
        f = raiz / 'traducao' / (idioma + '.json')
        if f.exists(): copiar(f, stage / 'traducao' / f.name)
    kit(stage / 'fonte')
    lic = stage / 'licencas'
    copiar(ffmpeg / 'LICENSE', lic / 'ffmpeg-LICENSE.txt')
    copiar(raiz / 'ps3recomp' / 'LICENSE', lic / 'ps3recomp-LICENSE.txt')
    (lic / 'LEIA-ME.txt').write_text(
        'bin\\ffmpeg.exe: FFmpeg (https://ffmpeg.org), "essentials" build from gyan.dev, GPL v3 (ffmpeg-LICENSE.txt).\r\n'
        'Source code: https://ffmpeg.org/download.html\r\n\r\n'
        'rpcs3\\: RPCS3 v0.0.30-16061-85f4c38b (https://rpcs3.net), GPL v2. Source code: https://github.com/RPCS3/rpcs3\r\n'
        'Used only to decrypt the EBOOT from your own dump and to read the official PS3 firmware.\r\n\r\n'
        'fonte\\ps3recomp\\: ps3recomp (https://github.com/sp00nznet/ps3recomp), MIT (ps3recomp-LICENSE.txt), with this\r\n'
        'project\'s changes. fonte\\bleach-recomp\\: this project\'s recomp code.\r\n\r\n'
        'Downloaded by the installer from the official sites (not included in the installer): Python (PSF License), Capstone (BSD),\r\n'
        'CMake (BSD), Ninja (Apache 2.0), LLVM-MinGW (Apache 2.0 with LLVM exception, mingw-w64: ZPL/its own licenses).\r\n'
        'The PS3 firmware is downloaded from Sony\'s official server or chosen by you.\r\n', encoding='utf-8')
    pacote = obra / 'stage-setup.zip'
    with zipfile.ZipFile(pacote, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for f in sorted(stage.rglob('*')):
            if f.is_file(): z.write(f, f.relative_to(stage).as_posix())
    saida.mkdir(parents=True, exist_ok=True)
    final = saida / NOME
    tamanho = pacote.stat().st_size
    with open(final, 'wb') as o:
        o.write(stub.read_bytes())
        with open(pacote, 'rb') as z: shutil.copyfileobj(z, o, 1 << 22)
        o.write(struct.pack('<q', tamanho)); o.write(b'BLEACHPK')
    print('%s  %.0f MB  sha256 %s' % (final, final.stat().st_size / 1e6, sha256(final)))


if __name__ == '__main__':
    a = argparse.ArgumentParser()
    a.add_argument('--rpcs3', required=True, type=Path)
    a.add_argument('--ffmpeg', required=True, type=Path)
    a.add_argument('--saida', default=raiz / 'dist', type=Path)
    o = a.parse_args()
    montar(o.rpcs3.resolve(), o.ffmpeg.resolve(), o.saida.resolve())
