// Installer only: what it checks on the PC, what it downloads (official sources only, each file
// checked against its SHA-256) and how it rebuilds the game from the player's own dump with
// construir.py (bleach-recomp). Nothing derived from the game is shipped; it is all generated here.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

class Download
{
    public string Nome, Url, Arquivo, Sha256; public long Tamanho;
    public string Caminho { get { return Path.Combine(Rede.Cache, Arquivo); } }
    // Already downloaded (size only: the hash is checked when it is used).
    public bool EmCache { get { try { return new FileInfo(Caminho).Length == Tamanho; } catch { return false; } } }
}

static class Rede
{
    // Kept between installs (an update or a reinstall does not download them again); removed with
    // "also remove the saves and settings".
    public static string Cache { get { return Path.Combine(Instalacao.Dados, "downloads"); } }

    // The build tools, pinned: Python (python.org), Capstone, CMake and Ninja (PyPI), LLVM-MinGW (GitHub).
    public static readonly Download[] Ferramentas = {
        new Download { Nome = "Python 3.11.9", Arquivo = "python-3.11.9-embed-amd64.zip", Tamanho = 11249023,
            Url = "https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip",
            Sha256 = "009d6bf7e3b2ddca3d784fa09f90fe54336d5b60f0e0f305c37f400bf83cfd3b" },
        new Download { Nome = "Capstone 5.0.9", Arquivo = "capstone-5.0.9-py3-none-win_amd64.whl", Tamanho = 1273459,
            Url = "https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl",
            Sha256 = "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0" },
        new Download { Nome = "CMake 4.4.4", Arquivo = "cmake-4.4.4-py3-none-win_amd64.whl", Tamanho = 42321211,
            Url = "https://files.pythonhosted.org/packages/87/9c/a7918eeeed09b34aa5dc1b767367fb1a2dcbb56171be00be0fd1280fe752/cmake-4.4.4-py3-none-win_amd64.whl",
            Sha256 = "ad8e0a38b5707e27882701146bdfffecacd80e7703fa0fc36c77528518d545af" },
        new Download { Nome = "Ninja 1.13.0", Arquivo = "ninja-1.13.0-py3-none-win_amd64.whl", Tamanho = 309975,
            Url = "https://files.pythonhosted.org/packages/29/45/c0adfbfb0b5895aa18cec400c535b4f7ff3e52536e0403602fc1a23f7de9/ninja-1.13.0-py3-none-win_amd64.whl",
            Sha256 = "fb8ee8719f8af47fed145cced4a85f0755dd55d45b2bddaf7431fa89803c5f3e" },
        new Download { Nome = "LLVM-MinGW 20261020", Arquivo = "llvm-mingw-20261020-ucrt-x86_64.zip", Tamanho = 190612575,
            Url = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip",
            Sha256 = "fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd" },
    };
    public static long FaltaBaixar()
    {
        long n = 0;
        foreach (var d in Ferramentas) if (!d.EmCache) n += d.Tamanho;
        return n;
    }

    static Rede()
    {
        ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2 (.NET 4.0 lacks the name)
    }

    static HttpWebRequest Pedido(string url, int msLimite)
    {
        var r = (HttpWebRequest)WebRequest.Create(url);
        r.UserAgent = "BleachSoulResurreccion-Instalador/1.0";
        r.Timeout = msLimite; r.ReadWriteTimeout = 60000; r.AllowAutoRedirect = true;
        return r;
    }

    // Reachable at all (a few seconds): the PC check shows it before anything is downloaded.
    public static bool Online()
    {
        foreach (var url in new[] { "https://github.com", "https://www.python.org" })
            try
            {
                var r = Pedido(url, 6000); r.Method = "HEAD";
                using (r.GetResponse()) return true;
            }
            catch (WebException ex) { if (ex.Response != null) return true; }
            catch { }
        return false;
    }

