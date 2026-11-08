// Shared by the launcher and the installer: install/data locations, the saved options, the
// game environment and its start, the user's dump (ISO or folder, EBOOT decryption), the PS3
// firmware files and shortcuts. C# 5 (.NET Framework csc).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Automation;

static class Instalacao
{
    public const string Titulo = "Bleach: Soul Resurrección";
    public const string NomeAtalho = "Bleach Soul Resurrección";
    public const string Id = "BleachSoulResurreccion";   // ASCII: data folder, registry key
    public const string Versao = "1.0";

    // The launcher's folder: bin\, configuracao.json, rpcs3\, firmware\ (personal build).
    public static string Pasta = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
    // Everything that is written: options, decrypted EBOOT, saves, extracted firmware, logs.
    // Program Files is read-only for the player.
    public static string Dados = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), Id);

    public static string ArquivoCfg { get { return Path.Combine(Dados, "launcher.cfg"); } }
    public static string Elf { get { return Path.Combine(Dados, "EBOOT.elf"); } }
    public static string Logs { get { return Path.Combine(Dados, "logs"); } }
    public static string Jogo { get { return Path.Combine(Pasta, "bin", "BleachRecomp.exe"); } }
    public static string Launcher { get { return Path.Combine(Pasta, "Bleach Launcher.exe"); } }
    public static string Icone { get { return Path.Combine(Pasta, "Bleach Launcher.exe"); } }
}

// launcher.cfg: key=value lines (jogo, resolucao, fps, tela).
class Opcoes
{
    readonly Dictionary<string, string> v = new Dictionary<string, string>();
    public string this[string chave]
    {
        get { string s; return v.TryGetValue(chave, out s) ? s : null; }
        set { if (value == null) v.Remove(chave); else v[chave] = value; }
    }
    public string Ou(string chave, string padrao) { return this[chave] ?? padrao; }

    public static Opcoes Ler()
    {
        var o = new Opcoes();
        string arquivo = Instalacao.ArquivoCfg;
        // A package from before the data folder kept launcher.cfg next to the launcher.
        if (!File.Exists(arquivo) && File.Exists(Path.Combine(Instalacao.Pasta, "launcher.cfg")))
            arquivo = Path.Combine(Instalacao.Pasta, "launcher.cfg");
        try
        {
            foreach (var linha in File.ReadAllLines(arquivo, Encoding.UTF8))
            { int i = linha.IndexOf('='); if (i > 0) o.v[linha.Substring(0, i).Trim()] = linha.Substring(i + 1).Trim(); }
        }
        catch { }
        return o;
    }
    public void Salvar()
    {
        var linhas = new List<string>();
        foreach (var kv in v) linhas.Add(kv.Key + "=" + kv.Value);
        try { Directory.CreateDirectory(Instalacao.Dados); File.WriteAllLines(Instalacao.ArquivoCfg, linhas.ToArray(), Encoding.UTF8); }
        catch { }
    }
}

