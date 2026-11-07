"""Builds BleachRecomp.exe on the player's PC from their own decrypted EBOOT.elf and the four
font/display PRXs of the official PS3 firmware. The installer runs it with the tools it
downloaded; nothing derived from the game is shipped, everything game-derived is generated here.

Steps: lift the PPU code, apply the timing patch and the recorded hand edits (edicoes-recompilado.json:
hashes of the lines they replace and the new lines, never the game's own code), lift the SPU programs
and the firmware font modules, configure with CMake + Ninja (llvm-mingw clang) and compile.

Progress on stdout for the installer: "@@etapa <n> <total> <name>" and "@@compilando <done> <total>".

Usage: python construir.py --elf EBOOT.elf --firmware <dir with libfont.prx ...> --kit <dir with
ps3recomp\\ and bleach-recomp\\> --toolchain <llvm-mingw\\bin> --cmake cmake.exe --ninja ninja.exe
--trabalho <work dir> --saida <bin dir> [--jobs N] [--gerar-apenas] [--sem-edicoes]"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from types import SimpleNamespace

ELF_HASH = '49a9b10868fb9d0b954c1734708345a4669acaafb0402fdc6f02ac5c3360193f'
ETAPAS = ['ppu', 'edicoes', 'spu', 'fontes', 'configurar', 'compilar', 'finalizar']
# Host sources copied next to the generated code (CMakeLists.txt expects them in one folder).
FONTES_HOST = ('CMakeLists.txt', 'main.cpp', 'stubs.cpp', 'sail_bridge.cpp', 'generate_spu_variants.py',
               'generate_native_kernels.py', 'patch_game_timing.py', 'generate_fonts.py',
               'spu-manifest.json', 'font-manifest.json')


def etapa(nome):
    print('@@etapa %d %d %s' % (ETAPAS.index(nome) + 1, len(ETAPAS), nome), flush=True)


def rodar(*cmd, **kw):
    subprocess.run([str(x) for x in cmd], check=True, **kw)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def aplicar_edicoes(raiz, arquivo, prefixo):
    """Applies the edicoes-recompilado.json entries under prefixo (recompiled/ or fonts/) to the freshly
    generated sources. Every file must hash to the generation this was recorded from before, and to
    the tested sources after."""
    edicoes = json.loads(arquivo.read_text(encoding='utf-8'))
    for nome, e in edicoes['arquivos'].items():
        if not nome.startswith(prefixo):
            continue
        caminho = raiz / nome
        dados = caminho.read_bytes()
        if sha(dados) != e['antes']:
            raise SystemExit('Lift diferente do esperado em %s; não dá para aplicar as edições.' % nome)
        linhas = dados.split(b'\n')
        # Bottom-up, so earlier line numbers stay valid.
        for h in sorted(e['trechos'], key=lambda h: h['linha'], reverse=True):
            i, n = h['linha'] - 1, h['remover']
            if sha(b'\n'.join(linhas[i:i + n])) != h['sha']:
                raise SystemExit('Trecho inesperado em %s, linha %d.' % (nome, h['linha']))
            linhas[i:i + n] = [l.encode('latin-1') for l in h['inserir']]
        novo = b'\n'.join(linhas)
        if sha(novo) != e['depois']:
            raise SystemExit('Resultado das edições diferente do testado em %s.' % nome)
        caminho.write_bytes(novo)


def main():
    # The lifters write their text with Python's default encoding (the Windows code page): run in UTF-8
    # mode so every PC generates the same bytes, whatever its language.
    if not sys.flags.utf8_mode:
        raise SystemExit(subprocess.run([sys.executable, '-X', 'utf8'] + sys.argv).returncode)
    os.environ['PYTHONUTF8'] = '1'
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--firmware', type=Path, required=True)
    p.add_argument('--kit', type=Path, required=True)
    p.add_argument('--toolchain', type=Path)
    p.add_argument('--cmake', type=Path)
    p.add_argument('--ninja', type=Path)
    p.add_argument('--trabalho', type=Path, required=True)
    p.add_argument('--saida', type=Path)
    p.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 1))
    p.add_argument('--gerar-apenas', action='store_true', help='stop after generating the sources')
    p.add_argument('--sem-edicoes', action='store_true', help='skip the hand edits (for gerar_edicoes.py)')
    a = p.parse_args()

    elf = a.elf.resolve()
    payload = elf.read_bytes()
    if sha(payload) != ELF_HASH:
        raise SystemExit('Este EBOOT não é o do BLUS30769 usado por esta recompilação.')
    kit = a.kit.resolve()
    sdk = kit / 'ps3recomp'
    host = kit / 'bleach-recomp'
    firmware = a.firmware.resolve()
    for item in json.loads((host / 'font-manifest.json').read_text()):
        if sha((firmware / (item['name'] + '.prx')).read_bytes()) != item['prx_sha256']:
            raise SystemExit('Módulo do firmware diferente do esperado: ' + item['name'])

    raiz = a.trabalho.resolve()
    if raiz.exists():
        shutil.rmtree(raiz)
    (raiz / 'analysis').mkdir(parents=True)
    for nome in FONTES_HOST:
        shutil.copy2(host / nome, raiz / nome)
    for h in host.glob('native_*.h'):
        shutil.copy2(h, raiz / h.name)
    tools = sdk / 'tools'
    py = sys.executable

    etapa('ppu')
    analise = raiz / 'analysis' / 'ppu'
    rodar(py, tools / 'ppu_loader.py', elf, '-o', analise)
    rodar(py, tools / 'ppu_lifter.py', elf, '--functions', analise / (elf.stem + '.functions.json'),
          '--hle-stubs', analise / (elf.stem + '.imports.json'), '--code-end', '0x3DB1C8',
          '--chunks', '64', '--jobs', '1', '--output', raiz / 'recompiled')

    etapa('edicoes')
    rodar(py, raiz / 'patch_game_timing.py', raiz / 'recompiled')
    if not a.sem_edicoes:
        aplicar_edicoes(raiz, host / 'edicoes-recompilado.json', 'recompiled/')

    etapa('spu')
    sys.path.insert(0, str(tools))
    from wrap_spu_elf import wrap
    from build_spu_workloads import debase_includes, fnv1a64, write_registry
    imagens = []
    manifesto = json.loads((host / 'spu-manifest.json').read_text())
    for item in manifesto:
        cru = payload[item['elf_offset']:item['elf_offset'] + item['size']]
        if sha(cru) != item['sha256']:
            raise SystemExit('Programa SPU diferente do esperado: ' + item['name'])
        alvo = raiz / 'spu' / item['name']
        alvo.mkdir(parents=True, exist_ok=True)
        envolto = raiz / 'analysis' / (item['name'] + '.elf')
        envolto.write_bytes(wrap(cru, base=item['ls_base'], entry=item['entry']))
        prefixo = item['name'] + '_'
        rodar(py, tools / 'spu_lifter.py', '--auto-functions', envolto, '--symbol-prefix', prefixo,
              '--output', alvo, '--merge-chunks', '--report', alvo / 'report.json', stdout=subprocess.DEVNULL)
        debase_includes(str(alvo / 'spu_recomp.c'))
        debase_includes(str(alvo / 'spu_recomp.h'))
        imagens.append((item['name'], prefixo, fnv1a64(cru), item['entry']))
    write_registry(SimpleNamespace(out=str(raiz / 'spu' / 'spu_workloads.c'), title='Bleach BLUS30769 SPU jobs',
                                   register_fn='bleach_spu_register', constructor=True),
                   imagens, {}, image_ids={item[0]: 1001 + i for i, item in enumerate(imagens)})

    etapa('fontes')
    sys.path.insert(0, str(raiz))
    from generate_fonts import generate
    generate(sdk, firmware, raiz / 'fonts')
    if not a.sem_edicoes:
        aplicar_edicoes(raiz, host / 'edicoes-recompilado.json', 'fonts/')
    if a.gerar_apenas:
        return

    etapa('configurar')
    build = raiz / 'build'
    ambiente = dict(os.environ)
    ambiente['PATH'] = str(a.toolchain.resolve()) + os.pathsep + ambiente.get('PATH', '')
    cc = a.toolchain.resolve()
    rodar(a.cmake, '-S', raiz, '-B', build, '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(a.ninja.resolve()),
          '-DPS3RECOMP_DIR=' + str(sdk), '-DCMAKE_C_COMPILER=' + str(cc / 'clang.exe'),
          '-DCMAKE_CXX_COMPILER=' + str(cc / 'clang++.exe'), '-DPython3_EXECUTABLE=' + py,
          '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG',
          '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG', env=ambiente, stdout=subprocess.DEVNULL)

    etapa('compilar')
    proc = subprocess.Popen([str(a.cmake), '--build', str(build), '--parallel', str(a.jobs)], env=ambiente,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    contador = re.compile(r'^\[(\d+)/(\d+)\]')
    ultimas = []
    for linha in proc.stdout:
        m = contador.match(linha)
        if m:
            print('@@compilando %s %s' % m.groups(), flush=True)
        else:
            ultimas = (ultimas + [linha.rstrip()])[-60:]
    if proc.wait() != 0:
        print('\n'.join(ultimas))
        raise SystemExit('A compilação falhou.')

    etapa('finalizar')
    saida = a.saida.resolve()
    saida.mkdir(parents=True, exist_ok=True)
    rodar(cc / 'llvm-strip.exe', '--strip-debug', '-o', saida / 'BleachRecomp.exe', build / 'BleachRecomp.exe')
    for dll in ('libc++.dll', 'libunwind.dll'):
        shutil.copy2(cc.parent / 'x86_64-w64-mingw32' / 'bin' / dll, saida / dll)
    print('@@pronto', flush=True)


if __name__ == '__main__':
    main()