    // Downloads url into destino (resuming a partial file), reporting (bytes, total).
    public static void Baixar(string url, string destino, long esperado, Action<long, long> progresso, Func<bool> cancelado)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destino));
        string parcial = destino + ".parcial";
        for (int tentativa = 1; ; tentativa++)
        {
            try
            {
                long tem = File.Exists(parcial) ? new FileInfo(parcial).Length : 0;
                var r = Pedido(url, 30000);
                if (tem > 0) r.AddRange(tem);
                using (var resp = (HttpWebResponse)r.GetResponse())
                {
                    bool continua = tem > 0 && resp.StatusCode == HttpStatusCode.PartialContent;
                    if (!continua) tem = 0;
                    long total = esperado > 0 ? esperado : (resp.ContentLength > 0 ? resp.ContentLength + tem : -1);
                    using (var s = resp.GetResponseStream())
                    using (var f = new FileStream(parcial, continua ? FileMode.Append : FileMode.Create))
                    {
                        var buf = new byte[1 << 16]; int n; long ultimo = 0;
                        while ((n = s.Read(buf, 0, buf.Length)) > 0)
                        {
                            if (cancelado()) throw new OperationCanceledException();
                            f.Write(buf, 0, n); tem += n;
                            if (progresso != null && tem - ultimo > (1 << 19)) { progresso(tem, total); ultimo = tem; }
                        }
                    }
                }
                if (File.Exists(destino)) File.Delete(destino);
                File.Move(parcial, destino);
                return;
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception)
            {
                if (tentativa >= 4 || cancelado()) throw;
                System.Threading.Thread.Sleep(2000 * tentativa);
            }
        }
    }

    public static string Baixar(Download d, Action<long, long> progresso, Func<bool> cancelado)
    {
        if (!d.EmCache || Util.Sha256(d.Caminho) != d.Sha256)
        {
            try
            {
                if (File.Exists(d.Caminho)) File.Delete(d.Caminho);
                Baixar(d.Url, d.Caminho, d.Tamanho, progresso, cancelado);
            }
            catch (WebException ex) { throw new Exception(L.F("download_falhou", d.Nome, d.Url, ex.Message)); }
            if (Util.Sha256(d.Caminho) != d.Sha256)
            {
                try { File.Delete(d.Caminho); } catch { }
                throw new Exception(L.F("download_corrompido", d.Nome));
            }
        }
        return d.Caminho;
    }

    // The PS3 system software from Sony's own update server (the address the console itself uses).
    const string ListaSony = "http://fus01.ps3.update.playstation.net/update/ps3/list/us/ps3-updatelist.txt";
    public static string Pup { get { return Path.Combine(Cache, "PS3UPDAT.PUP"); } }
    public static bool PupEmCache { get { return PupValido(Pup); } }
    public static bool PupValido(string arquivo)
    {
        try
        {
            if (new FileInfo(arquivo).Length < 100000000) return false;
            var cab = new byte[5];
            using (var f = File.OpenRead(arquivo)) f.Read(cab, 0, 5);
            return Encoding.ASCII.GetString(cab) == "SCEUF";
        }
        catch { return false; }
    }
    public static string BaixarPup(Action<long, long> progresso, Func<bool> cancelado)
    {
        if (PupEmCache) return Pup;
        string url;
        try
        {
            string lista;
            using (var resp = Pedido(ListaSony, 30000).GetResponse())
            using (var s = new StreamReader(resp.GetResponseStream())) lista = s.ReadToEnd();
            var m = Regex.Match(lista, @"CDN=(http[^;\s]+/PS3UPDAT\.PUP)");
            if (!m.Success) throw new Exception("PS3UPDAT.PUP?");
            url = m.Groups[1].Value;
            Baixar(url, Pup, -1, progresso, cancelado);
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception ex) { throw new Exception(L.F("pup_falhou", ex.Message)); }
        if (!PupValido(Pup)) { try { File.Delete(Pup); } catch { } throw new Exception(L.F("pup_falhou", "SCEUF")); }
        return Pup;
    }

    public static void Extrair(string zip, string destino, Func<string, string> mapear, Action<long, long> progresso,
                               Func<bool> cancelado)
    {
        using (var z = ZipFile.OpenRead(zip))
        {
            long total = 0, feito = 0;
            foreach (var e in z.Entries) total += e.Length;
            var buf = new byte[1 << 20];
            string raiz = Path.GetFullPath(destino).TrimEnd('\\') + "\\";
            foreach (var e in z.Entries)
            {
                if (cancelado()) throw new OperationCanceledException();
                string nome = mapear == null ? e.FullName : mapear(e.FullName);
                feito += e.Length;
                if (string.IsNullOrEmpty(nome) || nome.EndsWith("/")) continue;
                string alvo = Path.GetFullPath(Path.Combine(raiz, nome.Replace('/', '\\')));
                if (!alvo.StartsWith(raiz, StringComparison.OrdinalIgnoreCase)) continue;
                Directory.CreateDirectory(Path.GetDirectoryName(alvo));
                using (var de = e.Open()) using (var para = File.Create(alvo))
                { int n; while ((n = de.Read(buf, 0, buf.Length)) > 0) para.Write(buf, 0, n); }
                if (progresso != null) progresso(feito, total);
            }
        }
    }
}

