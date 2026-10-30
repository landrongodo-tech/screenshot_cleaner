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

L'automatisation est initialisée automatiquement lorsque l'utilisateur valide
les réglages dans la fenêtre. Cleaner enregistre d'abord ses préférences, puis
crée ou met à jour `cleaner.service` et `cleaner.timer` dans le répertoire
systemd utilisateur, importe l'environnement graphique courant et demande à
systemd de recharger et d'activer le timer. Aucun passage par le terminal n'est
nécessaire. Si systemd ne peut pas appliquer l'automatisation, Cleaner affiche
une erreur ; les préférences restent enregistrées.

Le timer vérifie l'heure configurée chaque minute. Le service appelle
`cleaner --scheduled`, qui lit l'heure et le report enregistrés dans
`config.conf` et n'ouvre GTK que lorsque le rituel est dû. Cette vérification
par minute permet également de respecter les reports de quelques minutes.
Modifier l'heure dans **Réglages** ne nécessite donc pas de réécrire une
planification quotidienne : la nouvelle préférence est utilisée par le prochain
contrôle.

Le compte utilisateur doit être connecté à sa session graphique à l'heure du
timer ; il ne s'agit pas d'un service système destiné à ouvrir une session
graphique depuis un écran de connexion. Avant le premier lancement interactif,
le timer n'ouvre pas l'assistant : il faut d'abord lancer Cleaner et enregistrer
les réglages dans la fenêtre. `Persistent=true` demande à systemd de rattraper
un déclenchement manqué (l'application peut donc s'ouvrir à la connexion
suivante).

Pour diagnostiquer une erreur, les journaux utilisateur systemd contiennent le
détail du service :

```sh
journalctl --user -u cleaner.service
```
