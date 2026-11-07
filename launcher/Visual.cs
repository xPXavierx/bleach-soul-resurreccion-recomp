// Bleach look shared by the launcher and the installer: colours, option pills, the big button.
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

// The window art. None of the game's art is shipped: until a dump is chosen it is drawn here; then
// it is the dump's own background picture (PS3_GAME\PIC1.PNG), kept in the data folder.
static class Arte
{
    static string Copia { get { return Path.Combine(Instalacao.Dados, "fundo.png"); } }

    public static Image Fundo()
    {
        try { if (File.Exists(Copia)) using (var s = new MemoryStream(File.ReadAllBytes(Copia))) return new Bitmap(Image.FromStream(s)); }
        catch { }
        return Desenhado();
    }

    // Copies the dump's PIC1.PNG once. True when the art changed.
    public static bool GuardarDoDump(string raizJogo)
    {
        try
        {
            if (File.Exists(Copia)) return false;
            string pic = Path.Combine(raizJogo, "PS3_GAME", "PIC1.PNG");
            if (!File.Exists(pic)) return false;
            Directory.CreateDirectory(Instalacao.Dados);
            File.Copy(pic, Copia, true);
            return true;
        }
        catch { return false; }
    }

    // Dark backdrop with a red slash and a blue edge, in the launcher's colours.
    static Image Desenhado()
    {
        var b = new Bitmap(1280, 720);
        using (var g = Graphics.FromImage(b))
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var f = new LinearGradientBrush(new Rectangle(0, 0, 1280, 720), Color.FromArgb(28, 10, 14), Color.FromArgb(6, 6, 10), 35f))
                g.FillRectangle(f, 0, 0, 1280, 720);
            var corte = new[] { new PointF(520, 720), new PointF(1010, 0), new PointF(1060, 0), new PointF(600, 720) };
            using (var f = new LinearGradientBrush(new Rectangle(500, 0, 600, 720), Color.FromArgb(150, Tema.Vermelho), Color.FromArgb(20, Tema.Vermelho), 90f))
                g.FillPolygon(f, corte);
            using (var p = new Pen(Color.FromArgb(160, Tema.Azul), 3f)) g.DrawLine(p, 600, 720, 1060, 0);
            using (var fonte = new Font("Segoe UI Black", 54f, FontStyle.Bold))
            using (var t = new SolidBrush(Color.FromArgb(235, 235, 235)))
                g.DrawString("BLEACH", fonte, t, 70, 150);
            using (var fonte = new Font("Segoe UI Semibold", 26f))
            using (var t = new SolidBrush(Tema.VermelhoClaro))
                g.DrawString("SOUL RESURRECCIÓN  ·  PC RECOMP", fonte, t, 76, 250);
        }
        return b;
    }
}

static class Tema
{
    public static readonly Color Vermelho = Color.FromArgb(200, 22, 30);
    public static readonly Color VermelhoClaro = Color.FromArgb(235, 52, 52);
    public static readonly Color Azul = Color.FromArgb(70, 170, 245);
    public static readonly Color Texto = Color.FromArgb(240, 240, 240);
    public static readonly Color TextoApagado = Color.FromArgb(160, 160, 165);

    public static GraphicsPath Arredondado(RectangleF r, float raio)
    {
        var p = new GraphicsPath();
        float d = raio * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }
}

// Selectable option ("pill"). Selected: Bleach red with a blue underline.
class Opcao : Control
{
    public string Valor;
    bool selecionada, sobre;
    public bool Selecionada { get { return selecionada; } set { selecionada = value; Invalidate(); } }

