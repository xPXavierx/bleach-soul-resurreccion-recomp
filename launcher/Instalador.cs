// Bleach: Soul Resurrección installer: a single .exe carrying the launcher, RPCS3, FFmpeg and the
// recomp's source kit as a zip appended to its own end ([stub][zip][zip length, 8 bytes][magic "BLEACHPK"]).
// It carries nothing of the game: it checks the PC, asks for the player's own dump, decrypts its EBOOT
// (RPCS3), gets the official PS3 firmware from Sony, downloads the build tools from their official sites
// and recompiles the game on this PC (Compilacao.cs). Then it creates the desktop and Start menu
// shortcuts, the "Launch options" shortcuts and the Windows uninstall entry. Run as Desinstalar.exe
// (or with /desinstalar) it removes the game. Builds with compilar-instalador.sh.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;

static class Pacote
{
    static readonly byte[] Magica = Encoding.ASCII.GetBytes("BLEACHPK");
    public static string Proprio { get { return Application.ExecutablePath; } }

    // Offset and length of the appended zip, or null when this copy carries none (the uninstaller).
    public static long[] Localizar()
    {
        using (var f = File.OpenRead(Proprio))
        {
            if (f.Length < 16) return null;
            var fim = new byte[16];
            f.Seek(-16, SeekOrigin.End); f.Read(fim, 0, 16);
            for (int i = 0; i < 8; i++) if (fim[8 + i] != Magica[i]) return null;
            long tamanho = BitConverter.ToInt64(fim, 0);
            return new[] { f.Length - 16 - tamanho, tamanho };
        }
    }
    // This program without the payload: the uninstaller.
    public static void CopiarStub(string destino)
    {
        var p = Localizar();
        using (var f = File.OpenRead(Proprio)) using (var d = File.Create(destino))
        {
            long resta = p == null ? f.Length : p[0]; var buf = new byte[1 << 20];
            while (resta > 0) { int n = f.Read(buf, 0, (int)Math.Min(buf.Length, resta)); if (n <= 0) break; d.Write(buf, 0, n); resta -= n; }
        }
    }
}

// A read-only window into part of a file (the zip inside this .exe).
class Trecho : Stream
{
    readonly Stream s; readonly long inicio, tamanho; long pos;
    public Trecho(Stream s, long inicio, long tamanho) { this.s = s; this.inicio = inicio; this.tamanho = tamanho; }
    public override bool CanRead { get { return true; } }
    public override bool CanSeek { get { return true; } }
    public override bool CanWrite { get { return false; } }
    public override long Length { get { return tamanho; } }
    public override long Position { get { return pos; } set { pos = value; } }
    public override int Read(byte[] b, int o, int n)
    {
        if (pos >= tamanho) return 0;
        n = (int)Math.Min(n, tamanho - pos);
        s.Seek(inicio + pos, SeekOrigin.Begin);
        int lidos = s.Read(b, o, n); pos += lidos; return lidos;
    }
    public override long Seek(long o, SeekOrigin origem)
    {
        pos = origem == SeekOrigin.Begin ? o : origem == SeekOrigin.Current ? pos + o : tamanho + o; return pos;
    }
    public override void Flush() { }
    public override void SetLength(long v) { throw new NotSupportedException(); }
    public override void Write(byte[] b, int o, int n) { throw new NotSupportedException(); }
    protected override void Dispose(bool d) { if (d) s.Dispose(); base.Dispose(d); }
}

static class Registro
{
    const string Chave = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\" + Instalacao.Id;
    // For all users when elevated (Program Files), else for the current user.
    public static void Gravar(string pasta, long kb)
    {
        RegistryKey k;
        try { k = Registry.LocalMachine.CreateSubKey(Chave); }
        catch (UnauthorizedAccessException) { k = Registry.CurrentUser.CreateSubKey(Chave); }
        using (k)
        {
            k.SetValue("DisplayName", Instalacao.Titulo);
            k.SetValue("DisplayIcon", Path.Combine(pasta, "Bleach Launcher.exe") + ",0");
            k.SetValue("DisplayVersion", Instalacao.Versao);
            k.SetValue("Publisher", "Bleach Recomp");
            k.SetValue("InstallLocation", pasta);
            k.SetValue("UninstallString", "\"" + Path.Combine(pasta, "Desinstalar.exe") + "\" /desinstalar");
            k.SetValue("EstimatedSize", (int)Math.Min(kb, int.MaxValue), RegistryValueKind.DWord);
            k.SetValue("NoModify", 1, RegistryValueKind.DWord);
            k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        }
    }
    public static string Pasta()
    {
        foreach (var raiz in new[] { Registry.LocalMachine, Registry.CurrentUser })
            using (var k = raiz.OpenSubKey(Chave)) if (k != null) return k.GetValue("InstallLocation") as string;
        return null;
    }
    public static void Apagar()
    {
        try { Registry.LocalMachine.DeleteSubKeyTree(Chave, false); } catch { }
        try { Registry.CurrentUser.DeleteSubKeyTree(Chave, false); } catch { }
    }
}