static class Util
{
    public static string Sha256(string arquivo)
    {
        using (var sha = SHA256.Create()) using (var s = File.OpenRead(arquivo))
            return BitConverter.ToString(sha.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern uint GetShortPathNameW(string longo, StringBuilder curto, uint tamanho);
    // The recomp opens paths with the ANSI code page: a folder with characters outside it (a user
    // name in another script) is handed over by its 8.3 short name instead.
    public static string ParaJogo(string caminho)
    {
        bool ascii = true;
        foreach (char c in caminho) if (c > 126) { ascii = false; break; }
        if (ascii) return caminho;
        var sb = new StringBuilder(1024);
        return GetShortPathNameW(caminho, sb, (uint)sb.Capacity) > 0 ? sb.ToString() : caminho;
    }

    // Runs a PowerShell snippet (encoded, so paths need no quoting) and returns its last output line.
    public static string PowerShell(string script, int msLimite)
    {
        var psi = new ProcessStartInfo("powershell.exe", "-NoProfile -NonInteractive -EncodedCommand " +
            Convert.ToBase64String(Encoding.Unicode.GetBytes("$ProgressPreference = 'SilentlyContinue'; " + script)))
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        using (var p = Process.Start(psi))
        {
            string saida = p.StandardOutput.ReadToEnd();
            p.WaitForExit(msLimite);
            var linhas = saida.Trim().Split('\n');
            return linhas.Length > 0 ? linhas[linhas.Length - 1].Trim() : "";
        }
    }

    public static void CopiarPasta(string origem, string destino)
    {
        Directory.CreateDirectory(destino);
        foreach (var f in Directory.GetFiles(origem)) File.Copy(f, Path.Combine(destino, Path.GetFileName(f)), true);
        foreach (var d in Directory.GetDirectories(origem)) CopiarPasta(d, Path.Combine(destino, Path.GetFileName(d)));
    }

    // RPCS3 (GPL, shipped in rpcs3\) decrypts the EBOOT and installs the firmware. It writes its
    // config and logs next to itself, so it runs from a copy in the data folder.
    public static string Rpcs3()
    {
        string origem = Path.Combine(Instalacao.Pasta, "rpcs3");
        string destino = Path.Combine(Instalacao.Dados, "rpcs3");
        string exe = Path.Combine(destino, "rpcs3.exe");
        if (!File.Exists(Path.Combine(origem, "rpcs3.exe"))) return File.Exists(exe) ? exe : null;
        if (!File.Exists(exe) || File.GetLastWriteTimeUtc(exe) != File.GetLastWriteTimeUtc(Path.Combine(origem, "rpcs3.exe")))
            CopiarPasta(origem, destino);
        // A fresh RPCS3 opens its welcome screen first and waits there for a click.
        string gui = Path.Combine(destino, "GuiConfigs", "CurrentSettings.ini");
        if (!File.Exists(gui))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(gui));
            File.WriteAllText(gui, "[main_window]\r\ninfoBoxEnabledWelcome=false\r\n");
        }
        return exe;
    }
    static ProcessStartInfo Rpcs3Info(string rpcs3, string argumentos)
    {
        // --allow-any-location: RPCS3 otherwise refuses to start from some folders (a temp one).
        return new ProcessStartInfo(rpcs3, "--allow-any-location " + argumentos)
            { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.GetDirectoryName(rpcs3) };
    }
    public static int RodarRpcs3(string rpcs3, string argumentos, int msLimite)
    {
        var psi = Rpcs3Info(rpcs3, argumentos);
        using (var p = Process.Start(psi))
        {
            if (!p.WaitForExit(msLimite)) { try { p.Kill(); } catch { } return -1; }
            return p.ExitCode;
        }
    }
    // RPCS3 asks "Install firmware: <file>?" even when started with --installfw; the player already chose
    // to install it, so its Yes button is pressed here (UI Automation: the dialog's default is No).
    static bool ConfirmarFirmware(int pid)
    {
        try
        {
            var doProcesso = new PropertyCondition(AutomationElement.ProcessIdProperty, pid);
            var botao = new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button);
            foreach (AutomationElement j in AutomationElement.RootElement.FindAll(TreeScope.Children, doProcesso))
            {
                if (j.Current.Name != "RPCS3 Firmware Installer") continue;
                foreach (AutomationElement b in j.FindAll(TreeScope.Descendants, botao))
                    if (b.Current.Name.Replace("&", "") == "Yes")
                    { ((InvokePattern)b.GetCurrentPattern(InvokePattern.Pattern)).Invoke(); return true; }
            }
        }
        catch { }
        return false;
    }
    // "--installfw" leaves RPCS3's window open once the firmware is in (and then starts compiling
    // the system modules): done = version.txt present and the dev_flash file count steady for 5 s.
    public static bool InstalarFirmwareRpcs3(string rpcs3, string pup, int msLimite)
    {
        string devFlash = Path.Combine(Path.GetDirectoryName(rpcs3), "dev_flash");
        string versao = Path.Combine(devFlash, "vsh", "etc", "version.txt");
        using (var p = Process.Start(Rpcs3Info(rpcs3, "--installfw \"" + pup + "\"")))
        {
            var relogio = Stopwatch.StartNew(); int ultimo = -1, estavel = 0; bool pronto = false;
            while (relogio.ElapsedMilliseconds < msLimite)
            {
                if (p.WaitForExit(1000)) { pronto = File.Exists(versao); break; }
                if (!File.Exists(versao)) { ConfirmarFirmware(p.Id); continue; }
                int n; try { n = Directory.GetFiles(devFlash, "*", SearchOption.AllDirectories).Length; } catch { continue; }
                estavel = n == ultimo ? estavel + 1 : 0; ultimo = n;
                if (estavel >= 5) { pronto = true; break; }
            }
            if (!p.HasExited) { try { p.Kill(); p.WaitForExit(10000); } catch { } }
            return pronto;
        }
    }
}

// The user's dump: an .iso (mounted by Windows while the game runs) or a folder with PS3_GAME.
class Dump
{
    // Only this lift's EBOOT works: BLUS30769, the version the recompiled code was produced from.
    public const string ShaEbootBin = "bad22f5ef94f516235240efba9d8ba39ac5b0f025d734fe807dc973ccaec67fd";
    public const string ShaEbootElf = "49a9b10868fb9d0b954c1734708345a4669acaafb0402fdc6f02ac5c3360193f";

