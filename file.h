#ifndef FILE_H
#define FILE_H

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/*
 * Une Image représente une entrée de fichier trouvée dans le dossier scanné.
 *
 * Les chaînes chemin et nom sont allouées dynamiquement : contrairement à des
 * tableaux de taille arbitraire (par exemple char chemin[256]), elles peuvent
 * contenir les chemins autorisés par le système sans être tronquées.
 *
 * Les champs appareil et inode permettent de vérifier, avant une suppression,
 * que le nom désigne toujours le même fichier que celui qui a été scanné.
 * suivant relie cette structure à la suivante : l'ensemble forme une liste
 * simplement chaînée, libérée avec liberer_images().
 */
struct Image {
    char *chemin;
    char *nom;
    uintmax_t taille;
    time_t date;
    dev_t appareil;
    ino_t inode;
    struct Image *suivant;
};

/*
 * Scanne un seul niveau du dossier, sans descendre dans les sous-dossiers.
 * À la réussite, *images reçoit la tête de la liste (ou NULL si elle est vide)
 * et la fonction renvoie 0. En cas d'erreur, la fonction renvoie -1, affiche
 * le motif système avec perror() et libère toute liste partiellement construite.
 */
int scanner_dossier(const char *chemin_dossier, struct Image **images);

/* Libère les chaînes et les structures de chaque maillon de la liste. */
void liberer_images(struct Image *images);

/*
 * Supprime uniquement l'entrée régulière dont le nom, l'appareil et l'inode
 * correspondent à l'image scannée. Le nom est passé à unlinkat() relativement
 * au dossier ouvert : un chemin stocké dans la structure n'est jamais utilisé
 * comme chemin de suppression.
 *
 * Renvoie 0 en cas de succès et -1 en cas d'erreur (motif accessible via errno).
 * La confirmation interactive est laissée à l'appelant, avant cet appel.
 */
int supprimer_image(const char *chemin_dossier, const struct Image *image);

#endif