// Shortcuts for all users when elevated, else for the current user.
static class Locais
{
    public static string AtalhoDesktop(bool todos)
    {
        return Path.Combine(Environment.GetFolderPath(todos ? Environment.SpecialFolder.CommonDesktopDirectory : Environment.SpecialFolder.DesktopDirectory),
                            Instalacao.NomeAtalho + ".lnk");
    }
    public static string MenuIniciar(bool todos)
    {
        return Path.Combine(Environment.GetFolderPath(todos ? Environment.SpecialFolder.CommonPrograms : Environment.SpecialFolder.Programs),
                            Instalacao.NomeAtalho);
    }
    public static bool Elevado()
    {
        return new System.Security.Principal.WindowsPrincipal(System.Security.Principal.WindowsIdentity.GetCurrent())
            .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
    }
}

// What the player chose on the options page.
class Escolhas
{
    public string Destino, Jogo, Pup;   // Pup null: downloaded from Sony (when the firmware is still needed)
    public bool Desktop = true, Menu = true;
}

// What installing and removing do, with or without the window.
static class Operacoes
{
    // The steps the progress page lists, in order.
    public static readonly string[] Etapas = { "etapa_copiar", "etapa_eboot", "etapa_firmware", "etapa_ferramentas", "etapa_recompilar", "etapa_finalizar" };
    public static string Log { get { return Path.Combine(Instalacao.Logs, "instalacao.log"); } }

    static int Mil(long a, long b) { return b <= 0 ? 0 : (int)Math.Max(0, Math.Min(1000, a * 1000 / b)); }
    static string Mb(long a, long b) { return b > 0 ? L.F("mb_de", a / 1e6, b / 1e6) : L.F("mb", a / 1e6); }

    // progresso(step, 0..1000, detail).
    public static void Instalar(Escolhas o, Action<int, int, string> progresso, Func<bool> cancelado)
    {
        var p = Pacote.Localizar();
        if (p == null) throw new Exception(L.T("incompleto"));
        string destino = o.Destino;
        Instalacao.Pasta = destino;   // RPCS3, the translations and the firmware are found from here
        Directory.CreateDirectory(destino);

        // 1. The program files.
        using (var zip = new ZipArchive(new Trecho(File.OpenRead(Pacote.Proprio), p[0], p[1]), ZipArchiveMode.Read))
        {
            long total = 0, feito = 0;
            foreach (var e in zip.Entries) total += e.Length;
            var buf = new byte[1 << 20];
            string raiz = destino.TrimEnd('\\') + "\\";
            foreach (var e in zip.Entries)
            {
                if (cancelado()) throw new OperationCanceledException();
                string alvo = Path.GetFullPath(Path.Combine(destino, e.FullName.Replace('/', '\\')));
                if (!alvo.StartsWith(raiz, StringComparison.OrdinalIgnoreCase)) continue;
                if (e.FullName.EndsWith("/")) { Directory.CreateDirectory(alvo); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(alvo));
                using (var de = e.Open()) using (var para = File.Create(alvo))
                {
                    int n;
                    while ((n = de.Read(buf, 0, buf.Length)) > 0) { para.Write(buf, 0, n); feito += n; }
                }
                progresso(0, Mil(feito, total), e.FullName);
            }
        }
        Pacote.CopiarStub(Path.Combine(destino, "Desinstalar.exe"));
        File.WriteAllText(L.Arquivo(destino), L.Idioma);
        // Registered now, so even an install that fails later can be removed from Windows Settings.
        Registro.Gravar(destino, 0);

        // 2. The player's EBOOT, decrypted by RPCS3 into the data folder.
        progresso(1, 0, o.Jogo);
        var dump = new Dump(o.Jogo);
        string raizJogo = dump.Abrir();
        try
        {
            if (raizJogo == null || !File.Exists(Path.Combine(raizJogo, "PS3_GAME", "USRDIR", "EBOOT.BIN")))
                throw new Exception(dump.Iso ? L.T("iso_falhou") : L.T("sem_eboot"));
            if (!Dump.EhBleach(raizJogo)) throw new Exception(L.T("nao_bleach"));
            if (!Dump.ElfPronto())
            {
                string erro = Dump.Decifrar(raizJogo);
                if (erro != null) throw new Exception(erro);
            }
        }
        finally { dump.Fechar(); }
        var cfg = Opcoes.Ler(); cfg["jogo"] = o.Jogo; cfg["idioma"] = L.Idioma; cfg.Salvar();   // the launcher opens ready
        progresso(1, 1000, "EBOOT.elf ✓");

        // 3. The PS3 system fonts and font modules, from Sony's firmware file.
        string[] prx = Firmware.Prx();
        if (prx == null)
        {
            string pup = o.Pup;
            if (pup == null)
                pup = Rede.BaixarPup((a, b) => progresso(2, Mil(a, b) * 7 / 10, L.T("baixando_pup") + "  " + Mb(a, b)), cancelado);
            progresso(2, 700, L.T("instalando_pup"));
            string erro = Firmware.InstalarPup(pup);
            if (erro != null) throw new Exception(erro);
            prx = Firmware.Prx();
            if (prx == null) throw new Exception(L.T("fw_nao_instalou"));
        }
        progresso(2, 1000, "✓");

        // 4. The build tools.
        long totalBaixar = 0, baixado = 0;
        foreach (var d in Rede.Ferramentas) totalBaixar += d.Tamanho;
        foreach (var d in Rede.Ferramentas)
        {
            long antes = baixado;
            Rede.Baixar(d, (a, b) => progresso(3, Mil(antes + a, totalBaixar) * 8 / 10, L.F("baixando", d.Nome) + "  " + Mb(a, b)), cancelado);
            baixado += d.Tamanho;
        }
        // A short path at the drive's root: the tools and the build nest deep, past Windows' 260 characters.
        string trabalho = Path.Combine(Path.GetPathRoot(destino), "BleachRecomp-compilacao");
        try { Directory.CreateDirectory(trabalho); }
        catch (Exception) { trabalho = Path.Combine(destino, "compilacao"); }
        try
        {
            var ferramentas = Compilacao.PrepararFerramentas(trabalho,
                (nome, a, b) => progresso(3, 800 + Mil(a, b) / 5, L.F("extraindo_ferramenta", nome)), cancelado);

            // 5. The game, recompiled on this PC.
            progresso(4, 0, L.T("sub_1"));
            Compilacao.Construir(ferramentas, Path.Combine(destino, "fonte"), Instalacao.Elf, prx, trabalho,
                Path.Combine(destino, "bin"), Log, (etapa, feito, total) =>
                {
                    // Generating the code takes ~1 minute, compiling all the rest.
                    if (etapa == 6) progresso(4, 60 + Mil(feito, total) * 93 / 100, L.F("compilando", feito, total));
                    else progresso(4, etapa < 6 ? etapa * 10 : 990, L.T("sub_" + etapa));
                }, cancelado);
        }
        finally
        {
            // The tools and the generated sources are not needed to play (about 2 GB).
            progresso(5, 100, L.T("limpando"));
            try { Directory.Delete(trabalho, true); } catch { }
        }

        // 6. Shortcuts.
        progresso(5, 500, "");
        string launcher = Path.Combine(destino, "Bleach Launcher.exe");
        Atalhos.CriarOpcoes(destino);
        bool todos = Locais.Elevado();
        if (o.Desktop) Atalhos.Criar(Locais.AtalhoDesktop(todos), launcher, "", launcher, Instalacao.Titulo);
        if (o.Menu)
        {
            Atalhos.Criar(Path.Combine(Locais.MenuIniciar(todos), Instalacao.NomeAtalho + ".lnk"), launcher, "", launcher, Instalacao.Titulo);
            Atalhos.Criar(Path.Combine(Locais.MenuIniciar(todos), L.T("desinstalar") + ".lnk"), Path.Combine(destino, "Desinstalar.exe"), "/desinstalar",
                          launcher, L.T("desinstalar") + " " + Instalacao.Titulo);
        }
        long bytes = 0;
        foreach (var f in Directory.GetFiles(destino, "*", SearchOption.AllDirectories)) try { bytes += new FileInfo(f).Length; } catch { }
        Registro.Gravar(destino, bytes / 1024);
        progresso(5, 1000, "");
    }