    public readonly string Origem;   // as chosen: the .iso or the dump root
    bool montadaPorNos;
    public Dump(string origem) { Origem = origem; }
    public bool Iso { get { return EhIso(Origem); } }
    public static bool EhIso(string c) { return c != null && c.EndsWith(".iso", StringComparison.OrdinalIgnoreCase); }

    // Accepts the dump root (with PS3_GAME), PS3_GAME itself or USRDIR.
    public static string AcharRaiz(string escolhida)
    {
        if (string.IsNullOrEmpty(escolhida)) return null;
        try
        {
            var d = new DirectoryInfo(escolhida);
            for (int i = 0; i < 3 && d != null; i++, d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "PS3_GAME", "USRDIR", "EBOOT.BIN"))) return d.FullName;
        }
        catch (Exception) { }
        return null;
    }

    // Where a mounted ISO is reached: a junction to its volume. The ISO is mounted without a drive
    // letter, so Windows announces no new disc (no AutoPlay, no Explorer window), and the game sees
    // an ordinary folder.
    static string Juncao { get { return Path.Combine(Instalacao.Dados, "iso"); } }
    static void ApagarJuncao()
    {
        // Non-recursive: removes the link only, never what it points to.
        try { if (Directory.Exists(Juncao)) Directory.Delete(Juncao, false); } catch { }
    }

    // The dump root, mounting the ISO when needed, or null.
    public string Abrir()
    {
        if (!Iso) return AcharRaiz(Origem);
        if (!File.Exists(Origem)) return null;
        string q = Origem.Replace("'", "''");
        string r = Util.PowerShell("$i = Get-DiskImage -ImagePath '" + q + "'; $m = 0; " +
            "if (-not $i.Attached) { $i = Mount-DiskImage -ImagePath '" + q + "' -StorageType ISO -NoDriveLetter -PassThru; $m = 1 }; " +
            "$v = $null; for ($t = 0; $t -lt 50 -and -not ($v -and $v.Path); $t++) { $v = $i | Get-Volume; if (-not ($v -and $v.Path)) { Start-Sleep -Milliseconds 100 } }; " +
            "if ($v.DriveLetter) { Write-Output (\"$m|\" + $v.DriveLetter + ':\\') } else { Write-Output (\"$m|\" + $v.Path) }", 30000);
        int barra = r.IndexOf('|');
        if (barra != 1) return null;
        montadaPorNos = r[0] == '1';
        string volume = r.Substring(2);
        if (volume.Length == 3 && volume[1] == ':') return volume;   // the user had it mounted with a letter
        if (!volume.StartsWith(@"\\?\Volume")) { Fechar(); return null; }
        ApagarJuncao();
        Directory.CreateDirectory(Instalacao.Dados);
        var psi = new ProcessStartInfo("cmd.exe", "/c mklink /J \"" + Juncao + "\" \"" + volume + "\"")
            { UseShellExecute = false, CreateNoWindow = true };
        using (var p = Process.Start(psi)) p.WaitForExit(15000);
        if (!File.Exists(Path.Combine(Juncao, "PS3_GAME", "PARAM.SFO"))) { Fechar(); return null; }
        return Juncao + "\\";
    }
    public void Fechar()
    {
        if (!Iso || !montadaPorNos) return;   // the user had it mounted already
        ApagarJuncao();
        Util.PowerShell("Dismount-DiskImage -ImagePath '" + Origem.Replace("'", "''") + "' | Out-Null", 30000);
        montadaPorNos = false;
    }

    public static bool EhBleach(string raiz)
    {
        string sfo = Path.Combine(raiz, "PS3_GAME", "PARAM.SFO");
        return File.Exists(sfo) && Encoding.ASCII.GetString(File.ReadAllBytes(sfo)).Contains("BLUS30769");
    }
    public static bool ElfPronto() { return File.Exists(Instalacao.Elf) && Util.Sha256(Instalacao.Elf) == ShaEbootElf; }

    // Decrypts the dump's EBOOT.BIN once into the data folder. Returns null or an error message.
    public static string Decifrar(string raiz)
    {
        string bin = Path.Combine(raiz, "PS3_GAME", "USRDIR", "EBOOT.BIN");
        if (Util.Sha256(bin) != ShaEbootBin)
            return L.T("eboot_versao");
        // A package from before the data folder kept it in dados\.
        string antigo = Path.Combine(Instalacao.Pasta, "dados", "EBOOT.elf");
        Directory.CreateDirectory(Instalacao.Dados);
        if (File.Exists(antigo) && Util.Sha256(antigo) == ShaEbootElf) { File.Copy(antigo, Instalacao.Elf, true); return null; }
        string rpcs3 = Util.Rpcs3();
        if (rpcs3 == null) return L.T("sem_rpcs3");
        string temp = Path.Combine(Path.GetTempPath(), "bleach-decrypt-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(temp);
            string copia = Path.Combine(temp, "EBOOT.BIN");
            File.Copy(bin, copia);
            Util.RodarRpcs3(rpcs3, "--decrypt \"" + copia + "\"", 180000);
            string saida = Path.Combine(temp, "EBOOT.elf");
            if (!File.Exists(saida) || Util.Sha256(saida) != ShaEbootElf) return L.T("eboot_falhou");
            File.Copy(saida, Instalacao.Elf, true);
            return null;
        }
        catch (Exception ex) { return L.T("eboot_erro") + ex.Message; }
        finally { try { Directory.Delete(temp, true); } catch { } }
    }
}

