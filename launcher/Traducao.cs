// Game text translations (launcher only). The install folder carries traducao\<pt|fr|es>.json: per .spk,
// [id, hash of the original string, translated text], never the game's own text or files. Before
// the game starts, the .spk files it touches are rebuilt from the player's own dump into the data
// folder, and the recomp serves them in place of the disc's (PS3_VFS_OVERLAY). Same layout as
// work/claude-tools/traducao/spk.py, where the formats are documented.
using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

static class Traducao
{
    // The game's text languages besides English (the disc's own).
    public static readonly string[] Idiomas = { "pt", "fr", "es" };
    static string Pacote(string idioma) { return Path.Combine(Instalacao.Pasta, "traducao", idioma + ".json"); }
    public static bool Disponivel(string idioma) { return Array.IndexOf(Idiomas, idioma) >= 0 && File.Exists(Pacote(idioma)); }
    public static bool AlgumDisponivel { get { foreach (var i in Idiomas) if (Disponivel(i)) return true; return false; } }
    static string Overlay(string idioma) { return Path.Combine(Instalacao.Dados, "traducao", idioma); }

    // The overlay root for the game (built or reused), or null to run in English. Never throws:
    // a problem leaves the game untranslated and is written to traducao-erro.txt.
    public static string Preparar(string raiz, string texto)
    {
        if (!Disponivel(texto)) return null;
        string overlay = Overlay(texto), arqEstado = Path.Combine(overlay, "estado.txt");
        try
        {
            string usrdir = Path.Combine(raiz, "PS3_GAME", "USRDIR");
            byte[] bruto = File.ReadAllBytes(Pacote(texto));
            var pacote = (Dictionary<string, object>)new JavaScriptSerializer { MaxJsonLength = int.MaxValue }
                .DeserializeObject(Encoding.UTF8.GetString(bruto));
            var arquivos = (Dictionary<string, object>)pacote["arquivos"];
            // Rebuilt only when the package or one of the source files changes.
            var estado = new StringBuilder(Sha256(bruto));
            foreach (var nome in arquivos.Keys)
            {
                var info = new FileInfo(Path.Combine(usrdir, nome.Replace('/', '\\')));
                estado.Append('|').Append(nome).Append(':').Append(info.Exists ? info.Length + ":" + info.LastWriteTimeUtc.Ticks : "-");
            }
            if (File.Exists(arqEstado) && File.ReadAllText(arqEstado) == estado.ToString()) return overlay;

            if (Directory.Exists(overlay)) Directory.Delete(overlay, true);
            var tipos = Fats(usrdir);
            var avisos = new List<string>();
            foreach (var kv in arquivos)
            {
                List<uint> contagem;
                string origem = Path.Combine(usrdir, kv.Key.Replace('/', '\\'));
                if (!tipos.TryGetValue(kv.Key, out contagem) || !File.Exists(origem)) { avisos.Add(kv.Key + ": não está no dump"); continue; }
                byte[] novo = Montar(File.ReadAllBytes(origem), contagem.Count, (object[])kv.Value, kv.Key, avisos);
                string destino = Path.Combine(overlay, "PS3_GAME", "USRDIR", kv.Key.Replace('/', '\\'));
                Directory.CreateDirectory(Path.GetDirectoryName(destino));
                File.WriteAllBytes(destino, novo);
            }
            File.WriteAllText(arqEstado, estado.ToString());
            string log = Path.Combine(Instalacao.Dados, "traducao-erro.txt");
            if (avisos.Count > 0) File.WriteAllLines(log, avisos.ToArray(), Encoding.UTF8);
            else if (File.Exists(log)) File.Delete(log);
            return overlay;
        }
        catch (Exception ex)
        {
            try { File.WriteAllText(Path.Combine(Instalacao.Dados, "traducao-erro.txt"), DateTime.Now + "\r\n" + ex, Encoding.UTF8); } catch { }
            return null;
        }
    }

    static uint Be32(byte[] b, int o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }
    static void Poe32(List<byte> b, uint v) { b.Add((byte)(v >> 24)); b.Add((byte)(v >> 16)); b.Add((byte)(v >> 8)); b.Add((byte)v); }
    static void Poe32(byte[] b, int o, uint v) { b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v; }
    static string Sha256(byte[] b) { using (var s = SHA256.Create()) return BitConverter.ToString(s.ComputeHash(b)).Replace("-", "").ToLowerInvariant(); }