// What the PC check shows before installing.
static class Sistema
{
    [StructLayout(LayoutKind.Sequential)] struct EstadoMemoria
    {
        public uint Tamanho, Carga; public ulong Total, Livre, PaginaTotal, PaginaLivre, VirtualTotal, VirtualLivre, Extendida;
    }
    [DllImport("kernel32.dll")] static extern bool GlobalMemoryStatusEx(ref EstadoMemoria m);
    public static double MemoriaGb()
    {
        var m = new EstadoMemoria { Tamanho = (uint)Marshal.SizeOf(typeof(EstadoMemoria)) };
        return GlobalMemoryStatusEx(ref m) ? m.Total / 1073741824.0 : 0;
    }

    [DllImport("d3d12.dll")] static extern int D3D12CreateDevice(IntPtr adaptador, int nivel, ref Guid iid, IntPtr dispositivo);
    // A DirectX 12 (feature level 11_0) graphics card: asked without creating the device (S_FALSE = it would work).
    public static bool DirectX12()
    {
        try { var iid = new Guid("189819f1-1db6-4b57-be54-1821339b85f7"); return D3D12CreateDevice(IntPtr.Zero, 0xb000, ref iid, IntPtr.Zero) >= 0; }
        catch { return false; }
    }

    public static bool Windows64() { return Environment.Is64BitOperatingSystem && Environment.OSVersion.Version.Major >= 10; }

    // Parallel compilations: one per core, within the memory (measured: up to ~0.45 GB per compiler).
    public static int Tarefas()
    {
        int porMemoria = (int)Math.Max(1, (MemoriaGb() - 2) / 0.6);
        return Math.Max(1, Math.Min(Environment.ProcessorCount, porMemoria));
    }
}

// Rebuilding the game on the player's PC: the tools, then construir.py.
static class Compilacao
{
    // LLVM-MinGW targets four CPUs; the game needs x86_64 only (and not the debugger's Python).
    static string SoX64(string nome)
    {
        int barra = nome.IndexOf('/');
        if (barra < 0) return null;
        string resto = nome.Substring(barra + 1);
        foreach (var fora in new[] { "aarch64-w64-mingw32/", "armv7-w64-mingw32/", "i686-w64-mingw32/", "python/" })
            if (resto.StartsWith(fora)) return null;
        return resto;
    }

    // Unpacks the downloaded tools into <trabalho>\ferramentas. Returns { python, cmake, ninja, toolchain bin }.
    public static string[] PrepararFerramentas(string trabalho, Action<string, long, long> progresso, Func<bool> cancelado)
    {
        string f = Path.Combine(trabalho, "ferramentas"), py = Path.Combine(f, "python");
        if (Directory.Exists(f)) Directory.Delete(f, true);
        var d = Rede.Ferramentas;
        Rede.Extrair(d[0].Caminho, py, null, (a, b) => progresso(d[0].Nome, a, b), cancelado);
        // Without the ._pth file the embedded Python behaves like an installed one: the script's folder
        // and Lib\site-packages are on its path (the lifter imports its neighbours, Capstone lives there).
        foreach (var pth in Directory.GetFiles(py, "*._pth")) File.Delete(pth);
        string site = Path.Combine(py, "Lib", "site-packages");
        for (int i = 1; i <= 3; i++) { int k = i; Rede.Extrair(d[k].Caminho, site, null, (a, b) => progresso(d[k].Nome, a, b), cancelado); }
        string llvm = Path.Combine(f, "llvm-mingw");
        Rede.Extrair(d[4].Caminho, llvm, SoX64, (a, b) => progresso(d[4].Nome, a, b), cancelado);
        return new[] { Path.Combine(py, "python.exe"), Path.Combine(site, "cmake", "data", "bin", "cmake.exe"),
                       Path.Combine(site, "ninja-1.13.0.data", "scripts", "ninja.exe"), Path.Combine(llvm, "bin") };
    }

