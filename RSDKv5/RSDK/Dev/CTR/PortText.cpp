#include "RSDK/Core/RetroEngine.hpp"
#include "RSDK/Dev/CTR/PortText.hpp"

#if RETRO_PLATFORM == RETRO_3DS

#include <string.h>

using namespace RSDK;

enum { COLS = LANGUAGE_JP + 1 };

// One row per English string, columns in the game's language order: EN, FR, IT, GE, SP, JP. UTF-8 (citro2d's system
// font has the accents and the Japanese). Kept about as long as the English: the cells are sized to it.
static const char *const kText[][COLS] = {
    // GAME tab
    { "MANIA MODE", "MODE MANIA", "MODALITÀ MANIA", "MANIA-MODUS", "MODO MANIA", "マニアモード" },
    { "ENCORE MODE", "MODE ENCORE", "MODALITÀ ENCORE", "ENCORE-MODUS", "MODO ENCORE", "アンコールモード" },
    { "TIME ATTACK", "CONTRE-LA-MONTRE", "GARA A TEMPO", "ZEITRENNEN", "CONTRARRELOJ", "タイムアタック" },
    { "COMPETITION", "COMPÉTITION", "COMPETIZIONE", "WETTKAMPF", "COMPETICIÓN", "対戦" },
    { "SAVE", "SAUVEGARDE", "SALVATAGGIO", "SPIELSTAND", "PARTIDA", "セーブ" },
    { "NO SAVE", "SANS SAUVEGARDE", "NESSUN SALVATAGGIO", "KEIN SPIELSTAND", "SIN PARTIDA", "セーブなし" },
    { "TIME", "TEMPS", "TEMPO", "ZEIT", "TIEMPO", "タイム" },
    { "RINGS", "ANNEAUX", "ANELLI", "RINGE", "ANILLOS", "リング" },
    { "SCORE", "SCORE", "PUNTEGGIO", "PUNKTE", "PUNTOS", "スコア" },
    { "LIVES", "VIES", "VITE", "LEBEN", "VIDAS", "残り" },
    { "EMERALDS", "ÉMERAUDES", "SMERALDI", "SMARAGDE", "ESMERALDAS", "エメラルド" },
    { "SHIELD", "BOUCLIER", "SCUDO", "SCHILD", "ESCUDO", "シールド" },
    { "NONE", "AUCUN", "NESSUNO", "KEINER", "NINGUNO", "なし" },
    { "BUBBLE", "BULLE", "BOLLA", "BLASE", "BURBUJA", "バブル" },
    { "FIRE", "FEU", "FUOCO", "FEUER", "FUEGO", "ファイア" },
    { "LIGHTNING", "ÉCLAIR", "FULMINE", "BLITZ", "RAYO", "サンダー" },
    { "SUPER", "SUPER", "SUPER", "SUPER", "SÚPER", "スーパー" },

    // OPTIONS tab
    { "FRAMES", "IMAGES", "FOTOGRAMMI", "BILDER", "IMÁGENES", "フレーム" },
    { "SHOW FPS", "VOIR FPS", "MOSTRA FPS", "FPS ZEIGEN", "VER FPS", "FPS表示" },
    { "TOP HUD", "HUD DU HAUT", "HUD IN ALTO", "HUD OBEN", "HUD ARRIBA", "上画面HUD" },
    { "RENDERER", "RENDU", "RENDERING", "RENDERER", "DIBUJO", "描画" },
    { "LANGUAGE", "LANGUE", "LINGUA", "SPRACHE", "IDIOMA", "言語" },
    { "30 FPS", "30 FPS", "30 FPS", "30 FPS", "30 FPS", "30 FPS" },
    { "AUTO", "AUTO", "AUTO", "AUTO", "AUTO", "自動" },
    { "ON", "OUI", "SÌ", "AN", "SÍ", "オン" },
    { "OFF", "NON", "NO", "AUS", "NO", "オフ" },
    { "TOP LEFT", "HAUT GAUCHE", "ALTO SINISTRA", "OBEN LINKS", "ARRIBA IZQ.", "左上" },
    { "TOP RIGHT", "HAUT DROITE", "ALTO DESTRA", "OBEN RECHTS", "ARRIBA DER.", "右上" },
    { "BOTTOM LEFT", "BAS GAUCHE", "BASSO SINISTRA", "UNTEN LINKS", "ABAJO IZQ.", "左下" },
    { "BOTTOM RIGHT", "BAS DROITE", "BASSO DESTRA", "UNTEN RECHTS", "ABAJO DER.", "右下" },
    { "SHOWN", "VISIBLE", "VISIBILE", "SICHTBAR", "VISIBLE", "表示" },
    { "HIDDEN", "CACHÉ", "NASCOSTO", "VERSTECKT", "OCULTO", "非表示" },
    { "AUTO SKIPS A LATE PICTURE TO KEEP THE SPEED, 30 FPS DRAWS EVERY OTHER ONE, OFF DRAWS THEM ALL (THE GAME SLOWS DOWN IF IT CANNOT KEEP UP)",
      "AUTO SAUTE UNE IMAGE EN RETARD POUR GARDER LA VITESSE, 30 FPS EN DESSINE UNE SUR DEUX, OFF LES DESSINE TOUTES (LE JEU RALENTIT S'IL NE SUIT PAS)",
      "AUTO SALTA UN'IMMAGINE IN RITARDO PER MANTENERE LA VELOCITÀ, 30 FPS NE DISEGNA UNA SU DUE, OFF LE DISEGNA TUTTE (IL GIOCO RALLENTA SE NON REGGE)",
      "AUTO LÄSST EIN VERSPÄTETES BILD AUS, UM DAS TEMPO ZU HALTEN, 30 FPS ZEICHNET JEDES ZWEITE, OFF ALLE (DAS SPIEL WIRD LANGSAMER, WENN ES NICHT MITKOMMT)",
      "AUTO SALTA UNA IMAGEN QUE LLEGA TARDE PARA MANTENER LA VELOCIDAD, 30 FPS DIBUJA UNA DE CADA DOS, OFF LAS DIBUJA TODAS (EL JUEGO VA MÁS LENTO SI NO LLEGA)",
      "自動は遅れた画面を飛ばして速さを保ち、30 FPSは2回に1回描き、オフはすべて描きます(間に合わないとゲームが遅くなります)" },
    { "FRAMES OFF: THE GAME MAY RUN SLOW IN SOME PLACES", "SANS SAUT D'IMAGES, LE JEU PEUT RALENTIR PAR ENDROITS",
      "SENZA SALTARE FOTOGRAMMI IL GIOCO PUÒ RALLENTARE IN ALCUNI PUNTI", "OHNE BILDAUSLASSEN KANN DAS SPIEL STELLENWEISE LANGSAM LAUFEN",
      "SIN SALTAR IMÁGENES EL JUEGO PUEDE IR LENTO EN ALGUNOS SITIOS", "フレームを飛ばさないと、場所によってゲームが遅くなることがあります" },
    { "SKIPS A LATE PICTURE SO THE GAME KEEPS ITS SPEED", "SAUTE UNE IMAGE EN RETARD POUR QUE LE JEU GARDE SA VITESSE",
      "SALTA UN'IMMAGINE IN RITARDO PERCHÉ IL GIOCO MANTENGA LA VELOCITÀ", "LÄSST EIN VERSPÄTETES BILD AUS, DAMIT DAS SPIEL SEIN TEMPO HÄLT",
      "SALTA UNA IMAGEN QUE LLEGA TARDE PARA QUE EL JUEGO NO PIERDA VELOCIDAD", "遅れた画面を飛ばしてゲームの速さを保ちます" },
    { "DRAWS EVERY OTHER PICTURE; THE GAME STAYS AT FULL SPEED", "DESSINE UNE IMAGE SUR DEUX ; LE JEU GARDE SA VITESSE",
      "DISEGNA UN'IMMAGINE SU DUE; IL GIOCO RESTA A PIENA VELOCITÀ", "ZEICHNET JEDES ZWEITE BILD; DAS SPIEL BLEIBT IM VOLLEN TEMPO",
      "DIBUJA UNA IMAGEN DE CADA DOS; EL JUEGO SIGUE A VELOCIDAD NORMAL", "2回に1回描きます。ゲームの速さはそのままです" },
    { "DRAWS EVERY PICTURE; THE GAME MAY RUN SLOW IN SOME PLACES", "DESSINE TOUTES LES IMAGES ; LE JEU PEUT RALENTIR PAR ENDROITS",
      "DISEGNA TUTTE LE IMMAGINI; IL GIOCO PUÒ RALLENTARE IN ALCUNI PUNTI", "ZEICHNET ALLE BILDER; DAS SPIEL KANN STELLENWEISE LANGSAM LAUFEN",
      "DIBUJA TODAS LAS IMÁGENES; EL JUEGO PUEDE IR LENTO EN ALGUNOS SITIOS", "すべて描きます。場所によってゲームが遅くなることがあります" },
    { "FRAME RATE ON THE TOP SCREEN: L LOGIC, R RENDER (MS)", "IMAGES PAR SECONDE SUR L'ÉCRAN DU HAUT : L LOGIQUE, R RENDU (MS)",
      "FOTOGRAMMI AL SECONDO SULLO SCHERMO SUPERIORE: L LOGICA, R DISEGNO (MS)", "BILDRATE AUF DEM OBEREN BILDSCHIRM: L LOGIK, R ZEICHNEN (MS)",
      "IMÁGENES POR SEGUNDO EN LA PANTALLA SUPERIOR: L LÓGICA, R DIBUJO (MS)", "上画面にフレームレート: L 処理、R 描画 (MS)" },
    { "AUTO HIDES THE GAME'S HUD WHILE THE GAME TAB SHOWS IT", "AUTO CACHE LE HUD DU JEU QUAND L'ONGLET JEU L'AFFICHE",
      "AUTO NASCONDE L'HUD DEL GIOCO MENTRE LA SCHEDA GIOCO LO MOSTRA", "AUTO VERSTECKT DAS HUD DES SPIELS, SOLANGE DER SPIEL-TAB ES ZEIGT",
      "AUTO OCULTA EL HUD DEL JUEGO MIENTRAS LA PESTAÑA JUEGO LO MUESTRA", "自動: ゲームタブが表示している間、ゲームのHUDを隠します" },
    { "GPU DRAWS WHAT IT CAN (FASTER), CPU IS THE ORIGINAL SOFTWARE RENDERER",
      "GPU DESSINE CE QU'IL PEUT (PLUS RAPIDE), CPU EST LE RENDU LOGICIEL D'ORIGINE",
      "GPU DISEGNA CIÒ CHE PUÒ (PIÙ VELOCE), CPU È IL RENDERING SOFTWARE ORIGINALE",
      "GPU ZEICHNET, WAS ES KANN (SCHNELLER), CPU IST DAS ORIGINALE SOFTWARE-RENDERING",
      "GPU DIBUJA LO QUE PUEDE (MÁS RÁPIDO), CPU ES EL DIBUJO POR SOFTWARE ORIGINAL", "GPUは描けるものを描きます(速い)。CPUは元のソフトウェア描画です" },
    { "THEME", "THÈME", "TEMA", "DESIGN", "TEMA", "テーマ" },
    { "CLASSIC", "CLASSIQUE", "CLASSICO", "KLASSISCH", "CLÁSICO", "クラシック" },
    { "THE COLOURS OF THIS SCREEN", "LES COULEURS DE CET ÉCRAN", "I COLORI DI QUESTO SCHERMO", "DIE FARBEN DIESES BILDSCHIRMS",
      "LOS COLORES DE ESTA PANTALLA", "この画面の色" },
    { "FOLLOWS THE GAME'S LANGUAGE: CHANGE IT IN THE GAME'S OPTIONS", "SUIT LA LANGUE DU JEU : À CHANGER DANS LES OPTIONS DU JEU",
      "SEGUE LA LINGUA DEL GIOCO: SI CAMBIA NELLE OPZIONI DEL GIOCO", "FOLGT DER SPRACHE DES SPIELS: IN DEN OPTIONEN DES SPIELS ÄNDERN",
      "SIGUE EL IDIOMA DEL JUEGO: SE CAMBIA EN LAS OPCIONES DEL JUEGO", "ゲームの言語に合わせます。ゲームのオプションで変更してください" },

    // UPDATES tab, the update prompt
    { "CHANNEL", "CANAL", "CANALE", "KANAL", "CANAL", "チャンネル" },
    { "AUTO UPDATE", "MAJ AUTO", "AGG. AUTO", "AUTO-UPDATE", "ACTUALIZ. AUTO", "自動更新" },
    { "RELEASE", "VERSION", "VERSIONE", "VERSION", "VERSIÓN", "リリース" },
    { "+ BETAS", "+ BÊTAS", "+ BETA", "+ BETAS", "+ BETAS", "+ ベータ" },
    { "STABLE", "STABLE", "STABILE", "STABIL", "ESTABLE", "安定版" },
    { "STATUS", "ÉTAT", "STATO", "STATUS", "ESTADO", "状態" },
    { "INSTALLED", "INSTALLÉE", "INSTALLATA", "INSTALLIERT", "INSTALADA", "インストール済み" },
    { "DOWNLOAD AND INSTALL THIS VERSION?", "TÉLÉCHARGER ET INSTALLER CETTE VERSION ?", "SCARICARE E INSTALLARE QUESTA VERSIONE?",
      "DIESE VERSION LADEN UND INSTALLIEREN?", "¿DESCARGAR E INSTALAR ESTA VERSIÓN?", "このバージョンをダウンロードしてインストールしますか?" },
    { "CHECK LOOKS FOR A NEW VERSION", "LE BOUTON CI-DESSOUS CHERCHE UNE NOUVELLE VERSION", "IL PULSANTE SOTTO CERCA UNA NUOVA VERSIONE",
      "DER KNOPF UNTEN SUCHT EINE NEUE VERSION", "EL BOTÓN DE ABAJO BUSCA UNA VERSIÓN NUEVA", "下のボタンで新しいバージョンを探します" },
    { "UNSAVED PROGRESS IS LOST", "LA PROGRESSION NON SAUVEGARDÉE EST PERDUE", "I PROGRESSI NON SALVATI VANNO PERSI",
      "UNGESPEICHERTER FORTSCHRITT GEHT VERLOREN", "SE PIERDE LO QUE NO ESTÉ GUARDADO", "セーブしていない進行は失われます" },
    { "TAP RELEASE FOR WHAT'S NEW", "TOUCHEZ VERSION POUR LES NOUVEAUTÉS", "TOCCA VERSIONE PER LE NOVITÀ", "VERSION ANTIPPEN: NEUIGKEITEN",
      "TOCA VERSIÓN PARA VER LAS NOVEDADES", "リリースをタッチすると新機能を表示" },
    { "INSTALL UPDATE", "INSTALLER LA MISE À JOUR", "INSTALLA L'AGGIORNAMENTO", "UPDATE INSTALLIEREN", "INSTALAR ACTUALIZACIÓN", "更新をインストール" },
    { "PLEASE WAIT", "PATIENTEZ", "ATTENDI", "BITTE WARTEN", "ESPERA", "お待ちください" },
    { "CANCEL", "ANNULER", "ANNULLA", "ABBRECHEN", "CANCELAR", "キャンセル" },
    { "DOWNLOAD UPDATE", "TÉLÉCHARGER LA MISE À JOUR", "SCARICA L'AGGIORNAMENTO", "UPDATE HERUNTERLADEN", "DESCARGAR ACTUALIZACIÓN", "更新をダウンロード" },
    { "UPDATE INSTALLED", "MISE À JOUR INSTALLÉE", "AGGIORNAMENTO INSTALLATO", "UPDATE INSTALLIERT", "ACTUALIZACIÓN INSTALADA", "更新をインストールしました" },
    { "CHECK FOR UPDATE", "CHERCHER UNE MISE À JOUR", "CERCA AGGIORNAMENTI", "NACH UPDATES SUCHEN", "BUSCAR ACTUALIZACIÓN", "更新を確認" },
    { "WAIT FOR THE UPDATER TO FINISH", "ATTENDEZ LA FIN DE LA MISE À JOUR", "ATTENDI CHE L'AGGIORNAMENTO FINISCA", "WARTE, BIS DAS UPDATE FERTIG IST",
      "ESPERA A QUE TERMINE EL ACTUALIZADOR", "更新が終わるまでお待ちください" },
    { "LOOKS FOR A NEW VERSION AT EVERY START", "CHERCHE UNE NOUVELLE VERSION À CHAQUE DÉMARRAGE", "CERCA UNA NUOVA VERSIONE A OGNI AVVIO",
      "SUCHT BEI JEDEM START NACH EINER NEUEN VERSION", "BUSCA UNA VERSIÓN NUEVA CADA VEZ QUE ARRANCA", "起動するたびに新しいバージョンを探します" },
    { "ONLY WHEN YOU CHECK HERE", "SEULEMENT QUAND VOUS CHERCHEZ ICI", "SOLO QUANDO CONTROLLI QUI", "NUR WENN DU HIER SUCHST", "SOLO CUANDO LO BUSQUES AQUÍ",
      "ここで確認した時だけ" },
    { "WHAT'S NEW", "NOUVEAUTÉS", "NOVITÀ", "NEUIGKEITEN", "NOVEDADES", "新機能" },
    { "PREV", "PRÉC.", "PREC.", "ZURÜCK", "ANT.", "前へ" },
    { "CLOSE", "FERMER", "CHIUDI", "SCHLIESSEN", "CERRAR", "閉じる" },
    { "BACK", "RETOUR", "INDIETRO", "ZURÜCK", "ATRÁS", "戻る" },
    { "NEXT", "SUIV.", "SUCC.", "WEITER", "SIG.", "次へ" },
    { "UPDATE AVAILABLE", "MISE À JOUR DISPONIBLE", "AGGIORNAMENTO DISPONIBILE", "UPDATE VERFÜGBAR", "ACTUALIZACIÓN DISPONIBLE", "更新があります" },
    { "NEW VERSION", "NOUVELLE VERSION", "NUOVA VERSIONE", "NEUE VERSION", "VERSIÓN NUEVA", "新バージョン" },
    { "INSTALL IT NOW?", "L'INSTALLER MAINTENANT ?", "INSTALLARLO ORA?", "JETZT INSTALLIEREN?", "¿INSTALARLA AHORA?", "今すぐインストールしますか?" },
    { "UPDATES TAB", "ONGLET MISES À JOUR", "SCHEDA AGGIORNAMENTI", "UPDATE-TAB", "PESTAÑA ACTUALIZACIONES", "更新タブ" },
    { "LATER", "PLUS TARD", "PIÙ TARDI", "SPÄTER", "MÁS TARDE", "後で" },
    { "INSTALL", "INSTALLER", "INSTALLA", "INSTALLIEREN", "INSTALAR", "インストール" },
    { "UPDATE FAILED", "ÉCHEC DE LA MISE À JOUR", "AGGIORNAMENTO NON RIUSCITO", "UPDATE FEHLGESCHLAGEN", "ERROR AL ACTUALIZAR", "更新に失敗しました" },
    { "THE GAME KEEPS RUNNING AS IT WAS.", "LE JEU CONTINUE COMME AVANT.", "IL GIOCO CONTINUA COME PRIMA.", "DAS SPIEL LÄUFT WEITER WIE BISHER.",
      "EL JUEGO SIGUE COMO ESTABA.", "ゲームはそのまま続きます。" },
    { "RESTARTING THE GAME", "REDÉMARRAGE DU JEU", "RIAVVIO DEL GIOCO", "SPIEL STARTET NEU", "REINICIANDO EL JUEGO", "ゲームを再起動しています" },
    { "UPDATING", "MISE À JOUR", "AGGIORNAMENTO", "UPDATE LÄUFT", "ACTUALIZANDO", "更新中" },
    { "DO NOT TURN THE CONSOLE OFF.", "N'ÉTEIGNEZ PAS LA CONSOLE.", "NON SPEGNERE LA CONSOLE.", "KONSOLE NICHT AUSSCHALTEN.", "NO APAGUES LA CONSOLA.",
      "電源を切らないでください。" },

    // The updater's status (Updater.cpp)
    { "SD CARD WRITE FAILED", "ÉCRITURE SUR LA SD ÉCHOUÉE", "SCRITTURA SU SD NON RIUSCITA", "SD-SCHREIBFEHLER", "ERROR AL ESCRIBIR EN LA SD",
      "SDカードへの書き込みに失敗" },
    { "NO WI-FI CONNECTION", "PAS DE CONNEXION WI-FI", "NESSUNA CONNESSIONE WI-FI", "KEINE WLAN-VERBINDUNG", "SIN CONEXIÓN WI-FI", "Wi-Fiに接続していません" },
    { "TLS SERVICE FAILED", "ÉCHEC DU SERVICE TLS", "SERVIZIO TLS NON RIUSCITO", "TLS-DIENST FEHLGESCHLAGEN", "FALLÓ EL SERVICIO TLS", "TLSサービスに失敗" },
    { "INVALID RELEASE DATA", "DONNÉES DE VERSION INVALIDES", "DATI DELLA VERSIONE NON VALIDI", "UNGÜLTIGE VERSIONSDATEN", "DATOS DE VERSIÓN NO VÁLIDOS",
      "リリース情報が正しくありません" },
    { "NO RELEASE AVAILABLE", "AUCUNE VERSION DISPONIBLE", "NESSUNA VERSIONE DISPONIBILE", "KEINE VERSION VERFÜGBAR", "NO HAY NINGUNA VERSIÓN",
      "リリースがありません" },
    { "NO STABLE RELEASE AVAILABLE", "AUCUNE VERSION STABLE", "NESSUNA VERSIONE STABILE", "KEINE STABILE VERSION", "NO HAY VERSIÓN ESTABLE", "安定版がありません" },
    { "YOU ARE UP TO DATE", "VOUS ÊTES À JOUR", "SEI AGGIORNATO", "AUF DEM NEUESTEN STAND", "ESTÁS AL DÍA", "最新です" },
    { "NOT ENOUGH SD SPACE", "ESPACE SD INSUFFISANT", "SPAZIO SD INSUFFICIENTE", "ZU WENIG PLATZ AUF DER SD", "NO HAY ESPACIO EN LA SD", "SDの空き容量が足りません" },
    { "3DSX LAUNCH PATH UNKNOWN", "CHEMIN DU 3DSX INCONNU", "PERCORSO 3DSX SCONOSCIUTO", "3DSX-PFAD UNBEKANNT", "RUTA DEL 3DSX DESCONOCIDA",
      "3DSXのパスが不明です" },
    { "CANNOT WRITE TO SD CARD", "IMPOSSIBLE D'ÉCRIRE SUR LA SD", "IMPOSSIBILE SCRIVERE SU SD", "SD NICHT BESCHREIBBAR", "NO SE PUEDE ESCRIBIR EN LA SD",
      "SDに書き込めません" },
    { "VERIFYING DOWNLOAD", "VÉRIFICATION DU TÉLÉCHARGEMENT", "VERIFICA DEL DOWNLOAD", "DOWNLOAD WIRD GEPRÜFT", "COMPROBANDO LA DESCARGA",
      "ダウンロードを確認中" },
    { "DOWNLOAD CHECK FAILED", "TÉLÉCHARGEMENT CORROMPU", "VERIFICA DEL DOWNLOAD NON RIUSCITA", "DOWNLOAD-PRÜFUNG FEHLGESCHLAGEN", "LA DESCARGA NO ES CORRECTA",
      "ダウンロードの確認に失敗" },
    { "INSTALLING UPDATE", "INSTALLATION DE LA MISE À JOUR", "INSTALLAZIONE AGGIORNAMENTO", "UPDATE WIRD INSTALLIERT", "INSTALANDO ACTUALIZACIÓN",
      "更新をインストール中" },
    { "INSTALLATION FAILED", "ÉCHEC DE L'INSTALLATION", "INSTALLAZIONE NON RIUSCITA", "INSTALLATION FEHLGESCHLAGEN", "FALLÓ LA INSTALACIÓN",
      "インストールに失敗" },
    { "UPDATE CANCELLED", "MISE À JOUR ANNULÉE", "AGGIORNAMENTO ANNULLATO", "UPDATE ABGEBROCHEN", "ACTUALIZACIÓN CANCELADA", "更新をキャンセルしました" },
    { "CONNECTION FAILED - RETRY", "ÉCHEC DE CONNEXION - RÉESSAYEZ", "CONNESSIONE NON RIUSCITA - RIPROVA", "VERBINDUNGSFEHLER - NOCHMAL VERSUCHEN",
      "FALLÓ LA CONEXIÓN - REINTENTA", "接続に失敗 - もう一度試してください" },
    { "DOWNLOADING UPDATE", "TÉLÉCHARGEMENT DE LA MISE À JOUR", "DOWNLOAD AGGIORNAMENTO", "UPDATE WIRD GELADEN", "DESCARGANDO ACTUALIZACIÓN",
      "更新をダウンロード中" },
    { "CHECKING FOR UPDATES", "RECHERCHE DE MISES À JOUR", "RICERCA AGGIORNAMENTI", "SUCHE NACH UPDATES", "BUSCANDO ACTUALIZACIONES", "更新を確認中" },
    { "CANNOT START UPDATE", "IMPOSSIBLE DE LANCER LA MISE À JOUR", "IMPOSSIBILE AVVIARE L'AGGIORNAMENTO", "UPDATE KANN NICHT STARTEN",
      "NO SE PUEDE INICIAR LA ACTUALIZACIÓN", "更新を開始できません" },

    // Boot in a smaller memory mode (Port::LowMemoryStop); %u: the mode's MB, then the RSF's
    { "NOT ENOUGH MEMORY", "MÉMOIRE INSUFFISANTE", "MEMORIA INSUFFICIENTE", "NICHT GENUG SPEICHER", "NO HAY MEMORIA SUFICIENTE", "メモリが足りません" },
    { "THE CONSOLE STARTED THE GAME IN ITS %u MB MODE INSTEAD OF %u MB.", "LA CONSOLE A LANCÉ LE JEU DANS SON MODE %u MO AU LIEU DE %u MO.",
      "LA CONSOLE HA AVVIATO IL GIOCO NELLA MODALITÀ DA %u MB INVECE DI %u MB.", "DIE KONSOLE HAT DAS SPIEL IM %u-MB-MODUS STATT MIT %u MB GESTARTET.",
      "LA CONSOLA HA ABIERTO EL JUEGO EN SU MODO DE %u MB EN VEZ DE %u MB.", "本体がゲームを%u MBモードで起動しました(%u MBが必要です)。" },
    { "TURN THE CONSOLE OFF AND ON, THEN OPEN THE GAME AGAIN.", "ÉTEIGNEZ ET RALLUMEZ LA CONSOLE, PUIS RELANCEZ LE JEU.",
      "SPEGNI E RIACCENDI LA CONSOLE, POI RIAPRI IL GIOCO.", "SCHALTE DIE KONSOLE AUS UND WIEDER EIN UND ÖFFNE DAS SPIEL ERNEUT.",
      "APAGA Y ENCIENDE LA CONSOLA Y VUELVE A ABRIR EL JUEGO.", "本体の電源を切ってから入れ直し、もう一度ゲームを開いてください。" },
    { "EXIT", "QUITTER", "ESCI", "BEENDEN", "SALIR", "終了" },

    // No game data (Port::MissingDataStop); %s: a file's name
    { "GAME DATA NOT FOUND", "DONNÉES DU JEU INTROUVABLES", "DATI DEL GIOCO NON TROVATI", "SPIELDATEN NICHT GEFUNDEN",
      "FALTAN LOS DATOS DEL JUEGO", "ゲームデータが見つかりません" },
    { "GAME DATA NOT VALID", "DONNÉES DU JEU NON VALIDES", "DATI DEL GIOCO NON VALIDI", "SPIELDATEN UNGÜLTIG", "DATOS DEL JUEGO NO VÁLIDOS",
      "ゲームデータが正しくありません" },
    { "COPY DATA.RSDK FROM YOUR SONIC MANIA (PC) TO THE SD CARD:", "COPIEZ DATA.RSDK DE VOTRE SONIC MANIA (PC) SUR LA CARTE SD :",
      "COPIA DATA.RSDK DAL TUO SONIC MANIA (PC) SULLA SCHEDA SD:", "KOPIERE DATA.RSDK AUS DEINEM SONIC MANIA (PC) AUF DIE SD-KARTE:",
      "COPIA DATA.RSDK DE TU SONIC MANIA (PC) A LA TARJETA SD:", "お持ちのソニックマニア(PC)のDATA.RSDKをSDカードにコピーしてください:" },
    { "THE DATA.RSDK ON THE SD CARD IS NOT A SONIC MANIA DATA FILE. COPY IT AGAIN:",
      "LE DATA.RSDK DE LA CARTE SD N'EST PAS UN FICHIER DE SONIC MANIA. COPIEZ-LE À NOUVEAU :",
      "IL DATA.RSDK DELLA SCHEDA SD NON È UN FILE DI SONIC MANIA. COPIALO DI NUOVO:",
      "DIE DATA.RSDK AUF DER SD-KARTE IST KEINE SONIC-MANIA-DATEI. KOPIERE SIE ERNEUT:",
      "EL DATA.RSDK DE LA TARJETA SD NO ES UN ARCHIVO DE SONIC MANIA. CÓPIALO DE NUEVO:",
      "SDカードのDATA.RSDKはソニックマニアのデータではありません。もう一度コピーしてください:" },
    { "THE SD CARD HAS %s. RENAME AND MOVE IT TO:", "LA CARTE SD CONTIENT %s. RENOMMEZ-LE ET DÉPLACEZ-LE VERS :",
      "LA SCHEDA SD HA %s. RINOMINALO E SPOSTALO IN:", "AUF DER SD-KARTE LIEGT %s. BENENNE SIE UM UND VERSCHIEBE SIE NACH:",
      "LA TARJETA SD TIENE %s. CAMBIA SU NOMBRE Y MUÉVELO A:", "SDカードに %s があります。名前を変えて次の場所に移してください:" },
    { "THEN OPEN THE GAME AGAIN.", "PUIS RELANCEZ LE JEU.", "POI RIAPRI IL GIOCO.", "DANN ÖFFNE DAS SPIEL ERNEUT.", "LUEGO VUELVE A ABRIR EL JUEGO.",
      "それからもう一度ゲームを開いてください。" },

    // The save import (Upstream.cpp)
    { "IMPORT SAVES", "IMPORTER LES SAUVEGARDES", "IMPORTA SALVATAGGI", "SPIELSTÄNDE IMPORTIEREN", "IMPORTAR PARTIDAS", "セーブを取り込む" },
    { "SAVES OF THE ORIGINAL 3DS PORT FOUND", "SAUVEGARDES DU PORTAGE 3DS D'ORIGINE TROUVÉES", "TROVATI SALVATAGGI DEL PORT 3DS ORIGINALE",
      "SPIELSTÄNDE DER ORIGINALEN 3DS-PORTIERUNG GEFUNDEN", "HAY PARTIDAS DEL PORT ORIGINAL DE 3DS", "元の3DS移植版のセーブがあります" },
    { "IN /3DS/SONICMANIA/. COPY THEM TO THIS APP?", "DANS /3DS/SONICMANIA/. LES COPIER DANS CETTE APP ?", "IN /3DS/SONICMANIA/. COPIARLI IN QUESTA APP?",
      "IN /3DS/SONICMANIA/. IN DIESE APP KOPIEREN?", "EN /3DS/SONICMANIA/. ¿COPIARLAS A ESTA APP?", "/3DS/SONICMANIA/ にあります。このアプリにコピーしますか?" },
    { "THE ORIGINAL APP KEEPS ITS OWN.", "L'APP D'ORIGINE GARDE LES SIENNES.", "L'APP ORIGINALE CONSERVA I SUOI.", "DIE ORIGINALE APP BEHÄLT IHRE.",
      "LA APP ORIGINAL CONSERVA LAS SUYAS.", "元のアプリのセーブはそのまま残ります。" },
    { "COPY", "COPIER", "COPIA", "KOPIEREN", "COPIAR", "コピー" },
    { "SKIP", "IGNORER", "SALTA", "ÜBERSPRINGEN", "OMITIR", "しない" },
    { "SAVES COPIED", "SAUVEGARDES COPIÉES", "SALVATAGGI COPIATI", "SPIELSTÄNDE KOPIERT", "PARTIDAS COPIADAS", "セーブをコピーしました" },
    { "COPY FAILED", "ÉCHEC DE LA COPIE", "COPIA NON RIUSCITA", "KOPIEREN FEHLGESCHLAGEN", "FALLÓ LA COPIA", "コピーに失敗しました" },

    // Sonic Mania Plus (Port::OfferPlus, OPTIONS > PLUS)
    { "PLUS", "PLUS", "PLUS", "PLUS", "PLUS", "PLUS" },
    { "SONIC MANIA PLUS", "SONIC MANIA PLUS", "SONIC MANIA PLUS", "SONIC MANIA PLUS", "SONIC MANIA PLUS", "ソニックマニア・プラス" },
    { "THE GAME DATA HAS THE PLUS CONTENT: MIGHTY, RAY AND ENCORE MODE.", "LES DONNÉES DU JEU ONT LE CONTENU PLUS : MIGHTY, RAY ET LE MODE ENCORE.",
      "I DATI DEL GIOCO HANNO I CONTENUTI PLUS: MIGHTY, RAY E LA MODALITÀ ENCORE.", "DIE SPIELDATEN ENTHALTEN DEN PLUS-INHALT: MIGHTY, RAY UND DEN ENCORE-MODUS.",
      "LOS DATOS DEL JUEGO TIENEN EL CONTENIDO PLUS: MIGHTY, RAY Y EL MODO ENCORE.", "ゲームデータにプラスの内容があります: マイティー、レイ、アンコールモード。" },
    { "TURN IT ON ONLY IF YOU OWN SONIC MANIA PLUS.", "NE L'ACTIVEZ QUE SI VOUS POSSÉDEZ SONIC MANIA PLUS.", "ATTIVALO SOLO SE POSSIEDI SONIC MANIA PLUS.",
      "NUR EINSCHALTEN, WENN DU SONIC MANIA PLUS BESITZT.", "ACTÍVALO SOLO SI TIENES SONIC MANIA PLUS.", "ソニックマニア・プラスを持っている場合だけオンにしてください。" },
    { "YOU CAN CHANGE IT IN OPTIONS > PLUS.", "MODIFIABLE DANS OPTIONS > PLUS.", "SI CAMBIA IN OPZIONI > PLUS.", "ÄNDERBAR UNTER OPTIONEN > PLUS.",
      "SE CAMBIA EN OPCIONES > PLUS.", "オプション > PLUS で変更できます。" },
    { "TURN ON", "ACTIVER", "ATTIVA", "EINSCHALTEN", "ACTIVAR", "オンにする" },
    { "LEAVE OFF", "LAISSER DÉSACTIVÉ", "LASCIA SPENTO", "AUS LASSEN", "DEJAR APAGADO", "オフのまま" },
    { "MIGHTY, RAY AND ENCORE MODE, ONLY IF YOU OWN SONIC MANIA PLUS. IT CHANGES AT THE NEXT START",
      "MIGHTY, RAY ET LE MODE ENCORE, SEULEMENT SI VOUS POSSÉDEZ SONIC MANIA PLUS. EFFET AU PROCHAIN DÉMARRAGE",
      "MIGHTY, RAY E LA MODALITÀ ENCORE, SOLO SE POSSIEDI SONIC MANIA PLUS. VALE DAL PROSSIMO AVVIO",
      "MIGHTY, RAY UND DER ENCORE-MODUS, NUR MIT SONIC MANIA PLUS. GILT AB DEM NÄCHSTEN START",
      "MIGHTY, RAY Y EL MODO ENCORE, SOLO SI TIENES SONIC MANIA PLUS. CAMBIA AL VOLVER A ARRANCAR",
      "マイティー、レイ、アンコールモード。ソニックマニア・プラスを持っている場合のみ。次の起動から変わります" },

    // The ACHIEVEMENTS tab; the descriptions are UserCore.cpp's (the names stay as they are: titles)
    { "UNLOCKED", "DÉBLOQUÉ", "SBLOCCATO", "FREIGESCHALTET", "DESBLOQUEADO", "達成" },
    { "LOCKED", "VERROUILLÉ", "BLOCCATO", "GESPERRT", "BLOQUEADO", "未達成" },
    { "NOTICE", "AVIS", "AVVISO", "HINWEIS", "AVISO", "通知" },
    { "TOP", "HAUT", "ALTO", "OBEN", "ARRIBA", "上" },
    { "BOTTOM", "BAS", "BASSO", "UNTEN", "ABAJO", "下" },
    { "SOUND", "SON", "SUONO", "TON", "SONIDO", "音" },
    { "ACHIEVEMENT!", "SUCCÈS !", "OBIETTIVO!", "ERFOLG!", "¡LOGRO!", "実績解除!" },
    { "Collect gold medallions in Blue Spheres Bonus stage", "Obtenez des médailles d'or dans le niveau bonus Blue Spheres", "Ottieni medaglie d'oro nel livello bonus Blue Spheres", "Hol Goldmedaillen in der Blue-Spheres-Bonusstage", "Consigue medallas de oro en la fase bonus de Blue Spheres", "ブルースフィアのボーナスステージで金メダルを集める" },
    { "Collect silver medallions in Blue Spheres Bonus stage", "Obtenez des médailles d'argent dans le niveau bonus Blue Spheres", "Ottieni medaglie d'argento nel livello bonus Blue Spheres", "Hol Silbermedaillen in der Blue-Spheres-Bonusstage", "Consigue medallas de plata en la fase bonus de Blue Spheres", "ブルースフィアのボーナスステージで銀メダルを集める" },
    { "Collect all seven Chaos Emeralds", "Obtenez les sept Émeraudes du Chaos", "Ottieni tutti e sette gli Smeraldi del Caos", "Sammle alle sieben Chaos-Smaragde", "Consigue las siete Esmeraldas del Caos", "カオスエメラルドを7つすべて集める" },
    { "Achieve any ending", "Atteignez une fin", "Raggiungi un finale qualsiasi", "Erreiche ein beliebiges Ende", "Consigue cualquier final", "いずれかのエンディングを見る" },
    { "Spin the Star Post!", "Faites tourner la borne !", "Fai girare il checkpoint!", "Dreh den Checkpoint!", "¡Haz girar el poste de control!", "スターポストを回そう!" },
    { "Find the hidden item boxes at the end of the Zone", "Trouvez les boîtes cachées à la fin de la zone", "Trova i monitor nascosti alla fine della zona", "Finde die versteckten Itemboxen am Ende der Zone", "Encuentra las cajas ocultas al final de la zona", "ゾーンの最後に隠されたアイテムボックスを見つける" },
    { "What would happen if you cross a bridge with a fire shield?", "Que se passe-t-il si vous traversez un pont avec un bouclier de feu ?", "Cosa succede se attraversi un ponte con uno scudo di fuoco?", "Was passiert, wenn du mit einem Feuerschild über eine Brücke läufst?", "¿Qué pasaría si cruzas un puente con un escudo de fuego?", "ファイアシールドで橋を渡ったらどうなる?" },
    { "Try for a 3 chain combo!", "Tentez un combo de 3 chaînes !", "Prova una combo da 3 catene!", "Versuch eine 3er-Kettenkombo!", "¡Intenta un combo de 3 cadenas!", "3連鎖を狙おう!" },
    { "Have your photos taken in Studiopolis Zone", "Faites-vous prendre en photo dans Studiopolis Zone", "Fatti fotografare in Studiopolis Zone", "Lass dich in der Studiopolis Zone fotografieren", "Hazte fotos en Studiopolis Zone", "スタジオポリスゾーンで写真を撮られよう" },
    { "Let the wind take you through", "Laissez le vent vous porter", "Lasciati trasportare dal vento", "Lass dich vom Wind tragen", "Deja que el viento te lleve", "風に運ばれよう" },
    { "Wreak havoc at the propaganda factory", "Semez le chaos dans l'usine de propagande", "Scatena il caos nella fabbrica di propaganda", "Richte Chaos in der Propagandafabrik an", "Causa estragos en la fábrica de propaganda", "プロパガンダ工場で大暴れしよう" },
    { "Get through Stardust Speedway Zone as quickly as possible", "Traversez Stardust Speedway Zone le plus vite possible", "Attraversa Stardust Speedway Zone il più in fretta possibile", "Durchquere die Stardust Speedway Zone so schnell wie möglich", "Cruza Stardust Speedway Zone lo más rápido posible", "スターダストスピードウェイゾーンをできるだけ速く駆け抜けよう" },
    { "We really like boats", "Nous aimons beaucoup les bateaux", "Ci piacciono molto le barche", "Wir mögen Boote sehr", "Nos gustan mucho los barcos", "ボートが大好き" },
    { "Try pushing a barrel to see how far it goes", "Poussez un tonneau pour voir jusqu'où il va", "Spingi un barile per vedere fin dove arriva", "Schieb ein Fass und schau, wie weit es rollt", "Empuja un barril a ver hasta dónde llega", "樽を押してどこまで行くか見てみよう" },
    { "You might have to submerge to find it", "Il faudra peut-être plonger pour le trouver", "Forse dovrai immergerti per trovarlo", "Vielleicht musst du abtauchen, um es zu finden", "Quizá tengas que sumergirte para encontrarlo", "見つけるには潜る必要があるかも" },
    { "Barrel through the lava, don't let anything stop you", "Foncez à travers la lave, que rien ne vous arrête", "Sfreccia nella lava, non lasciare che nulla ti fermi", "Brich durch die Lava, lass dich von nichts aufhalten", "Atraviesa la lava a toda velocidad, que nada te detenga", "溶岩を突き進め、何にも止められるな" },
    { "Gotta gacha 'em all", "Attrapez-les tous à la gacha", "Gachali tutti", "Schnapp sie dir alle aus der Gacha", "Hazte con todas las cápsulas", "ガチャを全部集めよう" },
    { "That elusive perfect run, only a professional can achieve", "Cette course parfaite que seul un pro peut réussir", "Quella corsa perfetta che solo un professionista può fare", "Der perfekte Lauf, den nur ein Profi schafft", "Esa partida perfecta que solo logra un profesional", "プロだけが成し遂げられる完璧な走り" },

    // The loading box (engine steps)
    { "STARTING SONIC MANIA", "DÉMARRAGE DE SONIC MANIA", "AVVIO DI SONIC MANIA", "SONIC MANIA STARTET", "INICIANDO SONIC MANIA", "SONIC MANIAを起動中" },
    { "GAME CONFIG", "CONFIGURATION DU JEU", "CONFIGURAZIONE GIOCO", "SPIEL-KONFIGURATION", "CONFIGURACIÓN DEL JUEGO", "ゲーム設定" },
    { "STAGE CONFIG", "CONFIGURATION DU NIVEAU", "CONFIGURAZIONE LIVELLO", "LEVEL-KONFIGURATION", "CONFIGURACIÓN DEL NIVEL", "ステージ設定" },
    { "SOUND EFFECTS", "EFFETS SONORES", "EFFETTI SONORI", "SOUNDEFFEKTE", "EFECTOS DE SONIDO", "効果音" },
    { "TILES", "TUILES", "TILE", "KACHELN", "BLOQUES", "タイル" },
    { "SCENE LAYOUT", "DISPOSITION DE LA SCÈNE", "STRUTTURA DELLA SCENA", "SZENENAUFBAU", "ESTRUCTURA DE LA ESCENA", "シーン配置" },
    { "OBJECTS", "OBJETS", "OGGETTI", "OBJEKTE", "OBJETOS", "オブジェクト" },
};

int Port::TextLanguage()
{
#if RETRO_REV02
    const int l = SKU::curSKU.language;
#else
    const int l = gameVerInfo.language;
#endif
    return l >= LANGUAGE_EN && l < COLS ? l : LANGUAGE_EN;
}

const char *Port::LanguageName()
{
    static const char *const kName[COLS] = { "ENGLISH", "FRANÇAIS", "ITALIANO", "DEUTSCH", "ESPAÑOL", "日本語" };
    return kName[TextLanguage()];
}

const char *Port::Tr(const char *english)
{
    const int l = TextLanguage();
    if (!english || l == LANGUAGE_EN)
        return english;
    for (unsigned i = 0; i < sizeof(kText) / sizeof(kText[0]); ++i)
        if (!strcmp(kText[i][0], english))
            return kText[i][l];
    return english;
}

#endif