// PS3 firmware files the game needs: the system fonts (dev_flash\data\font) and four font and
// display modules, as flat images relocated where the recompiled code expects them. The personal
// build ships them in firmware\; the public one extracts them from the official PS3UPDAT.PUP.
static class Firmware
{
    struct Modulo { public string Nome; public uint Base; }
    static readonly Modulo[] Modulos = {
        new Modulo { Nome = "libfont",     Base = 0x02000000 },
        new Modulo { Nome = "libfontFT",   Base = 0x02100000 },
        new Modulo { Nome = "libfreetype", Base = 0x02200000 },
        new Modulo { Nome = "libresc",     Base = 0x02300000 },
    };
    // SHA-256 of each relocated image (fonts/font-manifest.json of the build).
    static readonly Dictionary<string, string> ShaImagem = new Dictionary<string, string> {
        { "libfont",     "9af9f79fccfc57e23b9b4bb88f04f7e1e74b70e961c3455ccfba69b6da9556ad" },
        { "libfontFT",   "d0fd56f40df93c42139dafe0fd1fd71b297820ef11e3e77218caf8033a393db3" },
        { "libfreetype", "59072f40efd0da040a6b511cb1269ccd2254b736454eecbfe2f077ed44994e49" },
        { "libresc",     "cd13231ec70c631bbed8b9773c8c4023af160792a985986a1ea4a7c61cbce8d9" },
    };

    public static string Pessoal { get { return Path.Combine(Instalacao.Pasta, "firmware"); } }
    public static string Extraido { get { return Path.Combine(Instalacao.Dados, "firmware"); } }

    static bool Completo(string raiz)
    {
        try
        {
            string fontes = Path.Combine(raiz, "dev_flash", "data", "font");
            if (!Directory.Exists(fontes) || Directory.GetFiles(fontes, "*.TTF").Length < 20) return false;
            foreach (var m in Modulos)
                if (!File.Exists(Path.Combine(raiz, "prx", m.Nome, m.Nome + "_image.bin"))) return false;
            return true;
        }
        catch { return false; }
    }
    // The decrypted modules the installer's build needs (prx\<name>\<name>.prx), or null when the
    // firmware has to be extracted again (one extracted by an older launcher kept only the images).
    public static string[] Prx()
    {
        var r = new List<string>();
        foreach (var m in Modulos)
        {
            string prx = Path.Combine(Extraido, "prx", m.Nome, m.Nome + ".prx");
            if (!File.Exists(prx)) return null;
            r.Add(prx);
        }
        return Completo(Extraido) ? r.ToArray() : null;
    }
    // The firmware root in use (with dev_flash\ and prx\), or null when it still has to be extracted.
    public static string Raiz()
    {
        if (Completo(Pessoal)) return Pessoal;
        if (Completo(Extraido)) return Extraido;
        return null;
    }

    static uint Be32(byte[] b, long o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }
    static ushort Be16(byte[] b, long o) { return (ushort)(b[o] << 8 | b[o + 1]); }
    static ulong Be64(byte[] b, long o) { return (ulong)Be32(b, o) << 32 | Be32(b, o + 4); }
    static void Poe(byte[] b, long o, ulong v, int bytes) { for (int i = bytes - 1; i >= 0; i--) { b[o + i] = (byte)v; v >>= 8; } }

