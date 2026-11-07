// Bleach: Soul Resurrección recomp launcher. Builds with the .NET Framework csc (C# 5), see compilar.sh.
// Starts bin\BleachRecomp.exe with the chosen resolution, FPS (= simulation rate: 30, 60, 120),
// display mode, aspect ratio and the user's own game dump. "--jogar [--fps N] [--tela T] [--resolucao R]
// [--proporcao P]" starts
// it straight away with the saved options (shortcuts, Steam, Playnite).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;

// Small modal wait box: runs the work off the UI thread, keeps the window responsive.
class Espera : Form
{
    public static T Rodar<T>(IWin32Window dono, string mensagem, Func<T> trabalho)
    {
        T resultado = default(T); Exception erro = null;
        using (var f = new Espera(mensagem))
        {
            f.Shown += delegate
            {
                var t = new System.Threading.Thread(() =>
                {
                    try { resultado = trabalho(); } catch (Exception ex) { erro = ex; }
                    f.BeginInvoke((Action)f.Close);
                });
                t.IsBackground = true; t.Start();
            };
            if (dono != null) f.ShowDialog(dono); else f.ShowDialog();
        }
        if (erro != null) throw erro;
        return resultado;
    }
    Espera(string mensagem)
    {
        Text = Instalacao.Titulo; FormBorderStyle = FormBorderStyle.FixedDialog; ControlBox = false;
        StartPosition = FormStartPosition.CenterParent; ShowInTaskbar = false;
        BackColor = Color.FromArgb(14, 14, 18); ForeColor = Tema.Texto;
        ClientSize = new Size((int)(460 * JanelaLauncher.K), (int)(110 * JanelaLauncher.K));
        var l = new Label { Text = mensagem, AutoSize = false, Dock = DockStyle.Top, Height = (int)(64 * JanelaLauncher.K),
                            TextAlign = ContentAlignment.MiddleCenter, Font = new Font("Segoe UI", 10f) };
        var b = new ProgressBar { Style = ProgressBarStyle.Marquee, Dock = DockStyle.Bottom, Height = (int)(18 * JanelaLauncher.K) };
        Controls.Add(l); Controls.Add(b);
        Padding = new Padding((int)(16 * JanelaLauncher.K));
    }
}

class JanelaLauncher : Form
{
    readonly Opcoes cfg;
    Image fundo;
    Grupo resolucao, fps, tela, idioma, texto, proporcao;
    Label rotuloJogo, rResolucao, rFps, rTela, rJogo, rTexto, rProporcao;
    Opcao trocar;
    Func<string> estado = () => "";   // the game line, rebuilt when the language changes
    BotaoJogar jogar;
    string raizJogo;   // the chosen dump: an .iso or a folder root
    bool elfPronto;
    Point arrasto;

    public JanelaLauncher(Opcoes opcoes)
    {
        cfg = opcoes;
        Text = Instalacao.Titulo;
        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(1104, 621);
        DoubleBuffered = true;
        BackColor = Color.Black;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
        fundo = Arte.Fundo();

        MontarControles();
        // Laid out at 96 DPI; fonts are in points and already follow the monitor DPI.
        Scale(new SizeF(K, K));
        ValidarJogo(cfg["jogo"], false);

        MouseDown += delegate(object o, MouseEventArgs e) { if (e.Button == MouseButtons.Left) arrasto = e.Location; };
        MouseMove += delegate(object o, MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left) Location = new Point(Location.X + e.X - arrasto.X, Location.Y + e.Y - arrasto.Y);
        };
        Shown += delegate
        {
            if (raizJogo == null)
            {
                MessageBox.Show(this, L.T("pedir_dump"), Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
                EscolherJogo();
            }
        };
    }

    void SalvarCfg()
    {
        cfg["resolucao"] = resolucao.Valor; cfg["fps"] = fps.Valor; cfg["tela"] = tela.Valor; cfg["idioma"] = L.Idioma;
        cfg["proporcao"] = proporcao.Valor;
        if (Traducao.AlgumDisponivel) cfg["texto"] = texto.Valor;
        if (raizJogo != null) cfg["jogo"] = raizJogo;
        cfg.Salvar();
    }

    Label Rotulo(string texto, int x, int y)
    {
        var l = new Label { Text = texto, AutoSize = true, BackColor = Color.Transparent, ForeColor = Tema.VermelhoClaro,
                            Font = new Font("Segoe UI", 8.5f, FontStyle.Bold), Location = new Point(x, y) };
        Controls.Add(l); return l;
    }

