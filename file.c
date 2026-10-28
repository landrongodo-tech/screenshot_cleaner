#include <stdio.h>
#include <dirent.h>
#include <string.h> // Indispensable pour utiliser strcmp !
#include "file.h"


void scanner_dossier(const char *chemin_dossier) {
    DIR *dir = opendir(chemin_dossier);
    if (dir == NULL) {
        perror("Erreur lors de l'ouverture du dossier");
        return;
    }
    
    // Le dossier est ouvert avec succès !
    // ...
    
    closedir(dir); // On n'oublie jamais de fermer ce qu'on a ouvert !
}
void lecture_dossier(const char *chemin_dossier) {
    DIR *dir = opendir(chemin_dossier);
    if (dir == NULL) {
        perror("Erreur d'ouverture du dossier");
        return;
    }

    struct dirent *entree;

    while ((entree = readdir(dir)) != NULL) {
        // Ignorer les dossiers virtuels '.' et '..'
        if (strcmp(entree->d_name, ".") == 0 || strcmp(entree->d_name, "..") == 0) {
            continue; // On saute cette itération et on passe au suivant
        }
        // Si ce n'est ni un .png ni un .jpg, on ignore
        if (strstr(entree->d_name, ".png") == NULL && strstr(entree->d_name, ".jpg") == NULL) {
        continue;
        }

        printf("Fichiers trouvés : %s\n", entree->d_name);
    }

    closedir(dir);
}
