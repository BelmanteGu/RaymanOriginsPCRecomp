package io.github.belmantegu.raymanpc;

import java.util.HashMap;
import java.util.Map;

/** The launcher's own texts in French, German, Spanish, Italian, Dutch, Portuguese, Polish and Russian (English is the
 * key; GameMenu picks the column of the language chosen in its settings). */
final class MenuText {
    static final Map<String, String[]> I18N = new HashMap<>();
    static {
        Map<String, String[]> m = I18N;
        m.put("Select your Rayman Origins folder (Steam)", new String[] {"Choisissez votre dossier Rayman Origins (Steam)", "Wähle deinen Rayman-Origins-Ordner (Steam)", "Elige tu carpeta de Rayman Origins (Steam)", "Scegli la cartella di Rayman Origins (Steam)", "Kies je Rayman Origins-map (Steam)", "Escolha a sua pasta do Rayman Origins (Steam)", "Wybierz folder Rayman Origins (Steam)", "Выберите папку Rayman Origins (Steam)"});
        m.put("Your own copy is read in place, nothing is downloaded", new String[] {"Votre copie est lue sur place, rien n'est téléchargé", "Deine eigene Kopie wird direkt gelesen, nichts wird geladen", "Se lee tu propia copia, no se descarga nada", "La tua copia viene letta sul posto, non si scarica nulla", "Je eigen kopie wordt gelezen, er wordt niets gedownload", "A sua cópia é lida no lugar, nada é baixado", "Twoja kopia jest czytana na miejscu, nic nie jest pobierane", "Ваша копия читается на месте, ничего не скачивается"});
        m.put("Select your Rayman Origins folder or .zip (Steam)", new String[] {"Choisissez votre dossier ou .zip de Rayman Origins (Steam)", "Wähle deinen Rayman-Origins-Ordner oder .zip (Steam)", "Elige tu carpeta o .zip de Rayman Origins (Steam)", "Scegli la cartella o lo .zip di Rayman Origins (Steam)", "Kies je Rayman Origins-map of .zip (Steam)", "Escolha a sua pasta ou .zip do Rayman Origins (Steam)", "Wybierz folder lub .zip Rayman Origins (Steam)", "Выберите папку или .zip Rayman Origins (Steam)"});
        m.put("Game folder", new String[] {"Dossier du jeu", "Spielordner", "Carpeta del juego", "Cartella del gioco", "Spelmap", "Pasta do jogo", "Folder gry", "Папка игры"});
        m.put(".zip file", new String[] {"Fichier .zip", ".zip-Datei", "Archivo .zip", "File .zip", ".zip-bestand", "Arquivo .zip", "Plik .zip", "Файл .zip"});
        m.put("Importing the .zip", new String[] {"Importation du .zip", ".zip wird importiert", "Importando el .zip", "Importazione dello .zip", ".zip importeren", "Importando o .zip", "Importowanie .zip", "Импорт .zip"});
        m.put("GPU driver", new String[] {"Pilote GPU", "GPU-Treiber", "Controlador de GPU", "Driver GPU", "GPU-driver", "Driver de GPU", "Sterownik GPU", "Драйвер GPU"});
        m.put("System driver", new String[] {"Pilote du système", "System-Treiber", "Controlador del sistema", "Driver di sistema", "Systeemdriver", "Driver do sistema", "Sterownik systemowy", "Системный драйвер"});
        m.put("Turnip (download from GitHub)", new String[] {"Turnip (télécharger depuis GitHub)", "Turnip (von GitHub laden)", "Turnip (descargar de GitHub)", "Turnip (scarica da GitHub)", "Turnip (downloaden van GitHub)", "Turnip (baixar do GitHub)", "Turnip (pobierz z GitHub)", "Turnip (скачать с GitHub)"});
        m.put("Downloading the Turnip driver", new String[] {"Téléchargement du pilote Turnip", "Turnip-Treiber wird geladen", "Descargando el controlador Turnip", "Download del driver Turnip", "Turnip-driver downloaden", "Baixando o driver Turnip", "Pobieranie sterownika Turnip", "Загрузка драйвера Turnip"});
        m.put("Turnip installed", new String[] {"Turnip installé", "Turnip installiert", "Turnip instalado", "Turnip installato", "Turnip geïnstalleerd", "Turnip instalado", "Turnip zainstalowany", "Turnip установлен"});
        m.put("The last driver did not work; the system driver was used.", new String[] {"Le dernier pilote n'a pas fonctionné ; celui du système a été utilisé.", "Der letzte Treiber lief nicht; der System-Treiber wurde benutzt.", "El último controlador no funcionó; se usó el del sistema.", "L'ultimo driver non ha funzionato; è stato usato quello di sistema.", "De laatste driver werkte niet; de systeemdriver is gebruikt.", "O último driver não funcionou; foi usado o do sistema.", "Ostatni sterownik nie działał; użyto systemowego.", "Последний драйвер не заработал; использован системный."});
        m.put("Mode", new String[] {"Mode", "Modus", "Modo", "Modalità", "Modus", "Modo", "Tryb", "Режим"});
        m.put("Potato", new String[] {"Patate", "Kartoffel", "Patata", "Patata", "Aardappel", "Batata", "Ziemniak", "Картошка"});
        m.put("Low", new String[] {"Bas", "Niedrig", "Bajo", "Basso", "Laag", "Baixo", "Niski", "Низкий"});
        m.put("Medium", new String[] {"Moyen", "Mittel", "Medio", "Medio", "Gemiddeld", "Médio", "Średni", "Средний"});
        m.put("High", new String[] {"Élevé", "Hoch", "Alto", "Alto", "Hoog", "Alto", "Wysoki", "Высокий"});
        m.put("Max", new String[] {"Max", "Max", "Máximo", "Massimo", "Max", "Máximo", "Maks.", "Макс."});
        m.put("Restart the game to apply the mode", new String[] {"Redémarrez le jeu pour appliquer le mode", "Spiel neu starten, um den Modus zu übernehmen", "Reinicia el juego para aplicar el modo", "Riavvia il gioco per applicare la modalità", "Herstart het spel om de modus toe te passen", "Reinicie o jogo para aplicar o modo", "Uruchom grę ponownie, aby zastosować tryb", "Перезапустите игру, чтобы применить режим"});
        m.put("Custom", new String[] {"Personnalisé", "Eigene", "Personalizado", "Personalizzato", "Aangepast", "Personalizado", "Własny", "Свой"});
        m.put("Frameskip", new String[] {"Saut d'images", "Frameskip", "Salto de cuadros", "Salto fotogrammi", "Frameskip", "Pular quadros", "Pomijanie klatek", "Пропуск кадров"});
        m.put("System driver (Turnip off)", new String[] {"Pilote du système (Turnip désactivé)", "System-Treiber (Turnip aus)", "Controlador del sistema (Turnip desactivado)", "Driver di sistema (Turnip spento)", "Systeemdriver (Turnip uit)", "Driver do sistema (Turnip desligado)", "Sterownik systemowy (Turnip wył.)", "Системный драйвер (Turnip выкл.)"});
        m.put("Turnip", new String[] {"Turnip", "Turnip", "Turnip", "Turnip", "Turnip", "Turnip", "Turnip", "Turnip"});
        m.put("Download", new String[] {"Télécharger", "Laden", "Descargar", "Scarica", "Downloaden", "Baixar", "Pobierz", "Скачать"});
        m.put("Downloading", new String[] {"Téléchargement de", "Lade", "Descargando", "Download di", "Downloaden", "Baixando", "Pobieranie", "Загрузка"});
        m.put("Import driver (.zip)", new String[] {"Importer un pilote (.zip)", "Treiber importieren (.zip)", "Importar controlador (.zip)", "Importa driver (.zip)", "Driver importeren (.zip)", "Importar driver (.zip)", "Importuj sterownik (.zip)", "Импорт драйвера (.zip)"});
        m.put("Play", new String[] {"Jouer", "Spielen", "Jugar", "Gioca", "Spelen", "Jogar", "Graj", "Играть"});
        m.put("Options", new String[] {"Options", "Optionen", "Opciones", "Opzioni", "Opties", "Opções", "Opcje", "Настройки"});
        m.put("Game files", new String[] {"Fichiers du jeu", "Spieldateien", "Archivos del juego", "File di gioco", "Spelbestanden", "Arquivos do jogo", "Pliki gry", "Файлы игры"});
        m.put("Quit Game", new String[] {"Quitter le jeu", "Spiel beenden", "Salir del juego", "Esci dal gioco", "Spel afsluiten", "Sair do jogo", "Wyjdź z gry", "Выйти из игры"});
        m.put("Touch Controls", new String[] {"Commandes tactiles", "Touch-Steuerung", "Controles táctiles", "Comandi touch", "Aanraakbediening", "Controles de toque", "Sterowanie dotykowe", "Сенсорное управление"});
        m.put("Size", new String[] {"Taille", "Größe", "Tamaño", "Dimensione", "Grootte", "Tamanho", "Rozmiar", "Размер"});
        m.put("Defaults", new String[] {"Défaut", "Standard", "Predeterm.", "Predefiniti", "Standaard", "Padrão", "Domyślne", "По умолч."});
        m.put("Language", new String[] {"Langue", "Sprache", "Idioma", "Lingua", "Taal", "Idioma", "Język", "Язык"});
        m.put("Opacity", new String[] {"Opacité", "Deckkraft", "Opacidad", "Opacità", "Dekking", "Opacidade", "Krycie", "Прозрачн."});
        m.put("Stick", new String[] {"Stick", "Stick", "Stick", "Levetta", "Stick", "Analógico", "Gałka", "Стик"});
        m.put("Run", new String[] {"Courir", "Rennen", "Correr", "Corsa", "Rennen", "Correr", "Bieg", "Бег"});
        m.put("Vibration", new String[] {"Vibration", "Vibration", "Vibración", "Vibrazione", "Trilling", "Vibração", "Wibracje", "Вибрация"});
        m.put("Pad hides", new String[] {"Manette cache", "Pad verbirgt", "Mando oculta", "Pad nasconde", "Pad verbergt", "Controle oculta", "Pad ukrywa", "Пад скрывает"});
        m.put("Show FPS", new String[] {"Afficher FPS", "FPS zeigen", "Mostrar FPS", "Mostra FPS", "FPS tonen", "Mostrar FPS", "Pokaż FPS", "Показ. FPS"});
        m.put("RESET", new String[] {"RÉINIT.", "ZURÜCKS.", "RESTABLECER", "RIPRISTINA", "RESET", "REDEFINIR", "RESETUJ", "СБРОС"});
        m.put("Floating", new String[] {"Flottant", "Schwebend", "Flotante", "Mobile", "Zwevend", "Flutuante", "Pływająca", "Плавающий"});
        m.put("Fixed", new String[] {"Fixe", "Fest", "Fijo", "Fisso", "Vast", "Fixo", "Stała", "Фиксир."});
        m.put("Toggle", new String[] {"Bascule", "Umschalten", "Alternar", "Alterna", "Wisselen", "Alternar", "Przełącz", "Переключ."});
        m.put("Hold", new String[] {"Maintenir", "Halten", "Mantener", "Tieni", "Vasthouden", "Segurar", "Przytrzymaj", "Удерживать"});
        m.put("On", new String[] {"Oui", "An", "Sí", "Sì", "Aan", "Sim", "Wł.", "Вкл."});
        m.put("Off", new String[] {"Non", "Aus", "No", "No", "Uit", "Não", "Wył.", "Выкл."});
        m.put("Button layout", new String[] {"Position des boutons", "Tastenposition", "Posición botones", "Posizione tasti", "Knopposities", "Posição dos botões", "Układ przycisków", "Раскладка кнопок"});
        m.put("Edit", new String[] {"Modifier", "Bearbeiten", "Editar", "Modifica", "Bewerken", "Editar", "Edytuj", "Изменить"});
        m.put("Back to launcher", new String[] {"Retour au lanceur", "Zurück zum Launcher", "Volver al launcher", "Torna al launcher", "Terug naar launcher", "Voltar ao launcher", "Wróć do launchera", "Назад в лаунчер"});
        m.put("Making the light copy of the game (once)", new String[] {"Création de la copie légère du jeu (une fois)", "Leichte Kopie des Spiels wird erstellt (einmal)", "Creando la copia ligera del juego (una vez)", "Creazione della copia leggera del gioco (una volta)", "Lichte kopie van het spel maken (eenmalig)", "Criando a cópia leve do jogo (uma vez)", "Tworzenie lekkiej kopii gry (raz)", "Создание облегчённой копии игры (один раз)"});
        m.put("Change", new String[] {"Changer", "Ändern", "Cambiar", "Cambia", "Wijzigen", "Mudar", "Zmień", "Изменить"});
        m.put("Display Options", new String[] {"Options d'affichage", "Anzeigeoptionen", "Opciones de pantalla", "Opzioni schermo", "Beeldopties", "Opções de tela", "Opcje obrazu", "Настройки экрана"});
        m.put("Resolution", new String[] {"Résolution", "Auflösung", "Resolución", "Risoluzione", "Resolutie", "Resolução", "Rozdzielczość", "Разрешение"});
        m.put("Sound Options", new String[] {"Options sonores", "Tonoptionen", "Opciones de sonido", "Opzioni audio", "Geluidsopties", "Opções de som", "Opcje dźwięku", "Настройки звука"});
        m.put("Music Volume", new String[] {"Volume musique", "Musiklautstärke", "Volumen música", "Volume musica", "Muziekvolume", "Volume da música", "Głośność muzyki", "Громк. музыки"});
        m.put("SFX Volume", new String[] {"Volume effets", "Effektlautstärke", "Volumen efectos", "Volume effetti", "Effectvolume", "Volume dos efeitos", "Głośność efektów", "Громк. эффектов"});
        m.put("Accept", new String[] {"Accepter", "Annehmen", "Aceptar", "Accetta", "Accepteren", "Aceitar", "Akceptuj", "Принять"});
        m.put("Cancel", new String[] {"Annuler", "Abbrechen", "Cancelar", "Annulla", "Annuleren", "Cancelar", "Anuluj", "Отмена"});
        m.put("Settings saved", new String[] {"Réglages enregistrés", "Einstellungen gespeichert", "Ajustes guardados", "Impostazioni salvate", "Instellingen opgeslagen", "Configurações salvas", "Zapisano ustawienia", "Настройки сохранены"});
        m.put("Starting the game...", new String[] {"Lancement du jeu...", "Spiel startet...", "Iniciando el juego...", "Avvio del gioco...", "Spel wordt gestart...", "Iniciando o jogo...", "Uruchamianie gry...", "Запуск игры..."});
        m.put("Bye!", new String[] {"Au revoir !", "Tschüss!", "¡Adiós!", "Ciao!", "Doei!", "Tchau!", "Pa!", "Пока!"});
    }
    private MenuText() {}
}