    public Opcao(string texto, string valor)
    {
        Text = texto; Valor = valor;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                 ControlStyles.UserPaint | ControlStyles.SupportsTransparentBackColor | ControlStyles.ResizeRedraw, true);
        BackColor = Color.Transparent;
        Font = new Font("Segoe UI Semibold", 10.5f);
        Cursor = Cursors.Hand;
        Size = new Size(80, 34);
    }
    protected override void OnMouseEnter(EventArgs e) { sobre = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { sobre = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
        var r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 4f);
        using (var path = Tema.Arredondado(r, 6))
        {
            if (selecionada)
            {
                using (var b = new LinearGradientBrush(r, Tema.VermelhoClaro, Tema.Vermelho, 90f)) g.FillPath(b, path);
                using (var pen = new Pen(Color.FromArgb(255, 120, 110), 1f)) g.DrawPath(pen, path);
            }
            else
            {
                using (var b = new SolidBrush(Color.FromArgb(sobre ? 150 : 115, 12, 12, 16))) g.FillPath(b, path);
                using (var pen = new Pen(sobre ? Color.FromArgb(200, 200, 205) : Color.FromArgb(90, 90, 98), 1f)) g.DrawPath(pen, path);
            }
        }
        if (selecionada)
            using (var b = new SolidBrush(Tema.Azul)) g.FillRectangle(b, 10, Height - 3, Width - 20, 2.5f);
        // A longer translation shrinks to fit the pill instead of being cut.
        Font fonte = Font; bool propria = false;
        int largura = TextRenderer.MeasureText(Text, Font).Width, livre = Width - 10;
        if (largura > livre && largura > 0)
        { fonte = new Font(Font.FontFamily, Math.Max(7f, Font.Size * livre / largura), Font.Style); propria = true; }
        TextRenderer.DrawText(g, Text, fonte, new Rectangle(0, 0, Width, Height - 3),
            selecionada || sobre ? Color.White : Tema.Texto,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        if (propria) fonte.Dispose();
    }
}

// Group of mutually exclusive options under a caption.
class Grupo
{
    public readonly List<Opcao> Opcoes = new List<Opcao>();
    public event Action Mudou;
    public string Valor
    {
        get { foreach (var o in Opcoes) if (o.Selecionada) return o.Valor; return null; }
        set { bool achou = false; foreach (var o in Opcoes) achou |= o.Valor == value;
              if (!achou) return; foreach (var o in Opcoes) o.Selecionada = o.Valor == value; }
    }
    public Grupo(Control pai, int x, int y, int largura, params string[] textoValor)
    {
        for (int i = 0; i < textoValor.Length; i += 2)
        {
            var o = new Opcao(textoValor[i], textoValor[i + 1]);
            o.Location = new Point(x, y); o.Width = largura; x += largura + 6;
            o.Click += delegate { Valor = o.Valor; if (Mudou != null) Mudou(); };
            pai.Controls.Add(o); Opcoes.Add(o);
        }
    }
}

class BotaoJogar : Control
{
    bool sobre, pressionado;
    public BotaoJogar()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                 ControlStyles.UserPaint | ControlStyles.SupportsTransparentBackColor | ControlStyles.ResizeRedraw, true);
        BackColor = Color.Transparent; Cursor = Cursors.Hand;
        Font = new Font("Segoe UI Black", 20f, FontStyle.Bold);
    }
    protected override void OnMouseEnter(EventArgs e) { sobre = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { sobre = false; pressionado = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { pressionado = true; Invalidate(); base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { pressionado = false; Invalidate(); base.OnMouseUp(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var r = new RectangleF(3, 3, Width - 7, Height - 7);
        using (var glow = Tema.Arredondado(new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f), 11))
        using (var pen = new Pen(Color.FromArgb(Enabled ? (sobre ? 230 : 150) : 60, Tema.Azul), 2.5f))
            g.DrawPath(pen, glow);
        using (var path = Tema.Arredondado(r, 9))
        {
            Color topo = Enabled ? (pressionado ? Tema.Vermelho : (sobre ? Color.FromArgb(250, 70, 64) : Tema.VermelhoClaro)) : Color.FromArgb(90, 70, 72);
            Color base_ = Enabled ? Color.FromArgb(150, 10, 18) : Color.FromArgb(60, 45, 48);
            using (var b = new LinearGradientBrush(r, topo, base_, 90f)) g.FillPath(b, path);
            using (var pen = new Pen(Color.FromArgb(120, 255, 255, 255), 1f)) g.DrawPath(pen, path);
        }
        TextRenderer.DrawText(g, Text, Font, Rectangle.Round(r), Enabled ? Color.White : Color.FromArgb(170, 170, 170),
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
    }
}

