"""Builds the setup into outputs/Instaladores: "Bleach Soul Resurrección - Setup.exe".

It carries nothing derived from the game: no EBOOT, no recompiled code, no shaders, no firmware.
The player's PC rebuilds the game from their own dump (Instalador.cs, Compilacao.cs, construir.py).
Payload (zip appended to Instalador.exe):
  Bleach Launcher.exe, configuracao.json, traducao\ (translated strings +
  hashes of the originals), licencas\, rpcs3\ (GPL: decrypts the EBOOT, reads the firmware file),
  bin\ffmpeg.exe (GPL: the game's videos), fonte\ (ps3recomp SDK and the bleach-recomp host sources,
  with edicoes-recompilado.json: the hand edits as hashes of the replaced lines + new lines).
Before packing, the hand edits are recorded again from this machine's lift (gerar_edicoes.py) and
checked: a full generation from the local EBOOT must give exactly the sources in bleach-recomp.
Usage: python montar-instalador.py"""
import hashlib, os, shutil, struct, subprocess, sys, tempfile, zipfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bleach-recomp'))
from gerar_edicoes import canonico   # the UTF-8 form construir.py generates

aqui = Path(__file__).resolve().parent
work = aqui.parent
raiz = work.parent
saida = raiz / 'outputs' / 'Instaladores'
NOME = 'Bleach Soul Resurrección - Setup.exe'
ffmpeg = Path('C:/ffmpeg-2026-09-02-git-9fc8c785e2-essentials_build')
rpcs3 = Path.home() / 'Downloads' / 'rpcs3-v0.0.30-16061-85f4c38b_win64'
config = raiz / 'outputs' / 'BleachRecomp-Claude' / 'configuracao.json'
traducao = work / 'bleach-traducao'   # pt.json, fr.json, es.json (traduzir.py pacote --idioma X)
elf = work / 'bleach-analysis' / 'EBOOT.elf'           # this machine's own decrypted EBOOT (never packed)
firmware = work / 'bleach-analysis' / 'firmware'       # this machine's decrypted font PRXs (never packed)
csc = Path('C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe')
host = work / 'bleach-recomp'
# Host sources of bleach-recomp that the build needs (construir.py copies them next to the lift).
HOST = ['CMakeLists.txt', 'main.cpp', 'stubs.cpp', 'sail_bridge.cpp', 'generate_spu_variants.py',
        'generate_native_kernels.py', 'patch_game_timing.py', 'generate_fonts.py', 'spu-manifest.json',
        'font-manifest.json', 'construir.py', 'gerar_edicoes.py', 'edicoes-recompilado.json']
# Never packed from the SDK: work folders, caches, a third-party PDF.
FORA_SDK = {'.git', 'scratch', '__pycache__', 'build', 'out'}


def copiar(origem, destino):
    destino.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origem, destino)


def python_com_capstone():
    env = dict(os.environ)
    env['PYTHONPATH'] = str(work / 'python-tools')
    return env


def edicoes():
    """Records the hand edits from a fresh lift and checks that they rebuild bleach-recomp exactly."""
    env = python_com_capstone()
    with tempfile.TemporaryDirectory(prefix='bleach-edicoes-') as t:
        t = Path(t)
        base = [sys.executable, str(host / 'construir.py'), '--elf', str(elf), '--firmware', str(firmware),
                '--kit', str(work), '--gerar-apenas']
        subprocess.run(base + ['--trabalho', str(t / 'limpo'), '--sem-edicoes'], check=True, env=env, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, str(host / 'gerar_edicoes.py'), str(t / 'limpo')], check=True)
        subprocess.run(base + ['--trabalho', str(t / 'gerado')], check=True, env=env, stdout=subprocess.DEVNULL)
        for pasta in ('recompiled', 'spu', 'fonts'):
            for f in (host / pasta).rglob('*'):
                if f.is_file() and f.suffix in ('.c', '.cpp', '.h'):
                    g = t / 'gerado' / f.relative_to(host)
                    if not g.exists() or g.read_bytes() != canonico(f.read_bytes()):
                        raise SystemExit('A geração não reproduz %s; confira antes de empacotar.' % f.relative_to(host))
    print('edições conferidas: a geração reproduz recompiled/, spu/ e fonts/')