    // Removes the installed game (the folder this uninstaller is in) and, if asked, the saves and options.
    public static void Remover(string pastaJogo, bool apagarDados)
    {
        foreach (bool todos in new[] { true, false })
        {
            try { File.Delete(Locais.AtalhoDesktop(todos)); } catch { }
            try { Directory.Delete(Locais.MenuIniciar(todos), true); } catch { }
        }
        Registro.Apagar();
        foreach (var d in Directory.GetDirectories(pastaJogo)) try { Directory.Delete(d, true); } catch { }
        foreach (var f in Directory.GetFiles(pastaJogo))
            if (!string.Equals(f, Application.ExecutablePath, StringComparison.OrdinalIgnoreCase)) try { File.Delete(f); } catch { }
        if (apagarDados) try { Directory.Delete(Instalacao.Dados, true); } catch { }
        // This program and the folder go once it has exited.
        Process.Start(new ProcessStartInfo("cmd.exe", "/c ping -n 3 127.0.0.1 >nul & del /f /q \"" + Application.ExecutablePath +
            "\" & rmdir \"" + pastaJogo + "\"") { CreateNoWindow = true, UseShellExecute = false });
    }
}

// One line of the PC check or of the progress list: a mark, a name and a status.
class Linha
{
    public Label Marca, Nome, Estado;
    public void Por(string marca, Color cor, string estado)
    {
        Marca.Text = marca; Marca.ForeColor = cor;
        if (estado != null) Estado.Text = estado;
    }
    public bool Visivel { set { Marca.Visible = Nome.Visible = Estado.Visible = value; } }
}

class JanelaInstalador : Form
{
    static readonly Color Ok = Color.FromArgb(150, 225, 160), Baixar = Tema.Azul, Atencao = Color.FromArgb(255, 200, 120),
                          Ruim = Color.FromArgb(255, 130, 120), Pendente = Color.FromArgb(120, 120, 128);
    Image fundo;
    readonly bool desinstalar;
    int pagina;   // 0 PC check, 1 options, 2 installing, 3 done (uninstaller: 1 and 3)
    readonly List<Control>[] paginas = { new List<Control>(), new List<Control>(), new List<Control>(), new List<Control>() };
    Grupo idioma, firmware;
    BotaoJogar botao;
    Opcao voltar, cancelar;
    Point arrasto;
    bool cancelado;
    readonly Escolhas escolhas = new Escolhas();
    readonly Stopwatch relogio = new Stopwatch();
    readonly Timer tique = new Timer { Interval = 1000 };

