// Interface languages of the launcher and the installer: English (the default), Brazilian Portuguese, French, Spanish.
using System;
using System.Collections.Generic;
using System.IO;

static class L
{
    public static readonly string[] Codigos = { "en", "pt", "fr", "es" };
    public static readonly string[] Nomes = { "EN", "BR", "FR", "ES" };   // "BR": Brazilian, not European, Portuguese
    static int atual;
    public static string Idioma
    {
        get { return Codigos[atual]; }
        set { int i = Array.IndexOf(Codigos, value); atual = i < 0 ? 0 : i; }
    }

    public static string T(string id)
    {
        string[] v;
        if (!Textos.TryGetValue(id, out v)) return id;
        return v[atual];
    }
    // Numbers as the chosen language writes them (0.4 GB, 0,4 GB).
    static readonly string[] Culturas = { "en-US", "pt-BR", "fr-FR", "es-ES" };
    public static string F(string id, params object[] args)
    { return string.Format(System.Globalization.CultureInfo.GetCultureInfo(Culturas[atual]), T(id), args); }
    public static string[] TodosOsIdiomas(string id) { string[] v; return Textos.TryGetValue(id, out v) ? v : new[] { id }; }

    // The language the installer was run in, kept in the install folder: the launcher starts in it
    // until the player picks another one.
    public static string Arquivo(string pasta) { return Path.Combine(pasta, "idioma.txt"); }