    // A decrypted .prx (ELF64 PPC) placed at base: LOAD segments at base+vaddr, bss zeroed,
    // relocations applied (the same image ps3recomp's lift_prx.py builds).
    public static byte[] Relocar(byte[] d, uint baseAddr)
    {
        if (d.Length < 0x40 || d[0] != 0x7F || d[1] != (byte)'E' || d[4] != 2 || d[5] != 2) throw new Exception("não é um ELF64 big-endian");
        long phoff = (long)Be64(d, 0x20); int phnum = Be16(d, 0x38);
        var loads = new List<long[]>(); long relaOff = -1, relaSize = 0;
        for (int i = 0; i < phnum; i++)
        {
            long o = phoff + i * 0x38;
            uint tipo = Be32(d, o);
            if (tipo == 1) loads.Add(new long[] { (long)Be64(d, o + 8), (long)Be64(d, o + 0x10), (long)Be64(d, o + 0x20), (long)Be64(d, o + 0x28) });
            else if (tipo == 0x700000A4) { relaOff = (long)Be64(d, o + 8); relaSize = (long)Be64(d, o + 0x20); }
        }
        long topo = 0;
        foreach (var l in loads) topo = Math.Max(topo, l[1] + l[3]);
        var img = new byte[topo];
        var bases = new List<ulong>();
        foreach (var l in loads) { Array.Copy(d, l[0], img, l[1], l[2]); bases.Add(baseAddr + (ulong)l[1]); }
        if (relaOff < 0) throw new Exception("sem tabela de relocação");
        for (long i = 0; i < relaSize / 24; i++)
        {
            long o = relaOff + i * 24;
            ulong offset = Be64(d, o); int idxValor = d[o + 10], idxEndereco = d[o + 11];
            uint tipo = Be32(d, o + 12); ulong ptr = Be64(d, o + 16);
            long alvo = (long)(bases[idxEndereco] - baseAddr + offset);
            ulong valor = (idxValor == 0xFF ? 0 : bases[idxValor]) + ptr;
            switch (tipo)
            {
                case 1: Poe(img, alvo, valor & 0xFFFFFFFF, 4); break;                  // ADDR32
                case 4: Poe(img, alvo, valor & 0xFFFF, 2); break;                      // ADDR16_LO
                case 5: Poe(img, alvo, (valor >> 16) & 0xFFFF, 2); break;              // ADDR16_HI
                case 6: Poe(img, alvo, ((valor + 0x8000) >> 16) & 0xFFFF, 2); break;   // ADDR16_HA
                case 44: Poe(img, alvo, valor, 8); break;                              // REL64
                default: throw new Exception("relocação do tipo " + tipo + " não suportada");
            }
        }
        return img;
    }

    static string Sha256(byte[] b) { using (var s = SHA256.Create()) return BitConverter.ToString(s.ComputeHash(b)).Replace("-", "").ToLowerInvariant(); }

    // Builds the firmware folder from a dev_flash (RPCS3's, after installing the PUP).
    // Returns null or an error message.
    public static string GerarDe(string devFlash, string rpcs3, string destino)
    {
        string temp = Path.Combine(Path.GetTempPath(), "bleach-fw-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(temp);
            foreach (var m in Modulos)
            {
                string sprx = Path.Combine(devFlash, "sys", "external", m.Nome + ".sprx");
                if (!File.Exists(sprx)) return L.F("fw_sem_modulo", m.Nome);
                string copia = Path.Combine(temp, m.Nome + ".sprx");
                File.Copy(sprx, copia, true);
                Util.RodarRpcs3(rpcs3, "--decrypt \"" + copia + "\"", 120000);   // one file per call
                string prx = Path.Combine(temp, m.Nome + ".prx");
                if (!File.Exists(prx)) return L.F("fw_nao_decifrou", m.Nome);
                byte[] img = Relocar(File.ReadAllBytes(prx), m.Base);
                if (Sha256(img) != ShaImagem[m.Nome])
                    return L.F("fw_diferente", m.Nome);
                string pasta = Path.Combine(destino, "prx", m.Nome);
                Directory.CreateDirectory(pasta);
                File.WriteAllBytes(Path.Combine(pasta, m.Nome + "_image.bin"), img);
                // The decrypted module too: the installer lifts it into the game (construir.py).
                File.Copy(prx, Path.Combine(pasta, m.Nome + ".prx"), true);
            }
            string fontes = Path.Combine(devFlash, "data", "font");
            if (!Directory.Exists(fontes)) return L.T("fw_sem_fontes");
            Util.CopiarPasta(fontes, Path.Combine(destino, "dev_flash", "data", "font"));
            return Completo(destino) ? null : L.T("fw_fontes_incompletas");
        }
        catch (Exception ex) { return L.T("fw_erro") + ex.Message; }
        finally { try { Directory.Delete(temp, true); } catch { } }
    }

    // Public build: install the official PS3UPDAT.PUP into the bundled RPCS3, then extract.
    public static string InstalarPup(string pup)
    {
        string rpcs3 = Util.Rpcs3();
        if (rpcs3 == null) return L.T("sem_rpcs3");
        if (!Util.InstalarFirmwareRpcs3(rpcs3, pup, 600000)) return L.T("fw_nao_instalou");
        string devFlash = Path.Combine(Path.GetDirectoryName(rpcs3), "dev_flash");
        if (!File.Exists(Path.Combine(devFlash, "vsh", "etc", "version.txt"))) return L.T("fw_nao_instalou");
        return GerarDe(devFlash, rpcs3, Extraido);
    }
}