    // spk name ("spk/drawenglish/x.spk") -> resource types, from every RESFAT*.BIN.
    static Dictionary<string, List<uint>> Fats(string usrdir)
    {
        var r = new Dictionary<string, List<uint>>();
        foreach (var f in Directory.GetFiles(usrdir, "RESFAT*.BIN"))
        {
            byte[] b = File.ReadAllBytes(f);
            int tab = (int)Be32(b, 4), dados = (int)Be32(b, 8), n = (int)Be32(b, 12);
            for (int i = 0; i < n; i++)
            {
                int lista = (int)Be32(b, tab + 16 * i), cnt = (int)Be32(b, tab + 16 * i + 4);
                int nomeOff = dados + (int)Be32(b, dados + 0x18 + 8 * i) + (int)Be32(b, dados + 0x18 + 8 * i + 4);
                int fim = nomeOff; while (b[fim] != 0) fim++;
                string nome = Encoding.ASCII.GetString(b, nomeOff, fim - nomeOff).TrimStart('@').Replace('\\', '/').ToLowerInvariant();
                var tipos = new List<uint>();
                for (int j = 0; j < cnt; j++) tipos.Add(Be32(b, (int)Be32(b, lista + 16 * j + 8) + 24));
                r[nome] = tipos;
            }
        }
        return r;
    }

    // Rebuilds one .spk with the translated strings of its text tables.
    static byte[] Montar(byte[] d, int count, object[] entradas, string nome, List<string> avisos)
    {
        int cab = (count * 4 + 15) & ~15;
        var recursos = new List<byte[]>();   // header (32 bytes) + payload, each
        for (int k = 0; k < count; k++)
        {
            int a = cab + (int)Be32(d, 4 * k), tam = (int)Be32(d, a + 20);
            var r = new byte[32 + tam];
            Array.Copy(d, a, r, 0, r.Length);
            recursos.Add(r);
        }
        var tabelas = new Dictionary<int, List<uint[]>>();
        foreach (object[] e in entradas)
        {
            string[] id = ((string)e[0]).Split('.');
            int k = int.Parse(id[0]), i = int.Parse(id[1]);
            List<uint[]> strs;
            if (!tabelas.TryGetValue(k, out strs))
            {
                strs = k < count ? LerTabela(recursos[k]) : null;
                tabelas[k] = strs;
            }
            if (strs == null || i >= strs.Count || Hash(strs[i]) != (string)e[1])
            { avisos.Add(nome + " " + e[0] + ": não confere com o dump, mantido o original"); continue; }
            strs[i] = Codificar((string)e[2]);
        }
        foreach (var kv in tabelas)
            if (kv.Value != null) recursos[kv.Key] = Recurso(recursos[kv.Key], kv.Value);

        var saida = new List<byte>(d.Length + 4096);
        var offs = new List<uint>();
        int corpo = 0;
        foreach (var r in recursos) { offs.Add((uint)corpo); corpo += (r.Length + 15) & ~15; }
        foreach (var o in offs) Poe32(saida, o);
        while (saida.Count < cab) saida.Add(0);
        foreach (var r in recursos) { saida.AddRange(r); while ((saida.Count - cab) % 16 != 0) saida.Add(0); }
        return saida.ToArray();
    }

    // Strings of a text table that fills the whole resource, or null.
    static List<uint[]> LerTabela(byte[] r)
    {
        int tam = r.Length - 32;
        if (tam < 8) return null;
        int n = (int)Be32(r, 32);
        if (n <= 0 || n > 20000 || Be32(r, 36) != 4 + 4 * n || 4 + 4 * n > tam) return null;
        var strs = new List<uint[]>();
        int pos = 4 + 4 * n;
        for (int i = 0; i < n; i++)
        {
            if (Be32(r, 36 + 4 * i) != pos) return null;
            var cps = new List<uint>();
            for (;;)
            {
                if (pos + 4 > tam) return null;
                uint c = Be32(r, 32 + pos); pos += 4;
                if (c == 0) break;
                cps.Add(c);
            }
            strs.Add(cps.ToArray());
        }
        return tam - pos < 16 ? strs : null;
    }

    static byte[] Recurso(byte[] antigo, List<uint[]> strs)
    {
        var t = new List<byte>();
        int n = strs.Count, pos = 4 + 4 * n;
        Poe32(t, (uint)n);
        foreach (var s in strs) { Poe32(t, (uint)pos); pos += 4 * (s.Length + 1); }
        foreach (var s in strs) { foreach (var c in s) Poe32(t, c); Poe32(t, 0); }
        while (t.Count % 16 != 0) t.Add(0);   // text resources are sized to 16 bytes
        var r = new byte[32 + t.Count];
        Array.Copy(antigo, r, 32);
        Poe32(r, 20, (uint)t.Count);
        t.CopyTo(r, 32);
        return r;
    }

    static string Hash(uint[] cps)
    {
        var b = new byte[cps.Length * 4];
        for (int i = 0; i < cps.Length; i++) Poe32(b, 4 * i, cps[i]);
        return Sha256(b).Substring(0, 12);
    }

    // "{8000101a}" tags are the game's control codes; everything else is UTF-16 text.
    static uint[] Codificar(string s)
    {
        var cps = new List<uint>();
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] == '{')
            {
                int j = s.IndexOf('}', i);
                cps.Add(Convert.ToUInt32(s.Substring(i + 1, j - i - 1), 16)); i = j;
            }
            else if (char.IsHighSurrogate(s[i]) && i + 1 < s.Length) { cps.Add((uint)char.ConvertToUtf32(s[i], s[i + 1])); i++; }
            else cps.Add(s[i]);
        }
        return cps.ToArray();
    }
}
