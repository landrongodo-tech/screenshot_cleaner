# Cleaner

Cleaner est un assistant natif Linux en C/GTK 3 pour trier chaque jour un petit
flux d'images. Le parcours du dossier est non récursif. Le backend (`file.c`,
`file.h`) reste seul responsable du scan, des métadonnées et de la suppression
sécurisée.

## Compilation et installation

Sur Debian ou Ubuntu, installez les outils et bibliothèques nécessaires :

```sh
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev
```

Depuis ce répertoire :

```sh
make
./cleaner                         # captures par défaut, ou sélection de dossier
./cleaner /chemin/vers/images     # dossier choisi au lancement
```

`make` utilise `pkg-config` pour GTK 3 et GdkPixbuf et applique les options
`-Wall -Wextra -Wpedantic`. Pour installer le binaire, l'entrée de menu GNOME
et les modèles de service/timer systemd utilisateur :

```sh
sudo make install
```

L'installation utilise `/usr/local` par défaut. Pour choisir un autre préfixe,
par exemple `/usr`, utilisez `sudo make install PREFIX=/usr`. Pour désinstaller :

```sh
sudo make uninstall
```

`ui.c` et `ui.h` sont l'ancienne interface ncurses conservée comme référence ;
ils ne sont volontairement pas inclus dans le binaire GTK.

## Premier lancement et préférences

Au premier lancement, un assistant demande :

- l'heure locale du rituel quotidien (format 24 heures) ;
- le quota d'images à traiter par session, de 5 à 50 ;
- le dossier d'images à analyser ;
- le déplacement définitif ou vers la Corbeille système ;
- le comportement du bouton **Reporter / Pause** et son délai.

Les réglages, le dossier choisi, la progression de la session et l'échéance d'un
report sont enregistrés dans
`$XDG_CONFIG_HOME/cleaner/config.conf`, ou dans
`$HOME/.config/cleaner/config.conf` si `XDG_CONFIG_HOME` n'est pas défini. Un
ancien `settings.ini` est migré automatiquement au premier démarrage. Le
chemin du domicile vient de l'environnement du compte connecté ; aucun chemin
personnel n'est incorporé au programme. Le répertoire est créé avec des
permissions privées et le fichier de réglages est protégé en lecture/écriture
pour le compte utilisateur. **Réglages** permet de modifier ces préférences.
Passer un dossier en argument le choisit et le mémorise pour les prochains
lancements.

Sans argument (notamment depuis l'icône du bureau ou le timer systemd), Cleaner
cible le sous-dossier `Captures d'écran` du répertoire Images configuré par
l'environnement de bureau. Si ce dossier n'existe pas, un sélecteur de dossier
natif GTK s'ouvre pour laisser l'utilisateur en choisir un. Un dossier vide
affiche un message dans la fenêtre ; un dossier illisible produit une erreur
visible sans fermer l'application.

## Utilisation du flux

Cleaner affiche une image à la fois. Il reconnaît les fichiers réguliers dont
l'extension est `.png`, `.jpg` ou `.jpeg` (majuscules acceptées), sans suivre
les liens symboliques ni descendre dans les sous-dossiers.

- **Garder** marque l'image comme traitée et passe à la suivante.
- **Supprimer** applique le mode choisi dans Réglages : suppression définitive
  par `unlinkat()` après vérification de l'inode, ou transfert dans la Corbeille
  XDG via GIO.
- **Reporter / Pause** ferme la session, reporte automatiquement selon le délai
  configuré, ou demande à l'utilisateur de choisir entre ces deux actions.
- **Actualiser** rescane le dossier mémorisé ; **Choisir un dossier** change le
  flux ; **Réglages** modifie le rituel et le quota.

Garder une image ou réussir à la supprimer consomme une unité du quota de la
session.
L'application affiche un écran de félicitations à l'atteinte du quota. Si le
dossier est épuisé avant, il est possible de l'actualiser ou d'en choisir un
autre. Une image illisible par GdkPixbuf reste traitable : son aperçu est
remplacé par un message, mais les actions Garder et Supprimer restent proposées.

## Lancement automatique à l'heure du rituel

L'installation fournit un service systemd utilisateur et un timer d'exemple
(`cleaner.service` et `cleaner.timer`). Activez le timer une seule fois :

```sh
systemctl --user daemon-reload
systemctl --user enable --now cleaner.timer
systemctl --user list-timers cleaner.timer
```

Le timer vérifie l'heure configurée chaque minute. Le service appelle
`cleaner --scheduled` : le programme lit les préférences de l'utilisateur et
n'ouvre GTK qu'à l'heure locale enregistrée dans **Réglages**. Après une
modification de l'heure, aucune édition du fichier systemd n'est nécessaire.
Avant le premier lancement interactif, le timer n'ouvre pas l'assistant ; lancez
`cleaner` une fois pour initialiser l'heure et le quota.

Sur une session de bureau GNOME, le service doit hériter de l'environnement
graphique. En général, GNOME le publie déjà au gestionnaire systemd utilisateur.
Pour installer une copie personnelle de l'unité dans
`~/.config/systemd/user/cleaner.service`, copiez l'unité fournie après avoir
installé Cleaner. `XDG_RUNTIME_DIR=%t` est résolu par systemd vers le répertoire
runtime du compte ; `DISPLAY=:0` est un exemple X11 et doit correspondre à
l'affichage réellement utilisé :

```ini
[Unit]
Description=Cleaner - rituel quotidien de tri des images
After=graphical-session.target

[Service]
Type=oneshot
Environment=DISPLAY=:0
Environment=XDG_RUNTIME_DIR=%t
ExecStart=/usr/local/bin/cleaner --scheduled
```

Le chemin de `ExecStart` doit désigner le binaire installé. Pour laisser le
gestionnaire systemd utilisateur transmettre l'environnement exact de la session
(recommandé en particulier sous Wayland), ouvrez un terminal dans la session
graphique et importez ses variables :

```sh
systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XAUTHORITY \
    DBUS_SESSION_BUS_ADDRESS
```

Copiez aussi le timer dans le répertoire utilisateur puis rechargez les unités :

```sh
install -Dm 0644 systemd/cleaner.service \
    "$HOME/.config/systemd/user/cleaner.service"
install -Dm 0644 systemd/cleaner.timer \
    "$HOME/.config/systemd/user/cleaner.timer"
systemctl --user daemon-reload
systemctl --user enable --now cleaner.timer
```

La copie sous `~/.config/systemd/user` prend priorité sur une unité installée
sous `/usr/local/lib/systemd/user` ; gardez les deux copies synchronisées si
vous utilisez cette installation personnelle.
`DISPLAY=:0` n'est pas universel : vérifiez la valeur avec `printf '%s\n' "$DISPLAY"`
dans la session graphique et adaptez l'unité, ou laissez systemd hériter de la
valeur importée.

Le compte utilisateur doit être connecté à sa session graphique à l'heure du
timer ; ce n'est pas un service système destiné à ouvrir une session graphique
depuis un écran de connexion. `Persistent=true` demande à systemd de rattraper
un déclenchement manqué (l'application peut donc s'ouvrir à la connexion
suivante). Pour contrôler les erreurs :

```sh
journalctl --user -u cleaner.service
```

Systemd déclenche un contrôle bref chaque minute ; c'est la préférence persistée
dans Cleaner qui décide quand ouvrir l'interface. L'application doit donc être
installée et le timer utilisateur activé pour bénéficier du lancement
automatique.