// Environment and start of the recomp; the launcher waits for it.
static class Execucao
{
    // The game runs in a job that closes with it: the decoders it starts (ffmpeg) never outlive it.
    [DllImport("kernel32.dll")] static extern IntPtr CreateJobObject(IntPtr a, string nome);
    [DllImport("kernel32.dll")] static extern bool SetInformationJobObject(IntPtr job, int classe, ref LimiteJob info, int tamanho);
    [DllImport("kernel32.dll")] public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr processo);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] struct LimiteJob
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit; public uint LimitFlags; public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit; public UIntPtr Affinity; public uint PriorityClass, SchedulingClass;
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    public static IntPtr JobQueFechaJunto()
    {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        var info = new LimiteJob { LimitFlags = 0x2000 };   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        SetInformationJobObject(job, 9, ref info, Marshal.SizeOf(typeof(LimiteJob)));   // JobObjectExtendedLimitInformation
        return job;
    }

    // 60/120 FPS: every scene task that never asks for sub-steps runs at the original 30 Hz
    // ("auto"), plus these manager tasks always; TarefasCheias always run every tick
    // (actors, their components, effects, hit director, input, dialog, camera, event timeline 308d1c --
    // its clock steps 1/r per tick in the game code -- and its MotionCamera task 378748, so the camera of
    // events such as the Ignition special move moves every tick instead of 30 times a second), and 6cf14,
    // which refills the 128-entry per-update pool attacks and projectiles register in every tick (held at
    // 30 Hz it filled with 4 ticks of entries: Uryuu's aerial Licht Regen crashed the game).
    // See PROGRESSO-CLAUDE.md.
    const string TarefasCheias = "a1770,1bbea4,1b5b1c,3cfb90,3cf72c,20fba8,645a8,1b76cc,155d44,615d0,308d1c,378748,6cf14";
    const string TarefasMeias = "30cb54,1d54cc,1d3020,1c25d0,1b9590,1b8b7c,1b82b0,1b7dbc,1b6e10,ee414,eb7c0,9583c,77dd0," +
        "742c8,72b54,706b0,6fc6c,42714,3f4dc,3da30,307604,39e18,381c84,384b78,368688,336ddc,1f79dc," +
        "3a19d0,3876d8,39fe84";

    // Borderless fullscreen or a window. Exclusive fullscreen is not offered (Alt+Tab out of it crashed
    // the game); an "exclusiva" saved by an older launcher becomes borderless.
    public static string Tela(string salva) { return salva == "janela" ? "janela" : "semborda"; }

    // Builds without ULTRAWIDE (compilar.sh without "ultrawide", the v1 release) hide the option and
    // always run the game 16:9; montar-instalador.py defines it.
#if ULTRAWIDE
    public const bool Ultrawide = true;
#else
    public const bool Ultrawide = false;
