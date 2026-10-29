#include "gui.h"

#include <stdio.h>
#include <string.h>

/*
 * Sans dossier explicite, NULL laisse GTK résoudre le dossier propre à
 * l'utilisateur ou ouvrir le sélecteur. --scheduled est réservé à systemd :
 * le module GTK vérifie alors l'heure persistée avant d'afficher la fenêtre.
 */
int main(int argc, char **argv)
{
    const char *dossier = NULL;
    int lancement_programme = 0;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage : %s [--scheduled] [DOSSIER]\n"
               "Lance le rituel GTK de tri des images de DOSSIER.\n"
               "Au premier lancement, Cleaner demande l'heure quotidienne et le quota.\n"
               "Sans DOSSIER, Cleaner utilise le dossier standard de captures "
               "d'écran\n"
               "ou demande d'en choisir un s'il n'existe pas.\n"
               "--scheduled est réservé au timer systemd utilisateur.\n",
               argv[0]);
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--scheduled") == 0) {
            if (lancement_programme) {
                fprintf(stderr, "Option --scheduled répétée.\n");
                return 2;
            }
            lancement_programme = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Option inconnue : %s\n"
                            "Usage : %s [--scheduled] [DOSSIER]\n",
                    argv[i], argv[0]);
            return 2;
        } else if (dossier == NULL) {
            dossier = argv[i];
        } else {
            fprintf(stderr, "Un seul dossier peut être indiqué.\n"
                            "Usage : %s [--scheduled] [DOSSIER]\n",
                    argv[0]);
            return 2;
        }
    }

    return lancer_interface_graphique(dossier, lancement_programme);
}