    // id -> { English, Português, Français, Español }
    static readonly Dictionary<string, string[]> Textos = new Dictionary<string, string[]>
    {
        // launcher
        { "resolucao", new[] { "RESOLUTION", "RESOLUÇÃO", "RÉSOLUTION", "RESOLUCIÓN" } },
        { "fps", new[] { "FPS", "FPS", "FPS", "FPS" } },
        { "modo_tela", new[] { "DISPLAY MODE", "MODO DE TELA", "MODE D'AFFICHAGE", "MODO DE PANTALLA" } },
        { "sem_borda", new[] { "Borderless", "Sem borda", "Sans bordure", "Sin bordes" } },
        { "janela", new[] { "Windowed", "Janela", "Fenêtré", "Ventana" } },
        { "proporcao", new[] { "ASPECT RATIO", "PROPORÇÃO", "FORMAT D'IMAGE", "RELACIÓN DE ASPECTO" } },
        { "auto", new[] { "Auto", "Auto", "Auto", "Auto" } },
        { "jogo", new[] { "GAME", "JOGO", "JEU", "JUEGO" } },
        { "escolher_jogo", new[] { "Choose game…", "Escolher jogo…", "Choisir le jeu…", "Elegir juego…" } },
        { "jogar", new[] { "PLAY", "JOGAR", "JOUER", "JUGAR" } },
        { "texto_jogo", new[] { "GAME TEXT", "TEXTO DO JOGO", "TEXTE DU JEU", "TEXTO DEL JUEGO" } },
        { "pedir_dump", new[] {
            "Choose your Bleach: Soul Resurrección (BLUS30769) dump: the .iso file or the PARAM.SFO in the game folder.",
            "Escolha o seu dump de Bleach: Soul Resurrección (BLUS30769): o arquivo .iso ou o PARAM.SFO da pasta do jogo.",
            "Choisissez votre dump de Bleach: Soul Resurrección (BLUS30769) : le fichier .iso ou le PARAM.SFO du dossier du jeu.",
            "Elige tu dump de Bleach: Soul Resurrección (BLUS30769): el archivo .iso o el PARAM.SFO de la carpeta del juego." } },
        { "nenhum_jogo", new[] { "No game chosen", "Nenhum jogo escolhido", "Aucun jeu choisi", "Ningún juego elegido" } },
        { "iso_falhou", new[] { "Could not mount that ISO.", "Não consegui montar essa ISO.", "Impossible de monter cette ISO.", "No se pudo montar esa ISO." } },
        { "sem_eboot", new[] { "PS3_GAME\\USRDIR\\EBOOT.BIN was not found in that dump.",
                               "Não encontrei PS3_GAME\\USRDIR\\EBOOT.BIN nesse dump.",
                               "PS3_GAME\\USRDIR\\EBOOT.BIN est introuvable dans ce dump.",
                               "No se encontró PS3_GAME\\USRDIR\\EBOOT.BIN en ese dump." } },
        { "nao_blus", new[] { "  —  not BLUS30769", "  —  não é BLUS30769", "  —  pas BLUS30769", "  —  no es BLUS30769" } },
        { "nao_bleach", new[] { "That dump is not the US Bleach: Soul Resurrección (BLUS30769).",
                                "Esse dump não é Bleach: Soul Resurrección americano (BLUS30769).",
                                "Ce dump n'est pas Bleach: Soul Resurrección américain (BLUS30769).",
                                "Ese dump no es Bleach: Soul Resurrección estadounidense (BLUS30769)." } },
        { "preparando", new[] { "Preparing the game from your dump (first time only)…",
                                "Preparando o jogo a partir do seu dump (só na primeira vez)…",
                                "Préparation du jeu à partir de votre dump (la première fois seulement)…",
                                "Preparando el juego a partir de tu dump (solo la primera vez)…" } },
        { "falta_eboot", new[] { "   — the EBOOT still has to be prepared", "   — falta preparar o EBOOT", "   — l'EBOOT doit encore être préparé", "   — falta preparar el EBOOT" } },
        { "filtro_jogo", new[] { "ISO or PARAM.SFO|*.iso;PARAM.SFO;EBOOT.BIN|All files|*.*",
                                 "ISO ou PARAM.SFO|*.iso;PARAM.SFO;EBOOT.BIN|Todos os arquivos|*.*",
                                 "ISO ou PARAM.SFO|*.iso;PARAM.SFO;EBOOT.BIN|Tous les fichiers|*.*",
                                 "ISO o PARAM.SFO|*.iso;PARAM.SFO;EBOOT.BIN|Todos los archivos|*.*" } },
        { "pedir_pup", new[] {
            "The game uses the PS3 system fonts, which come from Sony's official firmware (PS3UPDAT.PUP, a free download " +
            "from the PlayStation site). This is done only once.\n\nOpen Sony's download page now?\n(Yes: opens the page. " +
            "No: choose the file you already downloaded.)",
            "O jogo usa as fontes do sistema do PS3, que vêm do firmware oficial da Sony (arquivo PS3UPDAT.PUP, gratuito no " +
            "site da PlayStation). Isso é feito uma única vez.\n\nAbrir a página de download da Sony agora?\n(Sim: abre a " +
            "página. Não: escolher o arquivo já baixado.)",
            "Le jeu utilise les polices système de la PS3, issues du firmware officiel de Sony (PS3UPDAT.PUP, téléchargement gratuit sur le site PlayStation). Cette opération n'est faite qu'une fois.\n" +
            "\n" +
            "Ouvrir maintenant la page de téléchargement de Sony ?\n" +
            "(Oui : ouvre la page. Non : choisir le fichier déjà téléchargé.)",
            "El juego usa las fuentes del sistema de PS3, que vienen del firmware oficial de Sony (archivo PS3UPDAT.PUP, " +
            "gratuito en el sitio de PlayStation). Esto se hace una sola vez.\n\n¿Abrir ahora la página de descarga de Sony?\n" +
            "(Sí: abre la página. No: elegir el archivo ya descargado.)" } },
        { "url_sony", new[] { "https://www.playstation.com/en-us/support/hardware/ps3/system-software/",
                              "https://www.playstation.com/pt-br/support/hardware/ps3/system-software/",
                              "https://www.playstation.com/fr-fr/support/hardware/ps3/system-software/",
                              "https://www.playstation.com/es-es/support/hardware/ps3/system-software/" } },
        { "titulo_pup", new[] { "PS3 firmware (PS3UPDAT.PUP)", "Firmware do PS3 (PS3UPDAT.PUP)", "Firmware de la PS3 (PS3UPDAT.PUP)", "Firmware de PS3 (PS3UPDAT.PUP)" } },
        { "filtro_pup", new[] { "PS3 firmware|PS3UPDAT.PUP;*.PUP", "Firmware do PS3|PS3UPDAT.PUP;*.PUP", "Firmware de la PS3|PS3UPDAT.PUP;*.PUP", "Firmware de PS3|PS3UPDAT.PUP;*.PUP" } },
        { "extraindo", new[] { "Extracting the firmware fonts (first time only)…",
                               "Extraindo as fontes do firmware (só na primeira vez)…",
                               "Extraction des polices du firmware (la première fois seulement)…",
                               "Extrayendo las fuentes del firmware (solo la primera vez)…" } },
        { "nao_abriu", new[] { "Could not open the chosen game.", "Não consegui abrir o jogo escolhido.", "Impossible d'ouvrir le jeu choisi.", "No se pudo abrir el juego elegido." } },
        { "nao_iniciou", new[] { "Could not start the game: ", "Não consegui iniciar o jogo: ", "Impossible de lancer le jeu : ", "No se pudo iniciar el juego: " } },
        { "erro_jogo", new[] { "The game closed with an error. The details are in:\n", "O jogo encerrou com erro. Os detalhes ficam em:\n",
                               "Le jeu s'est fermé sur une erreur. Les détails se trouvent dans :\n",
                               "El juego se cerró con un error. Los detalles están en:\n" } },
        { "erro_launcher", new[] { "The launcher ran into an error:\n\n", "O launcher encontrou um erro:\n\n", "Le launcher a rencontré une erreur :\n\n", "El launcher encontró un error:\n\n" } },
        { "detalhes_em", new[] { "\n\nDetails in ", "\n\nDetalhes em ", "\n\nDétails dans ", "\n\nDetalles en " } },
        // core
        { "eboot_versao", new[] {
            "That dump's EBOOT.BIN is from another version of the game. This recomp was made from the original BLUS30769 release (no update).",
            "O EBOOT.BIN desse dump é de outra versão do jogo. Este recomp foi feito a partir da versão BLUS30769 original (sem atualização).",
            "L'EBOOT.BIN de ce dump provient d'une autre version du jeu. Ce recomp a été fait à partir de la version BLUS30769 d'origine (sans mise à jour).",
            "El EBOOT.BIN de ese dump es de otra versión del juego. Este recomp se hizo a partir de la versión BLUS30769 original (sin actualización)." } },
        { "sem_rpcs3", new[] { "The RPCS3 that comes with the game was not found (rpcs3 folder). Reinstall the game.",
                               "O RPCS3 que acompanha o jogo não foi encontrado (pasta rpcs3). Reinstale o jogo.",
                               "Le RPCS3 fourni avec le jeu est introuvable (dossier rpcs3). Réinstallez le jeu.",
                               "No se encontró el RPCS3 que viene con el juego (carpeta rpcs3). Reinstala el juego." } },
        { "eboot_falhou", new[] { "RPCS3 did not produce the expected decrypted EBOOT.", "O RPCS3 não gerou o EBOOT decifrado esperado.",
                                  "RPCS3 n'a pas produit l'EBOOT déchiffré attendu.",
                                  "RPCS3 no generó el EBOOT descifrado esperado." } },
        { "eboot_erro", new[] { "Could not decrypt the EBOOT: ", "Falha ao decifrar o EBOOT: ", "Impossible de déchiffrer l'EBOOT : ", "No se pudo descifrar el EBOOT: " } },
        { "fw_sem_modulo", new[] { "The firmware has no {0}.sprx.", "O firmware não tem {0}.sprx.", "Le firmware ne contient pas {0}.sprx.", "El firmware no tiene {0}.sprx." } },
        { "fw_nao_decifrou", new[] { "RPCS3 did not decrypt {0}.sprx.", "O RPCS3 não decifrou {0}.sprx.", "RPCS3 n'a pas déchiffré {0}.sprx.", "RPCS3 no descifró {0}.sprx." } },
        { "fw_diferente", new[] {
            "That firmware's {0} module differs from the one this recomp uses. Use the official PS3 firmware 4.90 or newer (PS3UPDAT.PUP).",
            "O módulo {0} desse firmware é diferente do que este recomp usa. Use o firmware oficial 4.90 ou mais novo do PS3 (PS3UPDAT.PUP).",
            "Le module {0} de ce firmware diffère de celui qu'utilise ce recomp. Utilisez le firmware officiel de la PS3 4.90 ou plus récent (PS3UPDAT.PUP).",
            "El módulo {0} de ese firmware es distinto del que usa este recomp. Usa el firmware oficial de PS3 4.90 o más nuevo (PS3UPDAT.PUP)." } },
        { "fw_sem_fontes", new[] { "The firmware has no system fonts (data\\font).", "O firmware não tem as fontes do sistema (data\\font).",
                                   "Le firmware ne contient pas les polices système (data\\font).",
                                   "El firmware no tiene las fuentes del sistema (data\\font)." } },
        { "fw_fontes_incompletas", new[] { "The system fonts were not copied completely.", "As fontes do sistema não foram copiadas por completo.",
                                           "Les polices système n'ont pas été copiées entièrement.",
                                           "Las fuentes del sistema no se copiaron por completo." } },
        { "fw_erro", new[] { "Could not prepare the firmware: ", "Falha ao preparar o firmware: ", "Impossible de préparer le firmware : ", "No se pudo preparar el firmware: " } },
        { "fw_nao_instalou", new[] { "RPCS3 did not install that firmware file.", "O RPCS3 não instalou esse arquivo de firmware.",
                                     "RPCS3 n'a pas installé ce fichier de firmware.",
                                     "RPCS3 no instaló ese archivo de firmware." } },
        // run options (shortcuts in the install folder)
        { "pasta_opcoes", new[] { "Launch options", "Opções de execução", "Options de lancement", "Opciones de ejecución" } },
        { "ultima_config", new[] { "Last settings", "Última configuração", "Derniers réglages", "Última configuración" } },
        { "leia_me", new[] { "ReadMe.txt", "Leia-me.txt", "LisezMoi.txt", "Léame.txt" } },
        { "leia_me_texto", new[] {
            "These shortcuts start the game straight away, without the launcher.\r\n\r\n" +
            "Steam (non-Steam game) or Playnite: point to\r\n  {0}\r\nwith the launch options:\r\n\r\n" +
            "  --play                                  the settings last chosen in the launcher\r\n" +
            "  --fps 30|60|120                         frame rate\r\n" +
            "  --display borderless|windowed           borderless fullscreen or a window\r\n" +
            "  --resolution 720p|1080p|1440p|4K       internal resolution\r\n" +
            "  --aspect auto|16:9|21:9|32:9            monitor shape (ultrawide: wider view, HUD kept 16:9)\r\n" +
            "  --text en|br|fr|es                      the game's text: English, Brazilian Portuguese, French, Spanish\r\n\r\n" +
            "Example: --play --fps 60 --display borderless --resolution 1440p\r\n" +
            "Anything not given uses the launcher's last settings.\r\n",
            "Estes atalhos abrem o jogo direto, sem passar pelo launcher.\r\n\r\n" +
            "Steam (jogo não Steam) ou Playnite: aponte para\r\n  {0}\r\ncom os argumentos de inicialização:\r\n\r\n" +
            "  --jogar                                 a última configuração escolhida no launcher\r\n" +
            "  --fps 30|60|120                         taxa de quadros\r\n" +
            "  --tela semborda|janela                  tela cheia sem borda ou janela\r\n" +
            "  --resolucao 720p|1080p|1440p|4K         resolução interna\r\n" +
            "  --proporcao auto|16:9|21:9|32:9         formato do monitor (ultrawide: visão mais larga, HUD em 16:9)\r\n" +
            "  --texto en|br|fr|es                     texto do jogo: inglês, português do Brasil, francês, espanhol\r\n\r\n" +
            "Exemplo: --jogar --fps 60 --tela semborda --resolucao 1440p\r\n" +
            "O que não for informado usa a última configuração do launcher.\r\n",
            "Ces raccourcis lancent le jeu directement, sans passer par le launcher.\r\n\r\n" +
            "Steam (jeu non Steam) ou Playnite : pointez vers\r\n  {0}\r\navec les options de lancement :\r\n\r\n" +
            "  --play                                  les derniers réglages choisis dans le launcher\r\n" +
            "  --fps 30|60|120                         fréquence d'images\r\n" +
            "  --display borderless|windowed           plein écran sans bordure ou fenêtre\r\n" +
            "  --resolution 720p|1080p|1440p|4K       résolution interne\r\n" +
            "  --aspect auto|16:9|21:9|32:9            format de l'écran (ultrawide : vue plus large, HUD en 16:9)\r\n" +
            "  --text en|br|fr|es                      texte du jeu : anglais, portugais du Brésil, français, espagnol\r\n\r\n" +
            "Exemple : --play --fps 60 --display borderless --resolution 1440p\r\n" +
            "Ce qui n'est pas indiqué reprend les derniers réglages du launcher.\r\n",
            "Estos accesos directos abren el juego directamente, sin pasar por el launcher.\r\n\r\n" +
            "Steam (juego que no es de Steam) o Playnite: apunta a\r\n  {0}\r\ncon las opciones de inicio:\r\n\r\n" +
            "  --play                                  la última configuración elegida en el launcher\r\n" +
            "  --fps 30|60|120                         fotogramas por segundo\r\n" +
            "  --display borderless|windowed           pantalla completa sin bordes o ventana\r\n" +
            "  --resolution 720p|1080p|1440p|4K       resolución interna\r\n" +
            "  --aspect auto|16:9|21:9|32:9            formato del monitor (ultrawide: vista más amplia, HUD en 16:9)\r\n" +
            "  --text en|br|fr|es                      texto del juego: inglés, portugués de Brasil, francés, español\r\n\r\n" +
            "Ejemplo: --play --fps 60 --display borderless --resolution 1440p\r\n" +
            "Lo que no se indique usa la última configuración del launcher.\r\n" } },
        // installer
        { "pasta_instalacao", new[] { "INSTALL FOLDER", "PASTA DE INSTALAÇÃO", "DOSSIER D'INSTALLATION", "CARPETA DE INSTALACIÓN" } },
        { "procurar", new[] { "Browse…", "Procurar…", "Parcourir…", "Examinar…" } },
        { "atalho_desktop", new[] { "Create a desktop shortcut", "Criar atalho na Área de Trabalho", "Créer un raccourci sur le Bureau", "Crear acceso directo en el escritorio" } },
        { "atalho_menu", new[] { "Create a Start menu shortcut", "Criar atalho no Menu Iniciar", "Créer un raccourci dans le menu Démarrer", "Crear acceso directo en el menú Inicio" } },
        { "abrir_agora", new[] { "Launch the game now", "Abrir o jogo agora", "Lancer le jeu maintenant", "Abrir el juego ahora" } },
        { "instalar", new[] { "INSTALL", "INSTALAR", "INSTALLER", "INSTALAR" } },
        { "remover", new[] { "REMOVE", "REMOVER", "SUPPRIMER", "QUITAR" } },
        { "remover_dados", new[] { "Also remove the saves and settings", "Remover também os saves e as configurações",
                                   "Supprimer aussi les sauvegardes et les réglages",
                                   "Quitar también las partidas guardadas y la configuración" } },
        { "espaco", new[] { "Space required: {0:0.0} GB      Available: {1:0.0} GB", "Espaço necessário: {0:0.0} GB      Disponível: {1:0.0} GB",
                            "Espace nécessaire : {0:0.0} Go      Disponible : {1:0.0} Go",
                            "Espacio necesario: {0:0.0} GB      Disponible: {1:0.0} GB" } },
        { "pasta_invalida", new[] { "Invalid folder.", "Pasta inválida.", "Dossier non valide.", "Carpeta no válida." } },
        { "falhou_instalar", new[] { "The installation failed: ", "A instalação falhou: ", "L'installation a échoué : ", "La instalación falló: " } },
        { "instalando", new[] { "INSTALLING", "INSTALANDO", "INSTALLATION EN COURS", "INSTALANDO" } },
        { "concluida", new[] { "INSTALLATION COMPLETE", "INSTALAÇÃO CONCLUÍDA", "INSTALLATION TERMINÉE", "INSTALACIÓN COMPLETADA" } },
        { "concluir", new[] { "FINISH", "CONCLUIR", "TERMINER", "FINALIZAR" } },
        { "removido", new[] { "GAME REMOVED", "JOGO REMOVIDO", "JEU SUPPRIMÉ", "JUEGO QUITADO" } },
        { "nao_removeu", new[] { "Could not remove everything: ", "Não consegui remover tudo: ", "Impossible de tout supprimer : ", "No se pudo quitar todo: " } },
        { "desinstalar", new[] { "Uninstall", "Desinstalar", "Désinstaller", "Desinstalar" } },
        { "incompleto", new[] { "this installer is incomplete (no game files).", "este instalador está incompleto (sem os arquivos do jogo).",
                                "ce programme d'installation est incomplet (sans les fichiers du jeu).",
                                "este instalador está incompleto (sin los archivos del juego)." } },
        // installer: PC check, choices, progress (Compilacao.cs)
        { "continuar", new[] { "CONTINUE", "CONTINUAR", "CONTINUER", "CONTINUAR" } },
        { "voltar", new[] { "← Back", "← Voltar", "← Retour", "← Volver" } },
        { "cancelar", new[] { "Cancel", "Cancelar", "Annuler", "Cancelar" } },
        { "cancelar_pergunta", new[] { "Cancel the installation? What was already downloaded is kept for the next try.",
                                       "Cancelar a instalação? O que já foi baixado fica guardado para a próxima tentativa.",
                                       "Annuler l'installation ? Ce qui a déjà été téléchargé est conservé pour la prochaine fois.",
                                       "¿Cancelar la instalación? Lo que ya se descargó se guarda para el próximo intento." } },
        { "cancelando", new[] { "CANCELLING…", "CANCELANDO…", "ANNULATION…", "CANCELANDO…" } },
        { "antes_titulo", new[] { "BEFORE INSTALLING", "ANTES DE INSTALAR", "AVANT L'INSTALLATION", "ANTES DE INSTALAR" } },
        { "antes_texto", new[] {
            "This installer does not contain the game. It uses YOUR own copy of Bleach: Soul Resurrección (BLUS30769) and recompiles it to run natively on this PC. This is what it needs:",
            "Este instalador não contém o jogo. Ele usa a SUA cópia de Bleach: Soul Resurrección (BLUS30769) e a recompila para rodar nativamente neste PC. Veja o que ele precisa:",
            "Ce programme d'installation ne contient pas le jeu. Il utilise VOTRE copie de Bleach: Soul Resurrección (BLUS30769) et la recompile pour fonctionner nativement sur ce PC. Voici ce dont il a besoin :",
            "Este instalador no contiene el juego. Usa TU copia de Bleach: Soul Resurrección (BLUS30769) y la recompila para que funcione de forma nativa en este PC. Esto es lo que necesita:" } },
        { "nota_rpcs3", new[] {
            "About RPCS3: it is the open-source PS3 emulator (GPL). It comes with this installer and is used only to decrypt the EBOOT of your disc and to read Sony's firmware file. The game does not run on it: it runs as a native Windows program.",
            "Sobre o RPCS3: é o emulador de PS3 de código aberto (GPL). Ele vem junto com este instalador e é usado só para decifrar o EBOOT do seu disco e ler o arquivo de firmware da Sony. O jogo não roda nele: roda como um programa nativo do Windows.",
            "À propos de RPCS3 : c'est l'émulateur PS3 open source (GPL). Il est fourni avec ce programme d'installation et sert uniquement à déchiffrer l'EBOOT de votre disque et à lire le fichier de firmware de Sony. Le jeu ne tourne pas dessus : il fonctionne comme un programme Windows natif.",
            "Sobre RPCS3: es el emulador de PS3 de código abierto (GPL). Viene con este instalador y se usa solo para descifrar el EBOOT de tu disco y leer el archivo de firmware de Sony. El juego no se ejecuta en él: funciona como un programa nativo de Windows." } },
        { "ver_windows", new[] { "Windows 10/11, 64-bit", "Windows 10/11, 64 bits", "Windows 10/11, 64 bits", "Windows 10/11, 64 bits" } },
        { "ver_windows_nao", new[] { "required: Windows 10 or 11, 64-bit", "necessário: Windows 10 ou 11, 64 bits", "requis : Windows 10 ou 11, 64 bits", "necesario: Windows 10 u 11, 64 bits" } },
        { "ver_video", new[] { "DirectX 12 graphics card", "Placa de vídeo DirectX 12", "Carte graphique DirectX 12", "Tarjeta gráfica DirectX 12" } },
        { "ver_video_nao", new[] { "not found: update the graphics driver", "não encontrada: atualize o driver de vídeo", "introuvable : mettez à jour le pilote graphique", "no encontrada: actualiza el controlador de vídeo" } },
        { "ver_memoria", new[] { "Memory (RAM)", "Memória (RAM)", "Mémoire (RAM)", "Memoria (RAM)" } },
        { "ver_memoria_ok", new[] { "{0:0} GB", "{0:0} GB", "{0:0} Go", "{0:0} GB" } },
        { "ver_memoria_pouca", new[] { "{0:0} GB: may work, 8 GB recommended", "{0:0} GB: pode funcionar, recomendado 8 GB",
                                       "{0:0} Go : peut fonctionner, 8 Go conseillés", "{0:0} GB: puede funcionar, se recomiendan 8 GB" } },
        { "ver_disco", new[] { "Free disk space", "Espaço livre em disco", "Espace disque libre", "Espacio libre en disco" } },
        { "ver_disco_estado", new[] { "{0:0} GB needed, {1:0.0} GB free on {2}", "precisa de {0:0} GB, {1:0.0} GB livres em {2}", "{0:0} Go nécessaires, {1:0.0} Go libres sur {2}", "necesita {0:0} GB, {1:0.0} GB libres en {2}" } },
        { "ver_internet", new[] { "Internet connection", "Conexão com a internet", "Connexion Internet", "Conexión a internet" } },
        { "verificando", new[] { "checking…", "verificando…", "vérification…", "comprobando…" } },
        { "conectado", new[] { "connected", "conectado", "connecté", "conectado" } },
        { "sem_internet", new[] { "offline: needed to download what is missing", "sem conexão: necessária para baixar o que falta", "hors ligne : nécessaire pour télécharger ce qui manque", "sin conexión: necesaria para descargar lo que falta" } },
        { "ver_ferramentas", new[] { "Build tools (Python, CMake, Ninja, LLVM)", "Ferramentas de compilação (Python, CMake, Ninja, LLVM)",
                                     "Outils de compilation (Python, CMake, Ninja, LLVM)", "Herramientas de compilación (Python, CMake, Ninja, LLVM)" } },
        { "sera_baixado", new[] { "will be downloaded ({0:0} MB, official sites)", "serão baixadas ({0:0} MB, sites oficiais)", "seront téléchargés ({0:0} Mo, sites officiels)", "se descargarán ({0:0} MB, sitios oficiales)" } },
        { "ja_baixado", new[] { "already downloaded", "já baixado", "déjà téléchargé", "ya descargado" } },
        { "ja_preparado", new[] { "already prepared", "já preparado", "déjà préparé", "ya preparado" } },
        { "ver_firmware", new[] { "Official PS3 firmware (fonts)", "Firmware oficial do PS3 (fontes)", "Firmware officiel de la PS3 (polices)", "Firmware oficial de PS3 (fuentes)" } },
        { "fw_sera_baixado", new[] { "from Sony's server (~200 MB) or your own file", "do servidor da Sony (~200 MB) ou do seu arquivo", "depuis le serveur de Sony (~200 Mo) ou votre fichier", "del servidor de Sony (~200 MB) o tu archivo" } },
        { "ver_rpcs3", new[] { "RPCS3 (see below)", "RPCS3 (veja abaixo)", "RPCS3 (voir ci-dessous)", "RPCS3 (ver abajo)" } },
        { "ver_ffmpeg", new[] { "FFmpeg (game videos)", "FFmpeg (vídeos do jogo)", "FFmpeg (vidéos du jeu)", "FFmpeg (vídeos del juego)" } },
        { "incluido", new[] { "included in this installer", "incluído neste instalador", "inclus dans ce programme d'installation", "incluido en este instalador" } },
        { "seu_jogo", new[] { "YOUR GAME: THE .ISO OR THE DUMP FOLDER (BLUS30769)", "SEU JOGO: O .ISO OU A PASTA DO DUMP (BLUS30769)",
                              "VOTRE JEU : L'.ISO OU LE DOSSIER DU DUMP (BLUS30769)", "TU JUEGO: EL .ISO O LA CARPETA DEL DUMP (BLUS30769)" } },
        { "escolher", new[] { "Choose…", "Escolher…", "Choisir…", "Elegir…" } },
        { "firmware_ps3", new[] { "OFFICIAL PS3 FIRMWARE (PS3UPDAT.PUP)", "FIRMWARE OFICIAL DO PS3 (PS3UPDAT.PUP)", "FIRMWARE OFFICIEL DE LA PS3 (PS3UPDAT.PUP)", "FIRMWARE OFICIAL DE PS3 (PS3UPDAT.PUP)" } },
        { "fw_baixar", new[] { "Download from Sony", "Baixar da Sony", "Télécharger chez Sony", "Descargar de Sony" } },
        { "fw_arquivo", new[] { "I have the file…", "Já tenho o arquivo…", "J'ai le fichier…", "Ya tengo el archivo…" } },
        { "fw_sony_explica", new[] { "Downloaded from Sony's official PS3 update server; only the system fonts are used.",
                                     "Baixado do servidor oficial de atualizações do PS3 da Sony; só as fontes do sistema são usadas.",
                                     "Téléchargé depuis le serveur officiel de mises à jour PS3 de Sony ; seules les polices système sont utilisées.",
                                     "Se descarga del servidor oficial de actualizaciones de PS3 de Sony; solo se usan las fuentes del sistema." } },
        { "pup_invalido", new[] { "That is not a PS3 firmware file (PS3UPDAT.PUP).", "Esse arquivo não é um firmware do PS3 (PS3UPDAT.PUP).", "Ce fichier n'est pas un firmware de la PS3 (PS3UPDAT.PUP).", "Ese archivo no es un firmware de PS3 (PS3UPDAT.PUP)." } },
        { "pup_falhou", new[] { "Could not download the firmware from Sony ({0}). Download PS3UPDAT.PUP from playstation.com and choose \"I have the file\".",
                                "Não consegui baixar o firmware da Sony ({0}). Baixe o PS3UPDAT.PUP em playstation.com e escolha \"Já tenho o arquivo\".",
                                "Impossible de télécharger le firmware chez Sony ({0}). Téléchargez PS3UPDAT.PUP sur playstation.com et choisissez « J'ai le fichier ».",
                                "No se pudo descargar el firmware de Sony ({0}). Descarga PS3UPDAT.PUP en playstation.com y elige \"Ya tengo el archivo\"." } },
        { "depois_espaco", new[] { "After installing, the game takes about 0.4 GB; the build tools are removed.", "Depois de instalado, o jogo ocupa cerca de 0,4 GB; as ferramentas de compilação são apagadas.",
                                   "Une fois installé, le jeu occupe environ 0,4 Go ; les outils de compilation sont supprimés.", "Una vez instalado, el juego ocupa unos 0,4 GB; las herramientas de compilación se borran." } },
        { "etapa_copiar", new[] { "Copying the program files", "Copiando os arquivos do programa", "Copie des fichiers du programme", "Copiando los archivos del programa" } },
        { "etapa_eboot", new[] { "Decrypting your game's EBOOT (RPCS3)", "Decifrando o EBOOT do seu jogo (RPCS3)", "Déchiffrement de l'EBOOT de votre jeu (RPCS3)", "Descifrando el EBOOT de tu juego (RPCS3)" } },
        { "etapa_firmware", new[] { "PS3 firmware fonts", "Fontes do firmware do PS3", "Polices du firmware PS3", "Fuentes del firmware de PS3" } },
        { "etapa_ferramentas", new[] { "Downloading the build tools", "Baixando as ferramentas de compilação", "Téléchargement des outils de compilation", "Descargando las herramientas de compilación" } },
        { "etapa_recompilar", new[] { "Recompiling the game for this PC", "Recompilando o jogo para este PC", "Recompilation du jeu pour ce PC", "Recompilando el juego para este PC" } },
        { "etapa_finalizar", new[] { "Cleaning up and creating shortcuts", "Limpando e criando atalhos", "Nettoyage et création des raccourcis", "Limpiando y creando accesos directos" } },
        { "baixando_pup", new[] { "Downloading PS3UPDAT.PUP from Sony", "Baixando PS3UPDAT.PUP da Sony", "Téléchargement de PS3UPDAT.PUP chez Sony", "Descargando PS3UPDAT.PUP de Sony" } },
        { "instalando_pup", new[] { "Reading the firmware with RPCS3 (its window shows up for a few minutes; do not close it)…", "Lendo o firmware com o RPCS3 (a janela dele aparece por alguns minutos; não a feche)…", "Lecture du firmware avec RPCS3 (sa fenêtre s'affiche quelques minutes ; ne la fermez pas)…", "Leyendo el firmware con RPCS3 (su ventana aparece unos minutos; no la cierres)…" } },
        { "baixando", new[] { "Downloading {0}", "Baixando {0}", "Téléchargement de {0}", "Descargando {0}" } },
        { "extraindo_ferramenta", new[] { "Unpacking {0}…", "Descompactando {0}…", "Décompression de {0}…", "Descomprimiendo {0}…" } },
        { "mb_de", new[] { "{0:0} of {1:0} MB", "{0:0} de {1:0} MB", "{0:0} sur {1:0} Mo", "{0:0} de {1:0} MB" } },
        { "mb", new[] { "{0:0} MB", "{0:0} MB", "{0:0} Mo", "{0:0} MB" } },
        { "sub_1", new[] { "Translating the PS3 code to C++…", "Traduzindo o código do PS3 para C++…", "Traduction du code PS3 en C++…", "Traduciendo el código de PS3 a C++…" } },
        { "sub_2", new[] { "Applying the recomp's fixes…", "Aplicando as correções do recomp…", "Application des corrections du recomp…", "Aplicando las correcciones del recomp…" } },
        { "sub_3", new[] { "Translating the SPU programs…", "Traduzindo os programas da SPU…", "Traduction des programmes SPU…", "Traduciendo los programas de la SPU…" } },
        { "sub_4", new[] { "Translating the firmware font modules…", "Traduzindo os módulos de fonte do firmware…", "Traduction des modules de police du firmware…", "Traduciendo los módulos de fuentes del firmware…" } },
        { "sub_5", new[] { "Preparing the build…", "Preparando a compilação…", "Préparation de la compilation…", "Preparando la compilación…" } },
        { "sub_7", new[] { "Finishing the executable…", "Finalizando o executável…", "Finalisation de l'exécutable…", "Finalizando el ejecutable…" } },
        { "compilando", new[] { "Compiling: {0} of {1} files", "Compilando: {0} de {1} arquivos", "Compilation : {0} fichiers sur {1}", "Compilando: {0} de {1} archivos" } },
        { "limpando", new[] { "Removing the build tools…", "Apagando as ferramentas de compilação…", "Suppression des outils de compilation…", "Borrando las herramientas de compilación…" } },
        { "tempo", new[] { "Elapsed: {0}:{1:00}:{2:00}", "Tempo decorrido: {0}:{1:00}:{2:00}", "Temps écoulé : {0}:{1:00}:{2:00}", "Tiempo transcurrido: {0}:{1:00}:{2:00}" } },
        { "nota_tempo", new[] {
            "Recompiling uses the whole processor and takes about 3 to 20 minutes, depending on the processor. You can keep using the computer, it will just be slower. This is done only once.",
            "A recompilação usa todo o processador e leva de 3 a 20 minutos, dependendo do processador. Você pode continuar usando o computador, só que ele ficará mais lento. Isso é feito uma única vez.",
            "La recompilation utilise tout le processeur et prend de 3 à 20 minutes selon le processeur. Vous pouvez continuer à utiliser l'ordinateur, il sera seulement plus lent. Cela n'est fait qu'une fois.",
            "La recompilación usa todo el procesador y tarda de 3 a 20 minutos, según el procesador. Puedes seguir usando el ordenador, solo que irá más lento. Esto se hace una sola vez." } },
        { "final_texto", new[] {
            "The game was recompiled and installed in\n{0}\n(in {1} min).\n\nOpen it from the \"Bleach Soul Resurrección\" shortcut. The first time each scene appears, the game prepares its shaders and may pause for a moment; after that they are kept on disk.",
            "O jogo foi recompilado e instalado em\n{0}\n(em {1} min).\n\nAbra pelo atalho \"Bleach Soul Resurrección\". Na primeira vez que cada cena aparece, o jogo prepara os shaders dela e pode dar uma pequena pausa; depois eles ficam guardados no disco.",
            "Le jeu a été recompilé et installé dans\n{0}\n(en {1} min).\n\nLancez-le avec le raccourci « Bleach Soul Resurrección ». La première fois que chaque scène apparaît, le jeu prépare ses shaders et peut marquer une courte pause ; ensuite ils sont conservés sur le disque.",
            "El juego se recompiló e instaló en\n{0}\n(en {1} min).\n\nÁbrelo con el acceso directo \"Bleach Soul Resurrección\". La primera vez que aparece cada escena, el juego prepara sus shaders y puede pausarse un momento; después quedan guardados en el disco." } },
        { "log_em", new[] { "Details: {0}", "Detalhes: {0}", "Détails : {0}", "Detalles: {0}" } },
        { "download_falhou", new[] { "Could not download {0} from\n{1}\n({2}). Check the internet connection and try again.",
                                     "Não consegui baixar {0} de\n{1}\n({2}). Verifique a conexão com a internet e tente de novo.",
                                     "Impossible de télécharger {0} depuis\n{1}\n({2}). Vérifiez la connexion Internet et réessayez.",
                                     "No se pudo descargar {0} de\n{1}\n({2}). Comprueba la conexión a internet y vuelve a intentarlo." } },
        { "download_corrompido", new[] { "The download of {0} is damaged (wrong SHA-256). Try again.", "O download de {0} veio corrompido (SHA-256 diferente). Tente de novo.",
                                         "Le téléchargement de {0} est endommagé (SHA-256 incorrect). Réessayez.", "La descarga de {0} está dañada (SHA-256 distinto). Vuelve a intentarlo." } },
        { "compilacao_falhou", new[] { "The recompilation failed. Full log: {0}", "A recompilação falhou. Log completo: {0}", "La recompilation a échoué. Journal complet : {0}", "La recompilación falló. Registro completo: {0}" } },
        { "erro", new[] { "Error: ", "Erro: ", "Erreur : ", "Error: " } },
    };
}