    void MontarControles()
    {
        const int y0 = 468;
        rResolucao = Rotulo("", 36, y0);
        resolucao = new Grupo(this, 36, y0 + 20, 68, "720p", "720p", "1080p", "1080p", "1440p", "1440p", "4K", "4K");
        rFps = Rotulo("", 352, y0);
        fps = new Grupo(this, 352, y0 + 20, 60, "30", "30", "60", "60", "120", "120");
        rTela = Rotulo("", 560, y0);
        tela = new Grupo(this, 560, y0 + 20, 104, "", "semborda", "", "janela");
        resolucao.Valor = cfg.Ou("resolucao", "4K"); fps.Valor = cfg.Ou("fps", "60"); tela.Valor = Execucao.Tela(cfg["tela"]);
        if (resolucao.Valor == null) resolucao.Valor = "4K";
        if (fps.Valor == null) fps.Valor = "60";
        if (tela.Valor == null) tela.Valor = "semborda";

        rJogo = Rotulo("", 36, y0 + 88);
        rotuloJogo = new Label { AutoSize = false, BackColor = Color.Transparent, ForeColor = Tema.Texto,
                                 Font = new Font("Segoe UI", 9.5f), Location = new Point(36, y0 + 108), Size = new Size(300, 22),
                                 AutoEllipsis = true };
        Controls.Add(rotuloJogo);
        trocar = new Opcao("", "jogo") { Location = new Point(346, y0 + 101), Width = 140 };
        trocar.Click += delegate { EscolherJogo(); };
        Controls.Add(trocar);

        // Monitor shape (ultrawide): wider 3D view, HUD and menus kept 16:9 in the centre.
        rProporcao = Rotulo("", 500, y0 + 81);
        proporcao = new Grupo(this, 500, y0 + 101, 40, "", "auto", "16:9", "16:9", "21:9", "21:9", "32:9", "32:9");
        proporcao.Valor = Execucao.Proporcao(cfg["proporcao"]);
        rProporcao.Visible = Execucao.Ultrawide;
        foreach (var o in proporcao.Opcoes) o.Visible = Execucao.Ultrawide;

        // The game's own text: English (the disc) or the translation shipped in traducao\.
        int xTexto = Execucao.Ultrawide ? 692 : 500;
        rTexto = Rotulo("", xTexto, y0 + 81);
        texto = new Grupo(this, xTexto, y0 + 101, 46, "EN", "en", "BR", "pt", "FR", "fr", "ES", "es");
        texto.Valor = Programa.TextoDoJogo(cfg);
        texto.Mudou += delegate { SalvarCfg(); };
        // Only the languages whose package is installed; English (the disc) whenever any is.
        rTexto.Visible = Traducao.AlgumDisponivel;
        foreach (var o in texto.Opcoes)
        {
            o.Visible = o.Valor == "en" ? Traducao.AlgumDisponivel : Traducao.Disponivel(o.Valor);
            if (o.Visible) { o.Left = xTexto; xTexto += o.Width + 6; }   // no gap for a missing package
        }

        jogar = new BotaoJogar { Location = new Point(900, y0 + 14), Size = new Size(176, 118) };
        jogar.Click += delegate { Jogar(); };
        Controls.Add(jogar);

        var fechar = new Opcao("✕", "x") { Location = new Point(1104 - 46, 10), Size = new Size(36, 30) };
        fechar.Font = new Font("Segoe UI", 10f);
        fechar.Click += delegate { SalvarCfg(); Close(); };
        Controls.Add(fechar);
        var minimizar = new Opcao("–", "min") { Location = new Point(1104 - 88, 10), Size = new Size(36, 30) };
        minimizar.Click += delegate { WindowState = FormWindowState.Minimized; };
        Controls.Add(minimizar);
        idioma = new Grupo(this, 1104 - 88 - 10 - 4 * 46, 10, 40, L.Nomes[0], L.Codigos[0], L.Nomes[1], L.Codigos[1],
                       L.Nomes[2], L.Codigos[2], L.Nomes[3], L.Codigos[3]);
        foreach (var o in idioma.Opcoes) { o.Height = 30; o.Font = new Font("Segoe UI Semibold", 9f); }
        idioma.Valor = L.Idioma;
        idioma.Mudou += delegate { L.Idioma = idioma.Valor; AplicarIdioma(); SalvarCfg(); };
        AplicarIdioma();
    }

