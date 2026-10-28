# Screenshot Cleaner

Un utilitaire en langage C développé sous Linux pour scanner un répertoire, identifier automatiquement les fichiers d'images (comme les captures d'écran `.png` ou `.jpg`), et préparer leur gestion (filtrage et nettoyage).

## 🎯 Objectif du projet
Créer un outil système performant et modulaire pour automatiser le tri et le nettoyage de dossiers encombrés de captures d'écran ou d'images superflues, tout en explorant les concepts bas niveau du langage C (pointeurs, manipulation de structures, gestion des flux de dossiers).

## 🚀 Fonctionnalités actuelles
- **Architecture modulaire :** Séparation propre du code source (`main.c`, `file.c`, `file.h`).
- **Exploration de dossiers :** Utilisation des bibliothèques système `<dirent.h>` (`opendir`, `readdir`) pour parcourir le contenu d'un répertoire.
- **Filtrage intelligent :**
  - Exclusion automatique des répertoires virtuels (`.` et `..`) via `strcmp`.
  - Sélection ciblée des fichiers d'images (`.png`, `.jpg`) via `strstr`.

## 🛠️ Prérequis
- Un compilateur C (comme `gcc`).
- Un environnement Linux / Unix.

## ⚙️ Compilation
Pour compiler l'ensemble des fichiers du projet, exécute la commande suivante dans ton terminal :

```bash
gcc main.c file.c -o cleaner

Lance l'exécutable pour scanner le répertoire courant :
Bash

./cleaner

🗺️ Prochaines étapes

    [ ] Stocker les métadonnées des images (taille, date de modification) grâce à la fonction système stat.

    [ ] Implémenter la logique de tri ou de suppression sécurisée des fichiers sélectionnés.