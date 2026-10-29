#define _POSIX_C_SOURCE 200809L

#include "file.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Le suffixe doit être exactement une extension d'image reconnue. Ainsi,
 * "capture.png.txt" ou "pas-une-image.png.backup" ne passent pas le filtre.
 * strcasecmp() accepte aussi les variantes majuscules comme ".PNG".
 */
static int est_une_image(const char *nom)
{
    const char *extension = strrchr(nom, '.');

    if (extension == NULL) {
        return 0;
    }

    return strcasecmp(extension, ".png") == 0
        || strcasecmp(extension, ".jpg") == 0
        || strcasecmp(extension, ".jpeg") == 0;
}

/*
 * Construit un chemin d'affichage complet dans un nouveau bloc mémoire.
 * Le résultat appartient à l'appelant, qui devra appeler free().
 */
static char *construire_chemin(const char *dossier, const char *nom)
{
    const size_t longueur_dossier = strlen(dossier);
    const size_t longueur_nom = strlen(nom);
    const int ajouter_separateur =
        longueur_dossier > 0 && dossier[longueur_dossier - 1] != '/';

    /* Vérifier chaque addition évite un débordement de taille avant malloc(). */
    if (longueur_dossier > SIZE_MAX - longueur_nom
        || longueur_dossier + longueur_nom > SIZE_MAX
            - (size_t)ajouter_separateur - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    const size_t taille = longueur_dossier + longueur_nom
        + (size_t)ajouter_separateur + 1;
    char *chemin = malloc(taille);

    if (chemin == NULL) {
        return NULL;
    }

    memcpy(chemin, dossier, longueur_dossier);
    size_t position = longueur_dossier;

    if (ajouter_separateur) {
        chemin[position++] = '/';
    }

    memcpy(chemin + position, nom, longueur_nom + 1);
    return chemin;
}

void liberer_images(struct Image *images)
{
    /*
     * On mémorise le maillon suivant avant de libérer le maillon courant :
     * après free(image), lire image->suivant serait un accès mémoire invalide.
     */
    while (images != NULL) {
        struct Image *suivant = images->suivant;
        free(images->chemin);
        free(images->nom);
        free(images);
        images = suivant;
    }
}

int scanner_dossier(const char *chemin_dossier, struct Image **images)
{
    if (chemin_dossier == NULL || images == NULL) {
        errno = EINVAL;
        perror("scanner_dossier");
        return -1;
    }

    /* Même en cas d'erreur, l'appelant reçoit un pointeur de sortie défini. */
    *images = NULL;

    DIR *dossier = opendir(chemin_dossier);
    if (dossier == NULL) {
        perror("opendir");
        return -1;
    }

    /*
     * dirfd() récupère le descripteur du dossier déjà ouvert. fstatat() pourra
     * ainsi consulter chaque entrée relativement à ce dossier, sans assembler
     * un chemin temporaire pouvant être ambigu ou modifié.
     */
    const int descripteur_dossier = dirfd(dossier);
    if (descripteur_dossier == -1) {
        const int erreur = errno;
        perror("dirfd");
        (void)closedir(dossier);
        errno = erreur;
        return -1;
    }

    struct Image *tete = NULL;
    struct Image *queue = NULL;
    int erreur_scan = 0;

    for (;;) {
        /*
         * readdir() renvoie NULL aussi bien à la fin qu'en cas d'erreur.
         * Mettre errno à zéro avant l'appel permet de distinguer ces cas.
         */
        errno = 0;
        struct dirent *entree = readdir(dossier);

        if (entree == NULL) {
            if (errno != 0) {
                perror("readdir");
                erreur_scan = errno;
            }
            break;
        }

        /* Les noms "." et ".." désignent le dossier courant et son parent. */
        if (strcmp(entree->d_name, ".") == 0
            || strcmp(entree->d_name, "..") == 0) {
            continue;
        }

        if (!est_une_image(entree->d_name)) {
            continue;
        }

        struct stat informations;
        /*
         * AT_SYMLINK_NOFOLLOW nous donne les informations du lien lui-même
         * plutôt que celles de sa cible. Nous n'acceptons ensuite que les
         * fichiers réguliers : pas de sous-dossiers, sockets ou liens.
         */
        if (fstatat(descripteur_dossier, entree->d_name, &informations,
                    AT_SYMLINK_NOFOLLOW) == -1) {
            fprintf(stderr, "Impossible d'examiner %s/%s: ",
                    chemin_dossier, entree->d_name);
            perror("fstatat");
            erreur_scan = errno;
            break;
        }

        if (!S_ISREG(informations.st_mode)) {
            continue;
        }

        if (informations.st_size < 0) {
            fprintf(stderr, "Taille invalide pour %s/%s\n",
                    chemin_dossier, entree->d_name);
            erreur_scan = EOVERFLOW;
            break;
        }

        struct Image *image = malloc(sizeof *image);
        if (image == NULL) {
            perror("malloc");
            erreur_scan = errno;
            break;
        }

        image->chemin = construire_chemin(chemin_dossier, entree->d_name);
        image->nom = strdup(entree->d_name);
        image->taille = (uintmax_t)informations.st_size;
        image->date = informations.st_mtime;
        image->appareil = informations.st_dev;
        image->inode = informations.st_ino;
        image->suivant = NULL;

        if (image->chemin == NULL || image->nom == NULL) {
            const int erreur = errno;
            free(image->chemin);
            free(image->nom);
            free(image);
            errno = erreur;
            perror("allocation de l'image");
            erreur_scan = erreur;
            break;
        }

        /*
         * Ajouter en queue conserve l'ordre de parcours de readdir() sans
         * inverser la liste. La tête reste le pointeur vers son premier maillon.
         */
        if (tete == NULL) {
            tete = image;
        } else {
            queue->suivant = image;
        }
        queue = image;
    }

    if (closedir(dossier) == -1 && erreur_scan == 0) {
        perror("closedir");
        erreur_scan = errno;
    }

    if (erreur_scan != 0) {
        liberer_images(tete);
        errno = erreur_scan;
        return -1;
    }

    *images = tete;
    return 0;
}

int supprimer_image(const char *chemin_dossier, const struct Image *image)
{
    if (chemin_dossier == NULL || image == NULL || image->nom == NULL
        || strchr(image->nom, '/') != NULL
        || strcmp(image->nom, ".") == 0
        || strcmp(image->nom, "..") == 0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * O_DIRECTORY empêche l'ouverture accidentelle d'un fichier ordinaire.
     * O_CLOEXEC évite de transmettre ce descripteur à un éventuel programme
     * lancé ensuite par le processus.
     */
    const int descripteur_dossier =
        open(chemin_dossier, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descripteur_dossier == -1) {
        return -1;
    }

    struct stat informations_actuelles;
    if (fstatat(descripteur_dossier, image->nom, &informations_actuelles,
                AT_SYMLINK_NOFOLLOW) == -1) {
        const int erreur = errno;
        (void)close(descripteur_dossier);
        errno = erreur;
        return -1;
    }

    /*
     * Un autre programme peut avoir remplacé l'entrée depuis le scan.
     * Comparer type, appareil et inode évite de supprimer un nouveau fichier
     * portant simplement le même nom. Le lien symbolique n'est jamais suivi.
     */
    if (!S_ISREG(informations_actuelles.st_mode)
        || informations_actuelles.st_dev != image->appareil
        || informations_actuelles.st_ino != image->inode
        || !est_une_image(image->nom)) {
        (void)close(descripteur_dossier);
        errno = ESTALE;
        return -1;
    }

    /*
     * unlinkat() reçoit un simple nom relatif au descripteur du dossier.
     * Il ne suit pas les liens symboliques : il retire l'entrée elle-même.
     */
    const int resultat = unlinkat(descripteur_dossier, image->nom, 0);
    const int erreur = errno;

    if (close(descripteur_dossier) == -1 && resultat == 0) {
        return -1;
    }

    errno = erreur;
    return resultat;
}