    void AplicarIdioma()
    {
        rResolucao.Text = L.T("resolucao"); rFps.Text = L.T("fps"); rTela.Text = L.T("modo_tela"); rJogo.Text = L.T("jogo"); rTexto.Text = L.T("texto_jogo");
        tela.Opcoes[0].Text = L.T("sem_borda"); tela.Opcoes[1].Text = L.T("janela");
        rProporcao.Text = L.T("proporcao"); proporcao.Opcoes[0].Text = L.T("auto");
        trocar.Text = L.T("escolher_jogo"); jogar.Text = L.T("jogar");
        rotuloJogo.Text = estado();
        foreach (Control c in Controls) c.Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
        g.DrawImage(fundo, new Rectangle(0, 0, ClientSize.Width, ClientSize.Height));
        // Darkening band behind the controls (layout coordinates are at 96 DPI, times K).
        int topo = (int)(400 * K), x0 = (int)(36 * K), linha = (int)(458 * K);
        var faixa = new Rectangle(0, topo, ClientSize.Width, ClientSize.Height - topo);
        using (var b = new LinearGradientBrush(new Rectangle(faixa.X, faixa.Y - 1, faixa.Width, faixa.Height + 2), Color.FromArgb(0, 0, 0, 0), Color.FromArgb(235, 6, 6, 10), 90f))
            g.FillRectangle(b, faixa);
        using (var pen = new Pen(Color.FromArgb(160, Tema.Vermelho), 2f * K)) g.DrawLine(pen, x0, linha, ClientSize.Width - x0, linha);
        using (var pen = new Pen(Color.FromArgb(70, Tema.Azul), K)) g.DrawLine(pen, x0, linha + 3 * K, ClientSize.Width - x0, linha + 3 * K);
        using (var pen = new Pen(Color.FromArgb(90, 90, 98), 1f)) g.DrawRectangle(pen, 0, 0, ClientSize.Width - 1, ClientSize.Height - 1);
        base.OnPaint(e);
    }

    void Estado(Func<string> texto, Color cor) { estado = texto; rotuloJogo.Text = texto(); rotuloJogo.ForeColor = cor; AtualizarJogar(); }