    // Runs construir.py; progresso(sub-step 1..7, done, total). The output goes to the log.
    public static void Construir(string[] ferramentas, string kit, string elf, string[] prx, string trabalho, string bin,
                                 string log, Action<int, int, int> progresso, Func<bool> cancelado)
    {
        // The sources go next to the tools too, so no build path depends on the chosen install folder.
        string copia = Path.Combine(trabalho, "kit");
        if (Directory.Exists(copia)) Directory.Delete(copia, true);
        Util.CopiarPasta(kit, copia); kit = copia;
        string fw = Path.Combine(trabalho, "firmware");
        Directory.CreateDirectory(fw);
        foreach (var p in prx) File.Copy(p, Path.Combine(fw, Path.GetFileName(p)), true);
        string args = "-u \"" + Path.Combine(kit, "bleach-recomp", "construir.py") + "\" --elf \"" + elf + "\" --firmware \"" + fw +
                      "\" --kit \"" + kit + "\" --toolchain \"" + ferramentas[3] + "\" --cmake \"" + ferramentas[1] +
                      "\" --ninja \"" + ferramentas[2] + "\" --trabalho \"" + Path.Combine(trabalho, "jogo") +
                      "\" --saida \"" + bin + "\" --jobs " + Sistema.Tarefas();
        var psi = new ProcessStartInfo(ferramentas[0], args)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8, WorkingDirectory = trabalho
        };
        psi.EnvironmentVariables["PYTHONUTF8"] = "1";
        psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
        psi.EnvironmentVariables.Remove("PYTHONHOME"); psi.EnvironmentVariables.Remove("PYTHONPATH");
        Directory.CreateDirectory(Path.GetDirectoryName(log));
        var ultimas = new List<string>();
        int etapa = 1;
        using (var saida = new StreamWriter(log, false, Encoding.UTF8))
        using (var p = new Process { StartInfo = psi })
        {
            DataReceivedEventHandler ler = delegate(object s, DataReceivedEventArgs a)
            {
                if (a.Data == null) return;
                lock (saida)
                {
                    var c = a.Data.Split(' ');
                    if (c[0] == "@@etapa" && c.Length >= 3) { etapa = int.Parse(c[1]); progresso(etapa, 0, 1); }
                    else if (c[0] == "@@compilando" && c.Length >= 3) progresso(etapa, int.Parse(c[1]), int.Parse(c[2]));
                    else { saida.WriteLine(a.Data); ultimas.Add(a.Data); if (ultimas.Count > 25) ultimas.RemoveAt(0); }
                }
            };
            p.OutputDataReceived += ler; p.ErrorDataReceived += ler;
            // In a job that closes with the installer: cancelling (or closing it) stops every compiler.
            IntPtr job = Execucao.JobQueFechaJunto();
            try
            {
                p.Start();
                Execucao.AssignProcessToJobObject(job, p.Handle);
                p.BeginOutputReadLine(); p.BeginErrorReadLine();
                while (!p.WaitForExit(500))
                    if (cancelado()) { Execucao.CloseHandle(job); job = IntPtr.Zero; p.WaitForExit(10000); throw new OperationCanceledException(); }
                p.WaitForExit();
                if (p.ExitCode != 0)
                {
                    string fim; lock (saida) fim = string.Join("\n", ultimas.ToArray());
                    throw new Exception(L.F("compilacao_falhou", log) + "\n\n" + fim);
                }
            }
            finally { if (job != IntPtr.Zero) Execucao.CloseHandle(job); }
        }
    }
}