    // page 0
    Label titulo0, intro, notaRpcs3;
    readonly Dictionary<string, Linha> verificacao = new Dictionary<string, Linha>();
    static readonly string[] Itens = { "ver_windows", "ver_video", "ver_memoria", "ver_disco", "ver_internet", "ver_ferramentas", "ver_firmware", "ver_rpcs3", "ver_ffmpeg" };
    bool pcBloqueado;
    // page 1
    Label rPasta, rJogo, rFirmware, estadoJogo, estadoFirmware, info;
    TextBox pasta, jogo;
    Opcao procurar, escolherJogo;
    CheckBox desktop, menu;
    bool jogoValido;
    // page 2
    Label titulo2, detalhe, tempo, notaTempo;
    readonly List<Linha> etapas = new List<Linha>();
    ProgressBar barra;
    int etapaAtual = -1;
    // page 3
    Label titulo3, final;
    CheckBox abrir;

    public JanelaInstalador(bool desinstalar)
    {
        this.desinstalar = desinstalar;
        Text = Instalacao.Titulo + (desinstalar ? " - Desinstalar" : " - Instalação");
        FormBorderStyle = FormBorderStyle.None; StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(820, desinstalar ? 470 : 600); DoubleBuffered = true; BackColor = Color.Black;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
        fundo = Arte.Fundo();
        Montar();
        Scale(new SizeF(K, K));
        MouseDown += delegate(object o, MouseEventArgs e) { if (e.Button == MouseButtons.Left) arrasto = e.Location; };
        MouseMove += delegate(object o, MouseEventArgs e)
        { if (e.Button == MouseButtons.Left) Location = new Point(Location.X + e.X - arrasto.X, Location.Y + e.Y - arrasto.Y); };
        tique.Tick += delegate { tempo.Text = L.F("tempo", relogio.Elapsed.Hours, relogio.Elapsed.Minutes, relogio.Elapsed.Seconds); };
        FormClosing += delegate(object o, FormClosingEventArgs e)
        {
            if (pagina != 2) return;
            e.Cancel = true;
            if (!cancelado && MessageBox.Show(this, L.T("cancelar_pergunta"), Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
            { cancelado = true; detalhe.Text = L.T("cancelando"); }
        };
    }

    Label Texto(int pg, string t, int x, int y, float tamanho, bool negrito, Color cor)
    {
        var l = new Label { Text = t, AutoSize = true, BackColor = Color.Transparent, ForeColor = cor, Location = new Point(x, y),
                            Font = new Font("Segoe UI", tamanho, negrito ? FontStyle.Bold : FontStyle.Regular) };
        Controls.Add(l); paginas[pg].Add(l); return l;
    }
    // Wrapped text in a fixed box (scaled with the window).
    Label Paragrafo(int pg, int x, int y, int w, int h, float tamanho, Color cor)
    {
        var l = Texto(pg, "", x, y, tamanho, false, cor);
        l.AutoSize = false; l.Size = new Size(w, h); l.AutoEllipsis = true; return l;
    }
    CheckBox Marca(int pg, int x, int y)
    {
        var c = new CheckBox { Checked = true, AutoSize = true, BackColor = Color.Transparent, ForeColor = Tema.Texto,
                               Font = new Font("Segoe UI", 9.5f), Location = new Point(x, y) };
        Controls.Add(c); paginas[pg].Add(c); return c;
    }
    Linha NovaLinha(int pg, int y, int xEstado, int wEstado)
    {
        var l = new Linha();
        l.Marca = Texto(pg, "…", 32, y, 10f, true, Pendente);
        l.Nome = Texto(pg, "", 56, y + 1, 9.5f, false, Tema.Texto);
        l.Estado = Paragrafo(pg, xEstado, y + 2, wEstado, 20, 8.5f, Tema.TextoApagado);
        return l;
    }
    Opcao Botao(int pg, int x, int y, int w, string valor)
    {
        var o = new Opcao("", valor) { Location = new Point(x, y), Width = w };
        Controls.Add(o); paginas[pg].Add(o); return o;
    }

    void Montar()
    {
        botao = new BotaoJogar { Location = new Point(644, desinstalar ? 334 : 470), Size = new Size(150, 96) };
        botao.Font = new Font("Segoe UI Black", 15f, FontStyle.Bold);
        botao.Click += delegate { Avancar(); };
        Controls.Add(botao);
        var fechar = new Opcao("✕", "x") { Location = new Point(820 - 46, 10), Size = new Size(36, 30) };
        fechar.Click += delegate { Close(); };
        Controls.Add(fechar);
        idioma = new Grupo(this, 820 - 46 - 10 - 4 * 46, 10, 40, L.Nomes[0], L.Codigos[0], L.Nomes[1], L.Codigos[1],
                       L.Nomes[2], L.Codigos[2], L.Nomes[3], L.Codigos[3]);
        foreach (var o in idioma.Opcoes) { o.Height = 30; o.Font = new Font("Segoe UI Semibold", 9f); }
        idioma.Valor = L.Idioma;
        idioma.Mudou += delegate { L.Idioma = idioma.Valor; AplicarIdioma(); };

        if (desinstalar)
        {
            desktop = Marca(1, 32, 394); desktop.Checked = false;
            info = Texto(1, Path.GetDirectoryName(Application.ExecutablePath), 32, 428, 8.5f, false, Tema.TextoApagado);
            titulo3 = Texto(3, "", 32, 334, 8.5f, true, Tema.VermelhoClaro);
            Mostrar(1); AplicarIdioma();
            return;
        }

        // 0: what will happen and what this PC has.
        titulo0 = Texto(0, "", 32, 262, 8.5f, true, Tema.VermelhoClaro);
        intro = Paragrafo(0, 32, 282, 600, 52, 9f, Tema.Texto);
        for (int i = 0; i < Itens.Length; i++) verificacao[Itens[i]] = NovaLinha(0, 340 + i * 21, 300, 330);
        notaRpcs3 = Paragrafo(0, 32, 535, 600, 58, 8.5f, Tema.TextoApagado);

        // 1: folder, dump, firmware.
        rPasta = Texto(1, "", 32, 262, 8.5f, true, Tema.VermelhoClaro);
        string padrao = Registro.Pasta() ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Bleach Soul Resurreccion");
        pasta = new TextBox { Text = padrao, Location = new Point(32, 282), Size = new Size(470, 26), Font = new Font("Segoe UI", 10f),
                              BackColor = Color.FromArgb(22, 22, 28), ForeColor = Color.White, BorderStyle = BorderStyle.FixedSingle };
        Controls.Add(pasta); paginas[1].Add(pasta);
        procurar = Botao(1, 510, 279, 110, "p");
        procurar.Click += delegate
        {
            using (var d = new FolderBrowserDialog { SelectedPath = pasta.Text, ShowNewFolderButton = true })
                if (d.ShowDialog(this) == DialogResult.OK)
                    pasta.Text = d.SelectedPath.EndsWith("Bleach Soul Resurreccion", StringComparison.OrdinalIgnoreCase)
                               ? d.SelectedPath : Path.Combine(d.SelectedPath, "Bleach Soul Resurreccion");
        };
        pasta.TextChanged += delegate { MostrarEspaco(); };

        rJogo = Texto(1, "", 32, 322, 8.5f, true, Tema.VermelhoClaro);
        jogo = new TextBox { Location = new Point(32, 342), Size = new Size(470, 26), Font = new Font("Segoe UI", 10f), ReadOnly = true,
                             BackColor = Color.FromArgb(22, 22, 28), ForeColor = Color.White, BorderStyle = BorderStyle.FixedSingle };
        Controls.Add(jogo); paginas[1].Add(jogo);
        escolherJogo = Botao(1, 510, 339, 110, "j");
        escolherJogo.Click += delegate { EscolherJogo(); };
        estadoJogo = Paragrafo(1, 32, 372, 600, 20, 8.5f, Tema.TextoApagado);

        rFirmware = Texto(1, "", 32, 400, 8.5f, true, Tema.VermelhoClaro);
        firmware = new Grupo(this, 32, 420, 200, "", "sony", "", "arquivo");
        foreach (var o in firmware.Opcoes) paginas[1].Add(o);
        firmware.Valor = "sony";
        firmware.Mudou += delegate
        {
            if (firmware.Valor == "arquivo" && !EscolherPup()) firmware.Valor = "sony";
            if (firmware.Valor == "sony") escolhas.Pup = null;
            MostrarFirmware();
        };
        estadoFirmware = Paragrafo(1, 32, 458, 600, 20, 8.5f, Tema.TextoApagado);

        desktop = Marca(1, 32, 490);
        menu = Marca(1, 330, 490);
        info = Paragrafo(1, 32, 522, 600, 60, 8.5f, Tema.TextoApagado);
        voltar = Botao(1, 644, 420, 150, "v");
        voltar.Click += delegate { Mostrar(0); };

        // 2: progress.
        titulo2 = Texto(2, "", 32, 262, 8.5f, true, Tema.VermelhoClaro);
        for (int i = 0; i < Operacoes.Etapas.Length; i++) etapas.Add(NovaLinha(2, 288 + i * 22, 330, 300));
        barra = new ProgressBar { Location = new Point(32, 428), Size = new Size(588, 18), Maximum = 1000 };
        Controls.Add(barra); paginas[2].Add(barra);
        detalhe = Paragrafo(2, 32, 452, 600, 20, 8.5f, Tema.Texto);
        tempo = Texto(2, "", 32, 476, 8.5f, false, Tema.TextoApagado);
        notaTempo = Paragrafo(2, 32, 504, 760, 60, 8.5f, Tema.TextoApagado);
        cancelar = Botao(2, 644, 262, 150, "c");
        cancelar.Click += delegate { Close(); };

        // 3: done.
        titulo3 = Texto(3, "", 32, 262, 8.5f, true, Tema.VermelhoClaro);
        final = Paragrafo(3, 32, 286, 600, 150, 9f, Tema.Texto);
        abrir = Marca(3, 32, 446);

        string salvo = Opcoes.Ler()["jogo"];   // a reinstall or update already knows the dump
        Mostrar(0); AplicarIdioma();
        Shown += delegate
        {
            botao.Focus(); Verificar();
            if (!string.IsNullOrEmpty(salvo) && (Dump.EhIso(salvo) ? File.Exists(salvo) : Directory.Exists(salvo))) ValidarJogo(salvo);
        };
    }

    void Mostrar(int p)
    {
        pagina = p;
        for (int i = 0; i < paginas.Length; i++) foreach (var c in paginas[i]) c.Visible = i == p;
        foreach (var o in idioma.Opcoes) o.Visible = p < 2;
        botao.Visible = p != 2;
        if (p == 1 && !desinstalar) { MostrarEspaco(); MostrarFirmware(); }
        AtualizarBotao(); AplicarIdioma();
        Invalidate();
    }

    void AtualizarBotao()
    {
        botao.Enabled = pagina == 0 ? !pcBloqueado : pagina == 1 ? (desinstalar || jogoValido) : true;
    }

    void AplicarIdioma()
    {
        botao.Text = L.T(desinstalar ? (pagina == 3 ? "concluir" : "remover")
                         : pagina == 0 ? "continuar" : pagina == 1 ? "instalar" : "concluir");
        if (desinstalar)
        {
            desktop.Text = L.T("remover_dados"); if (pagina == 3) titulo3.Text = L.T("removido");
            foreach (Control c in Controls) c.Invalidate();
            return;
        }
        titulo0.Text = L.T("antes_titulo"); intro.Text = L.T("antes_texto"); notaRpcs3.Text = L.T("nota_rpcs3");
        foreach (var i in Itens) verificacao[i].Nome.Text = L.T(i);
        if (pagina == 0) Verificar(false);
        rPasta.Text = L.T("pasta_instalacao"); procurar.Text = L.T("procurar");
        rJogo.Text = L.T("seu_jogo"); escolherJogo.Text = L.T("escolher");
        rFirmware.Text = L.T("firmware_ps3"); firmware.Opcoes[0].Text = L.T("fw_baixar"); firmware.Opcoes[1].Text = L.T("fw_arquivo");
        desktop.Text = L.T("atalho_desktop"); menu.Text = L.T("atalho_menu"); voltar.Text = L.T("voltar");
        if (pagina == 1) { MostrarEspaco(); MostrarFirmware(); }
        titulo2.Text = L.T(cancelado ? "cancelando" : "instalando"); cancelar.Text = L.T("cancelar"); notaTempo.Text = L.T("nota_tempo");
        for (int i = 0; i < etapas.Count; i++) etapas[i].Nome.Text = L.T(Operacoes.Etapas[i]);
        titulo3.Text = L.T("concluida"); abrir.Text = L.T("abrir_agora");
        foreach (Control c in Controls) c.Invalidate();
    }

    // The PC check. comRede: also try the internet (in the background, a few seconds).
    void Verificar(bool comRede = true)
    {
        pcBloqueado = false;
        Action<string, bool, string, string> por = delegate(string id, bool ok, string sim, string nao)
        {
            verificacao[id].Por(ok ? "✓" : "✗", ok ? Ok : Ruim, ok ? sim : nao);
            if (!ok) pcBloqueado = true;
        };
        por("ver_windows", Sistema.Windows64(), "Windows " + Environment.OSVersion.Version.Major + " 64-bit", L.T("ver_windows_nao"));
        por("ver_video", Sistema.DirectX12(), "DirectX 12", L.T("ver_video_nao"));
        double gb = Sistema.MemoriaGb();
        verificacao["ver_memoria"].Por(gb >= 7.5 ? "✓" : "!", gb >= 7.5 ? Ok : Atencao, L.F(gb >= 7.5 ? "ver_memoria_ok" : "ver_memoria_pouca", gb));
        MostrarDisco();
        long falta = Rede.FaltaBaixar();
        if (falta == 0) verificacao["ver_ferramentas"].Por("✓", Ok, L.T("ja_baixado"));
        else verificacao["ver_ferramentas"].Por("↓", Baixar, L.F("sera_baixado", falta / 1e6));
        if (Firmware.Prx() != null) verificacao["ver_firmware"].Por("✓", Ok, L.T("ja_preparado"));
        else if (Rede.PupEmCache) verificacao["ver_firmware"].Por("✓", Ok, L.T("ja_baixado"));
        else verificacao["ver_firmware"].Por("↓", Baixar, L.T("fw_sera_baixado"));
        verificacao["ver_rpcs3"].Por("✓", Ok, L.T("incluido"));
        verificacao["ver_ffmpeg"].Por("✓", Ok, L.T("incluido"));
        if (comRede)
        {
            verificacao["ver_internet"].Por("…", Pendente, L.T("verificando"));
            var t = new System.Threading.Thread(() =>
            {
                bool online = Rede.Online();
                try { BeginInvoke((Action)delegate { internet = online ? 1 : 0; MostrarInternet(); }); } catch { }
            });
            t.IsBackground = true; t.Start();
        }
        else MostrarInternet();
        AtualizarBotao();
    }
    int internet = -1;   // -1 still checking, 0 offline, 1 online
    void MostrarInternet()
    {
        if (internet < 0) { verificacao["ver_internet"].Por("…", Pendente, L.T("verificando")); return; }
        bool precisa = Rede.FaltaBaixar() > 0 || (Firmware.Prx() == null && !Rede.PupEmCache);
        if (internet == 1) verificacao["ver_internet"].Por("✓", Ok, L.T("conectado"));
        else verificacao["ver_internet"].Por(precisa ? "✗" : "!", precisa ? Ruim : Atencao, L.T("sem_internet"));
    }
    void MostrarDisco()
    {
        try
        {
            var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(pasta.Text)));
            double livre = drive.AvailableFreeSpace / 1e9;
            bool ok = livre >= EspacoGb;
            verificacao["ver_disco"].Por(ok ? "✓" : "✗", ok ? Ok : Ruim, L.F("ver_disco_estado", EspacoGb, livre, drive.Name));
        }
        catch { verificacao["ver_disco"].Por("!", Atencao, ""); }
    }
    // Free space needed while installing: tools (~1 GB), generated code and objects (~2 GB), the game (~0.5 GB).
    const double EspacoGb = 4.0;