def compilar_cs(saida_exe, fontes, extras):
    subprocess.run([str(csc), '/nologo', '/target:winexe', '/codepage:65001', '/win32icon:icone-recomp.ico', '/define:ULTRAWIDE',
                    '/r:System.Windows.Forms.dll', '/r:System.Drawing.dll',
                    '/r:System.Web.Extensions.dll', '/lib:' + str(csc.parent / 'WPF'), '/r:UIAutomationClient.dll', '/r:UIAutomationTypes.dll'] + extras + ['/out:' + str(saida_exe)] + fontes,
                   check=True, cwd=aqui)


def kit(destino):
    sdk = work / 'ps3recomp'
    for f in sdk.rglob('*'):
        rel = f.relative_to(sdk)
        if not f.is_file() or set(rel.parts) & FORA_SDK or f.suffix.lower() == '.pdf':
            continue
        copiar(f, destino / 'ps3recomp' / rel)
    for nome in HOST:
        copiar(host / nome, destino / 'bleach-recomp' / nome)
    for h in host.glob('native_*.h'):
        copiar(h, destino / 'bleach-recomp' / h.name)


def montar():
    edicoes()
    stage = work / 'bleach-install' / 'stage-setup'
    if stage.exists(): shutil.rmtree(stage)
    stage.mkdir(parents=True)
    compilar_cs(stage / 'Bleach Launcher.exe', ['Launcher.cs', 'Nucleo.cs', 'Visual.cs', 'Idiomas.cs', 'Traducao.cs'], [])
    stub = work / 'bleach-install' / 'Instalador-setup.exe'
    compilar_cs(stub, ['Instalador.cs', 'Compilacao.cs', 'Nucleo.cs', 'Visual.cs', 'Idiomas.cs'],
                ['/win32manifest:instalador.manifest', '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll'])
    copiar(config, stage / 'configuracao.json')
    copiar(ffmpeg / 'bin' / 'ffmpeg.exe', stage / 'bin' / 'ffmpeg.exe')
    # RPCS3 program files only (no configuration, firmware, games or caches of this machine).
    for f in rpcs3.iterdir():
        if f.is_file() and f.suffix.lower() in ('.exe', '.dll'): copiar(f, stage / 'rpcs3' / f.name)
    shutil.copytree(rpcs3 / 'qt6', stage / 'rpcs3' / 'qt6')
    # Game text translations: translated strings + hashes of the originals only (traduzir.py pacote);
    # the launcher rebuilds the game files from the player's own dump.
    for idioma in ('pt', 'fr', 'es'):
        if (traducao / (idioma + '.json')).exists():
            copiar(traducao / (idioma + '.json'), stage / 'traducao' / (idioma + '.json'))
    kit(stage / 'fonte')
    lic = stage / 'licencas'
    copiar(ffmpeg / 'LICENSE', lic / 'ffmpeg-LICENSE.txt')
    copiar(work / 'ps3recomp' / 'LICENSE', lic / 'ps3recomp-LICENSE.txt')
    (lic / 'LEIA-ME.txt').write_text(
        'bin\\ffmpeg.exe: FFmpeg (https://ffmpeg.org), build "essentials" de gyan.dev, GPL v3 (ffmpeg-LICENSE.txt).\r\n'
        'Código-fonte: https://ffmpeg.org/download.html\r\n\r\n'
        'rpcs3\\: RPCS3 v0.0.30-16061-85f4c38b (https://rpcs3.net), GPL v2. Código-fonte: https://github.com/RPCS3/rpcs3\r\n'
        'Usado só para decifrar o EBOOT do seu próprio dump e ler o firmware oficial do PS3.\r\n\r\n'
        'fonte\\ps3recomp\\: ps3recomp (https://github.com/sp00nznet/ps3recomp), MIT (ps3recomp-LICENSE.txt), com as\r\n'
        'alterações deste projeto. fonte\\bleach-recomp\\: o código do recomp deste projeto.\r\n\r\n'
        'Baixados pelo instalador dos sites oficiais (não vêm no instalador): Python (PSF License), Capstone (BSD),\r\n'
        'CMake (BSD), Ninja (Apache 2.0), LLVM-MinGW (Apache 2.0 com exceção LLVM, mingw-w64: ZPL/licenças próprias).\r\n'
        'O firmware do PS3 é baixado do servidor oficial da Sony ou escolhido por você.\r\n', encoding='utf-8')
    pacote = stage.with_suffix('.zip')
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
    pacote.unlink()
    print('%s  %.0f MB' % (final, final.stat().st_size / 1e6))


montar()