#endif
    // Monitor shape: "16:9" (the game's own), "21:9", "32:9" or "auto" (the primary monitor's). Anything else is 16:9.
    public static string Proporcao(string salva)
    {
        return Ultrawide && (salva == "auto" || salva == "21:9" || salva == "32:9") ? salva : "16:9";
    }

    public static Dictionary<string, string> Ambiente(string raizJogo, string firmware, string resolucao, string fps, string tela,
                                                       string overlay, string proporcao)
    {
        tela = Tela(tela);
        var e = new Dictionary<string, string>();
        // Runtime settings validated for this build.
        try
        {
            var cfg = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(
                File.ReadAllText(Path.Combine(Instalacao.Pasta, "configuracao.json"), Encoding.UTF8));
            foreach (var kv in cfg) e[kv.Key] = kv.Value == null ? null : Convert.ToString(kv.Value, System.Globalization.CultureInfo.InvariantCulture);
        }
        catch { }
        Action<string, string> padrao = delegate(string k, string v) { if (!e.ContainsKey(k)) e[k] = v; };
        padrao("RSX_LIVE_DRAW", "1"); padrao("PS3_VERBOSE", "0"); padrao("GCM_PARK_STRICT", "1");
        padrao("GCM_FLIPCB_ONTICK", "1"); padrao("GCM_SEMA_ACQUIRE", "1"); padrao("SPURS_JOB_WORKERS", "auto");
        e["SPU_INTERP_UNLIFTED"] = null;

        string dados = Instalacao.Dados;
        e["PS3_TITLE"] = Instalacao.Titulo;
        e["PS3_VFS_ROOT"] = Util.ParaJogo(raizJogo.TrimEnd('\\'));
        // Translated copies of a few disc files, built by the launcher from this dump (Traducao.cs).
        e["PS3_VFS_OVERLAY"] = overlay == null ? null : Util.ParaJogo(overlay);
        e["PS3_PARAM_SFO"] = Util.ParaJogo(Path.Combine(raizJogo, "PS3_GAME", "PARAM.SFO"));
        e["PS3_HDD0_ROOT"] = Util.ParaJogo(Path.Combine(dados, "hdd0"));
        e["PS3_GAMEDATA_ROOT"] = Util.ParaJogo(Path.Combine(dados, "gamedata"));
        e["PS3_DEV_FLASH"] = Util.ParaJogo(Path.Combine(firmware, "dev_flash"));
        e["BLEACH_PRX_ROOT"] = Util.ParaJogo(Path.Combine(firmware, "prx"));
        e["PS3_HLE_UNRESOLVED"] = "trap"; e["PS3_HLE_TRACE"] = "2000"; e["PS3_HLE_PTRCHECK"] = "1";
        e["PS3_SPU_STRICT"] = "1"; e["PS3_SPURS_REAL_EVENTS"] = "1";
        e["SPU_DUMP_MISS"] = Util.ParaJogo(Path.Combine(Instalacao.Logs, "spu-raw"));

        e["BLEACH_DISPLAY_MODE"] = tela;
        e["BLEACH_NATIVE_CAMERA"] = "0";
        int hz = fps == "120" ? 120 : fps == "60" ? 60 : 30;
        if (hz == 30) { e["BLEACH_EXPERIMENTAL_RENDER_HZ"] = "30"; e["BLEACH_PRESENT_PACE_HZ"] = "30"; }
        else
        {
            e["BLEACH_SIMULATION_HZ"] = hz.ToString();
            e["BLEACH_SUBSTEPS"] = (4 / (hz / 30)).ToString();
            e["BLEACH_SIM_GAMEPLAY_ONLY"] = "1";
            e["BLEACH_TASK_HALF"] = "auto," + TarefasMeias;
            e["BLEACH_TASK_FULL"] = TarefasCheias;
            e["BLEACH_CALL_HALF"] = "33b870,1d034";   // the actor's secondary animation clock; screen fades (func_00011680)
            e["BLEACH_EXPERIMENTAL_RENDER_HZ"] = hz.ToString();
            e["BLEACH_PRESENT_PACE_HZ"] = hz.ToString();
        }
        var escalas = new Dictionary<string, string> { { "720p", "1" }, { "1080p", "1.5" }, { "1440p", "2" }, { "4K", "3" } };
        string escala; e["BLEACH_RENDER_SCALE"] = escalas.TryGetValue(resolucao, out escala) ? escala : "3";
        // Ultrawide: wider 3D field of view, 2D kept in its 16:9 shape (rsx_aspect.h). 16:9 sets nothing.
        proporcao = Proporcao(proporcao);
        e["BLEACH_ASPECT"] = proporcao == "16:9" ? null : proporcao;
        return e;
    }

    // Keeps the last runs' logs only.
    static void PodarLogs()
    {
        try
        {
            var arquivos = new List<FileInfo>(new DirectoryInfo(Instalacao.Logs).GetFiles("jogo-*.log"));
            arquivos.Sort((a, b) => b.LastWriteTimeUtc.CompareTo(a.LastWriteTimeUtc));
            for (int i = 10; i < arquivos.Count; i++) arquivos[i].Delete();
        }
        catch { }
    }

    // Starts the game and waits for it. Returns its exit code.
    public static int Rodar(string raizJogo, string firmware, string resolucao, string fps, string tela, string overlay,
                            string proporcao)
    {
        string dados = Instalacao.Dados;
        Directory.CreateDirectory(Instalacao.Logs);
        Directory.CreateDirectory(Path.Combine(dados, "hdd0"));
        Directory.CreateDirectory(Path.Combine(dados, "gamedata"));
        PodarLogs();
        var psi = new ProcessStartInfo(Instalacao.Jogo, "\"" + Util.ParaJogo(Instalacao.Elf) + "\"")
        {
            UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = dados,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        foreach (var kv in Ambiente(raizJogo, firmware, resolucao, fps, tela, overlay, proporcao))
        {
            if (kv.Value == null) psi.EnvironmentVariables.Remove(kv.Key);
            else psi.EnvironmentVariables[kv.Key] = kv.Value;
        }
        string nome = "jogo-" + DateTime.Now.ToString("yyyyMMdd-HHmmss");
        using (var saida = new StreamWriter(Path.Combine(Instalacao.Logs, nome + ".log"), false, Encoding.UTF8))
        using (var erros = new StreamWriter(Path.Combine(Instalacao.Logs, nome + ".stderr.txt"), false, Encoding.UTF8))
        using (var p = new Process { StartInfo = psi })
        {
            p.OutputDataReceived += delegate(object s, DataReceivedEventArgs a) { if (a.Data != null) lock (saida) saida.WriteLine(a.Data); };
            p.ErrorDataReceived += delegate(object s, DataReceivedEventArgs a) { if (a.Data != null) lock (erros) erros.WriteLine(a.Data); };
            IntPtr job = JobQueFechaJunto();
            try
            {
                p.Start();
                AssignProcessToJobObject(job, p.Handle);
                p.BeginOutputReadLine(); p.BeginErrorReadLine();
                p.WaitForExit();
                lock (saida) saida.WriteLine("[launcher] o jogo terminou com o código " + p.ExitCode);
                return p.ExitCode;
            }
            finally { CloseHandle(job); }
        }
    }
}