    void MostrarEspaco()
    {
        try
        {
            var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(pasta.Text)));
            info.Text = L.F("espaco", EspacoGb, drive.AvailableFreeSpace / 1e9) + "\n" + L.T("depois_espaco");
        }
        catch { info.Text = ""; }
    }

    void MostrarFirmware()
    {
        bool pronto = Firmware.Prx() != null;
        foreach (var o in firmware.Opcoes) o.Visible = pagina == 1 && !pronto;
        if (pronto) estadoFirmware.Text = "✓ " + L.T("ja_preparado");
        else if (firmware.Valor == "arquivo" && escolhas.Pup != null) estadoFirmware.Text = "✓ " + escolhas.Pup;
        else estadoFirmware.Text = L.T(Rede.PupEmCache ? "ja_baixado" : "fw_sony_explica");
    }

    void EscolherJogo()
    {
        using (var dlg = new OpenFileDialog { Title = "Bleach: Soul Resurrección (BLUS30769)", Filter = L.T("filtro_jogo"), CheckFileExists = true })
        {
            if (dlg.ShowDialog(this) != DialogResult.OK) return;
            ValidarJogo(Dump.EhIso(dlg.FileName) ? dlg.FileName : Path.GetDirectoryName(dlg.FileName));
        }
    }

    // Checks the chosen dump right away: BLUS30769 and the original EBOOT.
    void ValidarJogo(string escolhida)
    {
        jogoValido = false; escolhas.Jogo = null;
        string origem = Dump.EhIso(escolhida) ? escolhida : (Dump.AcharRaiz(escolhida) ?? escolhida);
        jogo.Text = origem;
        var dump = new Dump(origem);
        Cursor = Cursors.WaitCursor;
        string erro = null;
        try
        {
            string raiz = dump.Abrir();
            string bin = raiz == null ? null : Path.Combine(raiz, "PS3_GAME", "USRDIR", "EBOOT.BIN");
            if (raiz == null || !File.Exists(bin)) erro = dump.Iso && raiz == null ? L.T("iso_falhou") : L.T("sem_eboot");
            else if (!Dump.EhBleach(raiz)) erro = L.T("nao_bleach");
            else if (Util.Sha256(bin) != Dump.ShaEbootBin) erro = L.T("eboot_versao");
            else if (Arte.GuardarDoDump(raiz)) { fundo = Arte.Fundo(); Invalidate(); }
        }
        catch (Exception ex) { erro = ex.Message; }
        finally { dump.Fechar(); Cursor = Cursors.Default; }
        jogoValido = erro == null;
        if (jogoValido) escolhas.Jogo = origem;
        estadoJogo.Text = jogoValido ? "✓ BLUS30769" : "✗ " + erro;
        estadoJogo.ForeColor = jogoValido ? Ok : Ruim;
        AtualizarBotao();
    }

    bool EscolherPup()
    {
        using (var dlg = new OpenFileDialog { Title = L.T("titulo_pup"), Filter = L.T("filtro_pup"), CheckFileExists = true })
        {
            if (dlg.ShowDialog(this) != DialogResult.OK) return false;
            if (!Rede.PupValido(dlg.FileName)) { MessageBox.Show(this, L.T("pup_invalido"), Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return false; }
            escolhas.Pup = dlg.FileName; return true;
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        // The art is 16:9 like the window's upper part; the controls sit on a dark band below.
        g.DrawImage(fundo, new Rectangle(0, 0, ClientSize.Width, (int)(ClientSize.Width * 9f / 16f)));
        int topo = (int)((desinstalar ? 200 : 150) * K), solido = (int)((desinstalar ? 334 : 250) * K);
        var faixa = new Rectangle(0, topo, ClientSize.Width, solido - topo);
        using (var b = new LinearGradientBrush(new Rectangle(faixa.X, faixa.Y - 1, faixa.Width, faixa.Height + 2), Color.FromArgb(0, 0, 0, 0), Color.FromArgb(245, 6, 6, 10), 90f)) g.FillRectangle(b, faixa);
        using (var b = new SolidBrush(Color.FromArgb(245, 6, 6, 10))) g.FillRectangle(b, 0, solido, ClientSize.Width, ClientSize.Height);
        int linha = (int)((desinstalar ? 328 : 250) * K), x0 = (int)(30 * K);
        using (var pen = new Pen(Color.FromArgb(160, Tema.Vermelho), 2f * K)) g.DrawLine(pen, x0, linha, ClientSize.Width - x0, linha);
        using (var pen = new Pen(Color.FromArgb(70, Tema.Azul), K)) g.DrawLine(pen, x0, linha + 3 * K, ClientSize.Width - x0, linha + 3 * K);
        using (var pen = new Pen(Color.FromArgb(90, 90, 98), 1f)) g.DrawRectangle(pen, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
        base.OnPaint(e);
    }

    void Avancar()
    {
        if (pagina == 3)
        {
            if (!desinstalar && abrir.Checked)
                // Through Explorer, so the game runs as the player and not elevated like this installer.
                try { Process.Start("explorer.exe", "\"" + Path.Combine(escolhas.Destino, "Bleach Launcher.exe") + "\""); } catch { }
            Close(); return;
        }
        if (desinstalar) { Remover(); return; }
        if (pagina == 0) { Mostrar(1); return; }
        try { escolhas.Destino = Path.GetFullPath(pasta.Text.Trim()); }
        catch { MessageBox.Show(this, L.T("pasta_invalida"), Text); return; }
        escolhas.Desktop = desktop.Checked; escolhas.Menu = menu.Checked;
        Mostrar(2);
        foreach (var l in etapas) l.Por("○", Pendente, "");
        relogio.Start(); tique.Start();
        var t = new System.Threading.Thread(() =>
        {
            string erro = null;
            try { Operacoes.Instalar(escolhas, Progresso, () => cancelado); }
            catch (OperationCanceledException) { erro = ""; }
            catch (Exception ex) { erro = ex.Message; }
            BeginInvoke((Action)delegate
            {
                relogio.Stop(); tique.Stop();
                if (erro != null)
                {
                    if (etapaAtual >= 0) etapas[etapaAtual].Por("✗", Ruim, null);
                    pagina = -1;   // closing is allowed again
                    if (erro.Length > 0)
                        MessageBox.Show(this, L.T("falhou_instalar") + erro + "\n\n" + L.F("log_em", Operacoes.Log), Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
                    Close(); return;
                }
                Mostrar(3);
                final.Text = L.F("final_texto", escolhas.Destino, relogio.Elapsed.Hours * 60 + relogio.Elapsed.Minutes);
            });
        });
        t.IsBackground = true; t.Start();
    }

    void Progresso(int etapa, int valor, string texto)
    {
        BeginInvoke((Action)delegate
        {
            if (etapa != etapaAtual)
            {
                for (int i = 0; i < etapas.Count; i++)
                    if (i < etapa) etapas[i].Por("✓", Ok, "");
                    else if (i == etapa) etapas[i].Por("●", Tema.VermelhoClaro, "");
                etapaAtual = etapa;
            }
            barra.Value = Math.Max(0, Math.Min(1000, valor));
            if (!cancelado) detalhe.Text = texto;
        });
    }

    void Remover()
    {
        try { Operacoes.Remover(Path.GetDirectoryName(Application.ExecutablePath), desktop.Checked); }
        catch (Exception ex) { MessageBox.Show(this, L.T("nao_removeu") + ex.Message, Text); }
        desktop.Visible = false; info.Text = "";
        Mostrar(3);
    }

    public static float K = 1f;
}

static class ProgramaInstalador
{
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    [STAThread]
    static void Main(string[] args)
    {
        try { SetProcessDPIAware(); } catch { }
        using (var g = Graphics.FromHwnd(IntPtr.Zero)) JanelaInstalador.K = g.DpiX / 96f;
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        L.Idioma = "en";
        try { L.Idioma = File.ReadAllText(L.Arquivo(Path.GetDirectoryName(Application.ExecutablePath))).Trim(); } catch { }
        bool desinstalar = Pacote.Localizar() == null, silencioso = false;
        var e = new Escolhas();
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i].ToLowerInvariant(), v = i + 1 < args.Length ? args[i + 1] : null;
            if (a == "/desinstalar") desinstalar = true;
            else if (a == "/silencioso") silencioso = true;
            else if (a == "/sematalhos") e.Desktop = e.Menu = false;
            else if (v == null) continue;
            else if (a == "/pasta") { e.Destino = v; i++; }
            else if (a == "/jogo") { e.Jogo = v; i++; }
            else if (a == "/pup") { e.Pup = v; i++; }
            else if (a == "/idioma") { L.Idioma = v.ToLowerInvariant(); i++; }
            else if (a == "/dados") { Instalacao.Dados = Path.GetFullPath(v); i++; }   // tests: a separate data folder
        }
        // "/silencioso /jogo <iso or folder> [/pup <PS3UPDAT.PUP>] [/pasta <folder>] [/sematalhos]" installs with
        // no window (Program Files by default; progress in logs\instalacao-progresso.txt);
        // "/desinstalar /silencioso" removes the game, keeping the saves.
        if (silencioso)
        {
            try
            {
                if (desinstalar) Operacoes.Remover(Path.GetDirectoryName(Application.ExecutablePath), false);
                else
                {
                    if (e.Jogo == null) throw new Exception("/jogo <iso|pasta>");
                    e.Destino = Path.GetFullPath(e.Destino ?? Path.Combine(Environment.GetFolderPath(
                        Environment.SpecialFolder.ProgramFiles), "Bleach Soul Resurreccion"));
                    Directory.CreateDirectory(Instalacao.Logs);
                    using (var p = new StreamWriter(Path.Combine(Instalacao.Logs, "instalacao-progresso.txt"), false, Encoding.UTF8) { AutoFlush = true })
                    {
                        int ultimaEtapa = -1, ultimoValor = -100;
                        var relogio = Stopwatch.StartNew();
                        Operacoes.Instalar(e, (etapa, valor, texto) =>
                        {
                            lock (p)
                                if (etapa != ultimaEtapa || valor - ultimoValor >= 50 || valor == 1000)
                                {
                                    p.WriteLine("{0:hh\\:mm\\:ss} {1} {2,4} {3}", relogio.Elapsed, Operacoes.Etapas[etapa], valor, texto);
                                    ultimaEtapa = etapa; ultimoValor = valor;
                                }
                        }, () => false);
                        p.WriteLine("{0:hh\\:mm\\:ss} pronto", relogio.Elapsed);
                    }
                }
                Environment.Exit(0);
            }
            catch (Exception ex)
            {
                try { File.AppendAllText(Path.Combine(Instalacao.Logs, "instalacao-progresso.txt"), "ERRO: " + ex + "\r\n", Encoding.UTF8); } catch { }
                Environment.Exit(1);
            }
        }
        try { Application.Run(new JanelaInstalador(desinstalar)); }
        catch (Exception ex) { MessageBox.Show(L.T("erro") + ex.Message, Instalacao.Titulo, MessageBoxButtons.OK, MessageBoxIcon.Error); }
    }
}
