#define _POSIX_C_SOURCE 200809L

#include "file.h"
#include "ui.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Une réponse affirmative est volontairement limitée à "o" ou "O".
 * Une entrée vide, "oui", une erreur de lecture ou toute autre réponse
 * conserve le fichier : en cas de doute, le programme ne supprime rien.
 */
static int demander_confirmation(const char *chemin)
{
    char reponse[32];

    printf("Supprimer %s ? [o/N] ", chemin);
    fflush(stdout);

    if (fgets(reponse, sizeof reponse, stdin) == NULL) {
        return 0;
    }

    /* Une ligne trop longue n'est jamais interprétée comme une confirmation. */
    if (strchr(reponse, '\n') == NULL) {
        int caractere;
        while ((caractere = getchar()) != '\n' && caractere != EOF) {
            /* Consommer le reste de la ligne pour la prochaine lecture. */
        }
        return 0;
    }

    return reponse[0] == 'o' || reponse[0] == 'O';
}

static void afficher_image(const struct Image *image)
{
    char date_formatee[64];
    struct tm heure_locale;

    printf("- %s\n  Taille : %" PRIuMAX " octet(s)\n  Modification : ",
           image->chemin, image->taille);

    if (localtime_r(&image->date, &heure_locale) != NULL
        && strftime(date_formatee, sizeof date_formatee,
                    "%Y-%m-%d %H:%M:%S", &heure_locale) > 0) {
        printf("%s\n", date_formatee);
    } else {
        /*
         * L'horodatage Unix reste utile si sa conversion en heure locale échoue.
         * Il s'agit du nombre de secondes depuis le 1er janvier 1970 UTC.
         */
        fprintf(stderr, "Impossible de formater la date de %s\n", image->chemin);
        printf("horodatage Unix %jd\n", (intmax_t)image->date);
    }
}

static void afficher_aide(const char *nom_programme)
{
    printf("Usage : %s [--ui | --delete [--yes]] [DOSSIER]\n"
           "\n"
           "Sans option, affiche les images trouvées dans DOSSIER (par défaut : .).\n"
           "--ui      ouvre le navigateur interactif ncurses.\n"
           "--delete  demande une confirmation individuelle avant chaque suppression.\n"
           "--yes     avec --delete, supprime sans confirmation toutes les images\n"
           "          régulières reconnues dans le dossier, sans parcourir les sous-dossiers.\n"
           "--help    affiche cette aide.\n",
           nom_programme);
}

int main(int argc, char **argv)
{
    const char *chemin_dossier = ".";
    int suppression_demandee = 0;
    int suppression_sans_confirmation = 0;
    int interface_demandee = 0;
    int dossier_deja_fourni = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            afficher_aide(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "--delete") == 0) {
            suppression_demandee = 1;
            continue;
        }

        if (strcmp(argv[i], "--ui") == 0) {
            interface_demandee = 1;
            continue;
        }

        if (strcmp(argv[i], "--yes") == 0) {
            suppression_sans_confirmation = 1;
            continue;
        }

        if (argv[i][0] == '-') {
            fprintf(stderr, "Option inconnue : %s\n", argv[i]);
            afficher_aide(argv[0]);
            return 2;
        }

        if (dossier_deja_fourni) {
            fprintf(stderr, "Un seul dossier peut être indiqué.\n");
            afficher_aide(argv[0]);
            return 2;
        }

        chemin_dossier = argv[i];
        dossier_deja_fourni = 1;
    }

    if (suppression_sans_confirmation && !suppression_demandee) {
        fprintf(stderr, "--yes ne peut être utilisé qu'avec --delete.\n");
        afficher_aide(argv[0]);
        return 2;
    }

    if (interface_demandee && suppression_demandee) {
        fprintf(stderr, "--ui ne peut pas être combiné avec --delete.\n");
        afficher_aide(argv[0]);
        return 2;
    }

    /*
     * ncurses doit contrôler un terminal réel à la fois pour l'affichage et
     * pour les touches. Ne pas initialiser l'interface si l'un des flux est
     * redirigé : le message d'erreur reste alors lisible dans le terminal.
     */
    if (interface_demandee
        && (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))) {
        fprintf(stderr, "--ui nécessite un terminal interactif.\n");
        return 2;
    }

    /*
     * En mode interactif, refuser stdin non relié à un terminal. Cela évite
     * qu'un flux redirigé ou fermé ne produise une séquence de suppressions
     * dont l'utilisateur n'a pas vu les demandes de confirmation.
     */
    if (suppression_demandee && !suppression_sans_confirmation
        && !isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "La confirmation nécessite un terminal ; utilisez --yes "
                "pour demander explicitement une suppression sans confirmation.\n");
        return 2;
    }

    struct Image *images = NULL;
    if (scanner_dossier(chemin_dossier, &images) == -1) {
        return 1;
    }

    if (interface_demandee) {
        const int resultat = lancer_interface_tui(chemin_dossier, &images);
        liberer_images(images);
        return resultat == 0 ? 0 : 1;
    }

    if (images == NULL) {
        printf("Aucune image trouvée dans %s.\n", chemin_dossier);
        return 0;
    }

    size_t nombre_images = 0;
    for (const struct Image *image = images; image != NULL;
         image = image->suivant) {
        afficher_image(image);
        ++nombre_images;
    }

    printf("%zu image(s) trouvée(s).\n", nombre_images);

    int nombre_suppressions = 0;
    int erreur_suppression = 0;

    if (suppression_demandee) {
        for (const struct Image *image = images; image != NULL;
             image = image->suivant) {
            if (!suppression_sans_confirmation
                && !demander_confirmation(image->chemin)) {
                printf("Conservé : %s\n", image->chemin);
                continue;
            }

            if (supprimer_image(chemin_dossier, image) == -1) {
                fprintf(stderr, "Échec de suppression de %s : ",
                        image->chemin);
                perror("supprimer_image");
                erreur_suppression = 1;
                continue;
            }

            printf("Supprimé : %s\n", image->chemin);
            ++nombre_suppressions;
        }

        printf("%d image(s) supprimée(s).\n", nombre_suppressions);
    }

    liberer_images(images);
    return erreur_suppression ? 1 : 0;
}