// Windows shortcuts (.lnk) through the shell's IShellLink.
static class Atalhos
{
    [ComImport, Guid("00021401-0000-0000-C000-000000000046")] class ShellLink { }
    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
    interface IShellLinkW
    {
        void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder f, int c, IntPtr fd, int fl);
        void GetIDList(out IntPtr p); void SetIDList(IntPtr p);
        void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder n, int c);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string n);
        void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder d, int c);
        void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string d);
        void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder a, int c);
        void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string a);
        void GetHotkey(out short h); void SetHotkey(short h);
        void GetShowCmd(out int s); void SetShowCmd(int s);
        void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder p, int c, out int i);
        void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string p, int i);
        void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string p, int r);
        void Resolve(IntPtr h, int f);
        void SetPath([MarshalAs(UnmanagedType.LPWStr)] string f);
    }
    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("0000010b-0000-0000-C000-000000000046")]
    interface IPersistFile
    {
        void GetClassID(out Guid c); [PreserveSig] int IsDirty();
        void Load([MarshalAs(UnmanagedType.LPWStr)] string f, uint m);
        void Save([MarshalAs(UnmanagedType.LPWStr)] string f, [MarshalAs(UnmanagedType.Bool)] bool r);
        void SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string f); void GetCurFile([MarshalAs(UnmanagedType.LPWStr)] out string f);
    }

    public static void Criar(string lnk, string alvo, string argumentos, string icone, string descricao)
    {
        var link = (IShellLinkW)new ShellLink();
        link.SetPath(alvo);
        link.SetArguments(argumentos ?? "");
        link.SetWorkingDirectory(Path.GetDirectoryName(alvo));
        link.SetIconLocation(icone, 0);
        if (descricao != null) link.SetDescription(descricao);
        Directory.CreateDirectory(Path.GetDirectoryName(lnk));
        ((IPersistFile)link).Save(lnk, true);
    }

    // "Launch options" (in the installer's language): one shortcut per FPS and display mode, plus the
    // last used settings, for starting the game straight away (or from Steam or Playnite).
    public static void CriarOpcoes(string pasta)
    {
        string destino = Path.Combine(pasta, L.T("pasta_opcoes"));
        foreach (var nome in L.TodosOsIdiomas("pasta_opcoes"))   // a reinstall in another language
            if (nome != L.T("pasta_opcoes")) try { Directory.Delete(Path.Combine(pasta, nome), true); } catch { }
        Directory.CreateDirectory(destino);
        foreach (var antigo in Directory.GetFiles(destino, "*.lnk")) try { File.Delete(antigo); } catch { }   // e.g. a former "Fullscreen"
        string launcher = Path.Combine(pasta, "Bleach Launcher.exe");
        bool pt = L.Idioma == "pt";
        string jogar = pt ? "--jogar" : "--play", argTela = pt ? "--tela " : "--display ";
        string[][] telas = {
            new[] { pt ? "semborda" : "borderless", L.T("sem_borda") },
            new[] { pt ? "janela" : "windowed", L.T("janela") } };
        Criar(Path.Combine(destino, "Bleach - " + L.T("ultima_config") + ".lnk"), launcher, jogar, launcher, Instalacao.Titulo);
        foreach (var f in new[] { "30", "60", "120" })
            foreach (var t in telas)
                Criar(Path.Combine(destino, "Bleach - " + f + " FPS - " + t[1] + ".lnk"), launcher,
                      jogar + " --fps " + f + " " + argTela + t[0], launcher, Instalacao.Titulo);
        string leia = L.F("leia_me_texto", launcher);
        if (!Execucao.Ultrawide)   // no aspect option in this build
            leia = System.Text.RegularExpressions.Regex.Replace(leia, @"  --(aspect|proporcao) [^
]*
", "");
        File.WriteAllText(Path.Combine(destino, L.T("leia_me")), leia, Encoding.UTF8);
    }
}
