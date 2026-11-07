"""Records the hand edits of recompiled/ and fonts/ as edicoes-recompilado.json, which construir.py
applies on the player's PC. Compares a fresh generation (construir.py --gerar-apenas --sem-edicoes)
with the edited sources (this folder). Only hashes of the replaced lines are stored, never the
generated code itself.

Usage: python gerar_edicoes.py <fresh generation dir> [<edited dir>] [-o edicoes-recompilado.json]"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path


def sha(b):
    return hashlib.sha256(b).hexdigest()


def canonico(b):
    """The sources as a UTF-8 generation writes them. Lines the lifter wrote in the Windows code page
    (cp1252: "—" in its comments) become UTF-8; hand-edited lines already in UTF-8 stay as they are."""
    linhas = []
    for l in b.split(b'\n'):
        try:
            l.decode('utf-8')
        except UnicodeDecodeError:
            l = l.decode('cp1252').encode('utf-8')
        linhas.append(l)
    return b'\n'.join(linhas)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('limpo', type=Path)
    p.add_argument('editado', type=Path, nargs='?', default=Path(__file__).resolve().parent)
    p.add_argument('-o', type=Path, default=Path(__file__).resolve().parent / 'edicoes-recompilado.json')
    a = p.parse_args()
    arquivos = {}
    editados = sorted(a.editado.glob('recompiled/*.*')) + sorted(a.editado.glob('fonts/*/ppu_recomp*.*'))
    for editado in editados:
        nome = editado.relative_to(a.editado).as_posix()
        x, y = (a.limpo / nome).read_bytes(), canonico(editado.read_bytes())
        if x == y:
            continue
        lx, ly = x.split(b'\n'), y.split(b'\n')
        trechos = []
        for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, lx, ly, autojunk=False).get_opcodes():
            if op == 'equal':
                continue
            trechos.append({'linha': i1 + 1, 'remover': i2 - i1, 'sha': sha(b'\n'.join(lx[i1:i2])),
                            'inserir': [l.decode('latin-1') for l in ly[j1:j2]]})
        arquivos[nome] = {'antes': sha(x), 'depois': sha(y), 'trechos': trechos}
    a.o.write_text(json.dumps({'arquivos': arquivos}, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
    print('%d arquivos, %d trechos -> %s' % (len(arquivos), sum(len(v['trechos']) for v in arquivos.values()), a.o))


if __name__ == '__main__':
    main()