    bool ValidarJogo(string escolhida, bool avisar)
    {
        raizJogo = null; elfPronto = false;
        if (string.IsNullOrEmpty(escolhida)) { Estado(() => L.T("nenhum_jogo"), Color.FromArgb(255, 150, 140)); return false; }
        var dump = new Dump(Dump.EhIso(escolhida) ? escolhida : (Dump.AcharRaiz(escolhida) ?? escolhida));
        Cursor = Cursors.WaitCursor;
        string raiz = dump.Abrir();
        Cursor = Cursors.Default;
        try
        {
            if (raiz == null || !File.Exists(Path.Combine(raiz, "PS3_GAME", "USRDIR", "EBOOT.BIN")))
            {
                Estado(() => L.T("nenhum_jogo"), Color.FromArgb(255, 150, 140));
                if (avisar) MessageBox.Show(this, dump.Iso && raiz == null ? L.T("iso_falhou") : L.T("sem_eboot"),
                    Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            if (!Dump.EhBleach(raiz))
            {
                Estado(() => dump.Origem + L.T("nao_blus"), Color.FromArgb(255, 150, 140));
                if (avisar) MessageBox.Show(this, L.T("nao_bleach"), Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return false;
            }
            raizJogo = dump.Origem;
            if (Arte.GuardarDoDump(raiz)) { fundo = Arte.Fundo(); Invalidate(); }
            elfPronto = Dump.ElfPronto();
            if (!elfPronto && avisar)
            {
                string erro = Espera.Rodar(this, L.T("preparando"), () => Dump.Decifrar(raiz));
                if (erro != null) MessageBox.Show(this, erro, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                elfPronto = erro == null;
            }
            bool pronto = elfPronto;
            Estado(() => dump.Origem + (pronto ? "   ✓ BLUS30769" : L.T("falta_eboot")),
                   elfPronto ? Color.FromArgb(150, 225, 160) : Color.FromArgb(255, 200, 120));
            return elfPronto;
        }
        finally { dump.Fechar(); }
    }

    void EscolherJogo()
    {
        using (var dlg = new OpenFileDialog { Title = "Bleach: Soul Resurrección (BLUS30769)",
                                              Filter = L.T("filtro_jogo"),
                                              CheckFileExists = true, AutoUpgradeEnabled = true })
        {
            try
            {
                if (raizJogo != null) dlg.InitialDirectory = Dump.EhIso(raizJogo) ? Path.GetDirectoryName(raizJogo)
                                                                                    : Path.Combine(raizJogo, "PS3_GAME");
            }
            catch { }
            if (dlg.ShowDialog(this) != DialogResult.OK) return;
            ValidarJogo(Dump.EhIso(dlg.FileName) ? dlg.FileName : Path.GetDirectoryName(dlg.FileName), true);
            SalvarCfg();
        }
    }

    void AtualizarJogar() { if (jogar != null) jogar.Enabled = raizJogo != null; }

    // Public build: the PS3 system fonts and font modules come from the official firmware file.
    public static string PrepararFirmware(IWin32Window dono)
    {
        string raiz = Firmware.Raiz();
        if (raiz != null) return raiz;
        var r = MessageBox.Show(dono, L.T("pedir_pup"), Instalacao.Titulo, MessageBoxButtons.YesNoCancel, MessageBoxIcon.Information);
        if (r == DialogResult.Cancel) return null;
        if (r == DialogResult.Yes)
            try { Process.Start(L.T("url_sony")); } catch { }
        using (var dlg = new OpenFileDialog { Title = L.T("titulo_pup"), Filter = L.T("filtro_pup"),
                                              CheckFileExists = true })
        {
            if (dlg.ShowDialog(dono) != DialogResult.OK) return null;
            string erro = Espera.Rodar(dono, L.T("extraindo"), () => Firmware.InstalarPup(dlg.FileName));
            if (erro != null) { MessageBox.Show(dono, erro, Instalacao.Titulo, MessageBoxButtons.OK, MessageBoxIcon.Warning); return null; }
        }
        return Firmware.Raiz();
    }

    void Jogar()
    {
        if (raizJogo == null) { EscolherJogo(); return; }
        if (!elfPronto && !ValidarJogo(raizJogo, true)) return;
        SalvarCfg();
        string firmware = PrepararFirmware(this);
        if (firmware == null) return;
        var dump = new Dump(raizJogo);
        Cursor = Cursors.WaitCursor;
        string raiz = dump.Abrir();
        Cursor = Cursors.Default;
        if (raiz == null) { MessageBox.Show(this, L.T("nao_abriu"), Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        string res = resolucao.Valor, f = fps.Valor, t = tela.Valor, tx = texto.Valor, pr = proporcao.Valor;
        Hide();
        var thread = new System.Threading.Thread(() =>
        {
            int codigo; string falha = null;
            try { codigo = Execucao.Rodar(raiz, firmware, res, f, t, Traducao.Preparar(raiz, tx), pr); }
            catch (Exception ex) { codigo = -1; falha = ex.Message; }
            dump.Fechar();
            BeginInvoke((Action)delegate
            {
                Show(); Activate();
                if (falha != null) MessageBox.Show(this, L.T("nao_iniciou") + falha, Text);
                else if (codigo != 0) Programa.AvisarErro(this);
            });
        });
        thread.IsBackground = true; thread.Start();
    }

    public static float K = 1f;   // monitor DPI / 96
}

static class Programa
{
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    public static void AvisarErro(IWin32Window dono)
    {
        MessageBox.Show(dono, L.T("erro_jogo") + Instalacao.Logs, Instalacao.Titulo,
            MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }

    // Any unexpected failure is written to the data folder and shown, never a silent exit.
    static void Falha(Exception ex)
    {
        try { Directory.CreateDirectory(Instalacao.Dados);
              File.WriteAllText(Path.Combine(Instalacao.Dados, "launcher-erro.txt"), DateTime.Now + "\r\n" + ex, Encoding.UTF8); } catch { }
        MessageBox.Show(L.T("erro_launcher") + ex.Message + L.T("detalhes_em") +
                        Path.Combine(Instalacao.Dados, "launcher-erro.txt"), Instalacao.Titulo, MessageBoxButtons.OK, MessageBoxIcon.Error);
    }

    // The game text chosen in the launcher; until then, the launcher's own language when the game has it.
    public static string TextoDoJogo(Opcoes cfg)
    {
        string t = cfg["texto"];
        if (t == "en" || Traducao.Disponivel(t)) return t;
        return Traducao.Disponivel(L.Idioma) ? L.Idioma : "en";
    }

    // "--jogar": start with the saved options (any --fps/--tela/--resolucao given replaces them),
    // no window, the process lasting as long as the game (Steam and Playnite track it). Anything
    // still missing (the dump, the firmware) opens the launcher instead.
    static int Direto(Opcoes cfg)
    {
        string jogo = cfg["jogo"];
        if (string.IsNullOrEmpty(jogo)) return -2;
        string firmware = Firmware.Raiz();
        if (firmware == null) return -2;
        var dump = new Dump(jogo);
        string raiz = dump.Abrir();
        try
        {
            if (raiz == null || !Dump.EhBleach(raiz)) return -2;
            if (!Dump.ElfPronto())
            {
                string erro = Dump.Decifrar(raiz);
                if (erro != null) { MessageBox.Show(erro, Instalacao.Titulo, MessageBoxButtons.OK, MessageBoxIcon.Warning); return 1; }
            }
            int codigo = Execucao.Rodar(raiz, firmware, cfg.Ou("resolucao", "4K"), cfg.Ou("fps", "60"), Execucao.Tela(cfg["tela"]),
                                        Traducao.Preparar(raiz, TextoDoJogo(cfg)), cfg["proporcao"]);
            dump.Fechar();
            if (codigo != 0) AvisarErro(null);
            return codigo;
        }
        finally { dump.Fechar(); }
    }

    [STAThread]   // file and folder dialogs are COM and need a single-threaded apartment
    static int Main(string[] args)
    {
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += delegate(object s, System.Threading.ThreadExceptionEventArgs e) { Falha(e.Exception); };
        AppDomain.CurrentDomain.UnhandledException += delegate(object s, UnhandledExceptionEventArgs e)
            { var ex = e.ExceptionObject as Exception; if (ex != null) Falha(ex); };
        // Crisp at high scaling (e.g. 4K at 250%) instead of a stretched bitmap.
        try { SetProcessDPIAware(); } catch { }
        using (var g = Graphics.FromHwnd(IntPtr.Zero)) JanelaLauncher.K = g.DpiX / 96f;
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        var cfg = Opcoes.Ler();
        // English unless the player picked another language (here or in the installer).
        string inicial = cfg["idioma"];
        if (inicial == null) try { inicial = File.ReadAllText(L.Arquivo(Instalacao.Pasta)).Trim(); } catch { }
        L.Idioma = inicial ?? "en";
        bool direto = false;
        for (int i = 0; i < args.Length; i++)
        {
            string a = args[i].ToLowerInvariant(), v = i + 1 < args.Length ? args[i + 1] : null;
            string vm = v == null ? null : v.ToLowerInvariant();
            if (vm == "br" || vm == "pt-br") vm = "pt";   // the launcher shows Portuguese as BR
            if (a == "--jogar" || a == "--play" || a == "--jugar") direto = true;
            else if (a == "--fps" && (v == "30" || v == "60" || v == "120")) { cfg["fps"] = v; i++; }
            else if ((a == "--texto" || a == "--text") && (vm == "en" || Array.IndexOf(Traducao.Idiomas, vm) >= 0)) { cfg["texto"] = vm; i++; }
            else if ((a == "--tela" || a == "--display") && vm != null)
            {
                // "fullscreen"/"exclusiva" (older shortcuts) now mean borderless fullscreen.
                string t = vm == "fullscreen" || vm == "exclusiva" || vm == "borderless" || vm == "semborda" ? "semborda"
                         : vm == "windowed" || vm == "janela" ? "janela" : null;
                if (t != null) cfg["tela"] = t;
                i++;
            }
            else if ((a == "--resolucao" || a == "--resolution") && v != null)
            {
                string r = v.ToUpperInvariant() == "4K" ? "4K" : v.ToLowerInvariant();
                if (r == "720p" || r == "1080p" || r == "1440p" || r == "4K") cfg["resolucao"] = r;
                i++;
            }
            else if ((a == "--proporcao" || a == "--aspect" || a == "--format") && vm != null)
            {
                if (Execucao.Proporcao(vm) == vm) cfg["proporcao"] = vm;
                i++;
            }
        }
        if (direto)
        {
            try { int codigo = Direto(cfg); if (codigo != -2) return codigo; }
            catch (Exception ex) { Falha(ex); return 1; }
        }
        Application.Run(new JanelaLauncher(cfg));
        return 0;
    }
}
