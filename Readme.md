# Screenshot Cleaner

Utilitaire Linux en C qui recense les images d'un dossier et affiche leur taille
et leur date de modification. Le parcours n'est pas récursif : les sous-dossiers
ne sont ni explorés ni supprimés.

## Compilation

Depuis ce dossier, installez d'abord les en-têtes ncurses (par exemple
`libncurses-dev` sur Debian/Ubuntu), puis compilez les trois fichiers C :

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic main.c file.c ui.c -lncurses -o cleaner
```

Le standard POSIX est activé dans les sources pour les appels système utilisés.

## Utilisation

```sh
# Afficher les images du dossier courant (comportement par défaut)
./cleaner

# Afficher les images d'un dossier choisi
./cleaner /chemin/vers/images

# Ouvrir le navigateur visuel interactif ncurses
./cleaner --ui /chemin/vers/images

# Demander une confirmation séparée avant chaque suppression
./cleaner --delete /chemin/vers/images

# Supprimer sans confirmation (option volontairement explicite)
./cleaner --delete --yes /chemin/vers/images
```

Les extensions reconnues sont `.png`, `.jpg` et `.jpeg`, sans distinction de
majuscules/minuscules. Seuls les fichiers réguliers sont affichés ; les liens
symboliques ne sont jamais suivis ni supprimés. Avant chaque suppression,
l'utilitaire vérifie que le nom correspond toujours au même inode que celui
observé lors du scan, puis emploie `unlinkat()` relativement au dossier ouvert.
Cette vérification réduit les risques liés au remplacement d'un fichier entre
le scan et l'action, mais comme avec les opérations de fichiers concurrentes,
elle ne constitue pas un verrou contre un autre processus modifiant le dossier
au même instant.

## Commandes de l'interface

- `Up` / `Down` ou `k` / `j` : déplacer la sélection.
- `d` : ouvrir une boîte de confirmation, puis supprimer l'image sélectionnée
  si `y` ou `o` est pressé.
- `r` : rescanner le dossier.
- `q` : quitter.

La suppression via l'interface ne lance jamais une suppression globale :
chaque fichier est confirmé individuellement.