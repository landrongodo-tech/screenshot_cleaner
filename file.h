#ifndef FILE_H
#define FILE_H

#include <time.h> // Pour le type time_t
#include <stddef.h> // Pour size_t

struct Image {
    char chemin[256];
    char nom[256];
    size_t taille;
    time_t date;
};

// Prototype de la fonction (la vitrine)
void lecture_dossier(const char *chemin_dossier);

#endif