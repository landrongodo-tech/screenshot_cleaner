#define _POSIX_C_SOURCE 200809L

#include "ui.h"

#include <errno.h>
#include <inttypes.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* La première ligne de la liste reste visible après chaque déplacement. */
#define LIGNE_DEBUT_LISTE 3

/*
 * Formate la date dans un tampon fourni par l'appelant.
 * localtime_r() est utilisé plutôt que localtime() car il écrit dans une
 * structure tm fournie par le programme, sans tampon statique partagé.
 */
static void formater_date(time_t date, char *tampon, size_t taille)
{
    struct tm heure_locale;

    if (localtime_r(&date, &heure_locale) == NULL
        || strftime(tampon, taille, "%Y-%m-%d %H:%M:%S",
                    &heure_locale) == 0) {
        (void)snprintf(tampon, taille, "date indisponible");
    }
}

/*
 * Maintient l'index sélectionné dans la zone visible.
 * Une liste trop longue est parcourue par pages sans allouer de copie :
 * l'interface garde uniquement un index et suit les maillons de la liste.
 */
static void ajuster_defilement(size_t selection, size_t nombre,
                               int hauteur_liste, size_t *premier_visible)
{
    if (nombre == 0 || hauteur_liste <= 0) {
        *premier_visible = 0;
        return;
    }

    if (selection < *premier_visible) {
        *premier_visible = selection;
    } else if (selection >= *premier_visible + (size_t)hauteur_liste) {
        *premier_visible = selection - (size_t)hauteur_liste + 1;
    }
}

/*
 * Accède au maillon d'index donné.
 * L'index de sélection n'est pas un pointeur vers l'écran : il sert à parcourir
 * la liste depuis sa tête, ce qui évite de conserver un pointeur invalide après
 * une mise à jour de la liste.
 */
static struct Image *image_a_l_index(struct Image *images, size_t index)
{
    while (images != NULL && index > 0) {
        images = images->suivant;
        --index;
    }
    return images;
}

/*
 * Dessine une frame complète à partir de l'état courant.
 * erase() efface le tampon curses, puis refresh() transmet la frame au terminal
 * en une seule fois : cela limite le scintillement produit par plusieurs
 * écritures successives.
 */
static void dessiner_interface(const char *dossier, struct Image *images,
                               size_t nombre, size_t selection,
                               size_t premier_visible, const char *statut)
{
    int lignes;
    int colonnes;
    getmaxyx(stdscr, lignes, colonnes);
    erase();

    if (lignes < 11 || colonnes < 40) {
        mvaddnstr(0, 0, "Terminal trop petit (minimum : 40 colonnes x 11 lignes).",
                  colonnes > 0 ? colonnes - 1 : 0);
        if (lignes > 1) {
            mvaddnstr(lignes - 1, 0, "Agrandissez le terminal ou appuyez sur q.",
                      colonnes > 0 ? colonnes - 1 : 0);
        }
        refresh();
        return;
    }

    mvaddnstr(0, 0, "CLEANER - navigateur d'images", colonnes - 1);
    mvaddnstr(1, 0, "Dossier : ", colonnes - 1);
    mvaddnstr(1, 10, dossier, colonnes > 10 ? colonnes - 11 : 0);
    mvhline(2, 0, ACS_HLINE, colonnes);

    const int hauteur_liste = lignes - 10;

    if (nombre == 0) {
        mvaddnstr(LIGNE_DEBUT_LISTE, 0,
                  "Aucune image trouvee. Appuyez sur r pour rescanner.",
                  colonnes - 1);
    } else {
        size_t index = premier_visible;
        struct Image *image = image_a_l_index(images, premier_visible);
        int ligne = LIGNE_DEBUT_LISTE;

        while (ligne < LIGNE_DEBUT_LISTE + hauteur_liste
               && index < nombre && image != NULL) {
            char texte[1024];
            const char marque = index == selection ? '>' : ' ';
            const int longueur = snprintf(
                texte, sizeof texte, "%c %s  (%" PRIuMAX " octet(s))",
                marque, image->nom, image->taille);

            if (index == selection) {
                attron(A_REVERSE);
            }
            if (longueur >= 0) {
                mvaddnstr(ligne, 0, texte, colonnes - 1);
            }
            if (index == selection) {
                attroff(A_REVERSE);
            }

            ++ligne;
            ++index;
            image = image->suivant;
        }
    }

    mvhline(lignes - 7, 0, ACS_HLINE, colonnes);
    mvaddnstr(lignes - 6, 0, "Details de la selection", colonnes - 1);

    if (nombre > 0) {
        struct Image *image = image_a_l_index(images, selection);
        if (image != NULL) {
            char date[64];
            char detail[1024];
            formater_date(image->date, date, sizeof date);

            (void)snprintf(detail, sizeof detail,
                           "Taille : %" PRIuMAX " octet(s) | Modifie : %s",
                           image->taille, date);
            mvaddnstr(lignes - 5, 0, detail, colonnes - 1);
            mvaddnstr(lignes - 4, 0, image->chemin, colonnes - 1);
        }
    } else {
        mvaddnstr(lignes - 5, 0, "Aucun fichier a afficher.", colonnes - 1);
    }

    mvaddnstr(lignes - 3, 0, statut, colonnes - 1);
    mvhline(lignes - 2, 0, ACS_HLINE, colonnes);
    mvaddnstr(lignes - 1, 0,
              "Up/Down ou j/k : naviguer | d : supprimer | r : rescanner | q : quitter",
              colonnes - 1);
    refresh();
}

/*
 * Affiche une boîte de dialogue et attend une seule touche.
 * wgetch() lit une touche décodée par ncurses (y compris les touches spéciales)
 * sans écho automatique : la touche ne s'imprime pas dans la fenêtre.
 */
static int confirmer_suppression(const struct Image *image)
{
    int lignes;
    int colonnes;
    getmaxyx(stdscr, lignes, colonnes);

    const int largeur = colonnes - 4 < 64 ? colonnes - 4 : 64;
    const int hauteur = 5;

    /* Une fenêtre trop petite ne peut pas afficher une validation lisible. */
    if (largeur < 12 || lignes < hauteur) {
        return 0;
    }

    const int origine_y = (lignes - hauteur) / 2;
    const int origine_x = (colonnes - largeur) / 2;

    WINDOW *popup = newwin(hauteur, largeur, origine_y, origine_x);
    if (popup == NULL) {
        errno = ENOMEM;
        return -1;
    }

    /*
     * La chaîne est tronquée à la largeur intérieure de la boîte pour que les
     * chemins longs ne débordent pas sur les bordures ncurses.
     */
    box(popup, 0, 0);
    mvwaddnstr(popup, 1, 2, "Supprimer definitivement cette image ?", largeur - 4);
    mvwaddnstr(popup, 2, 2, image->nom, largeur - 4);
    mvwaddnstr(popup, 3, 2, "y/o : confirmer | toute autre touche : annuler",
               largeur - 4);
    wrefresh(popup);

    const int touche = wgetch(popup);
    delwin(popup);

    if (touche == ERR) {
        errno = EIO;
        return -1;
    }

    return touche == 'y' || touche == 'Y' || touche == 'o' || touche == 'O';
}

/*
 * Retire un maillon déjà supprimé du système de fichiers et libère ses
 * allocations. Le double pointeur permet de modifier le lien qui pointe vers
 * le maillon : cela couvre aussi le cas où il s'agit de la tête de liste.
 */
static void retirer_image_de_liste(struct Image **images,
                                   struct Image *image_a_retirer)
{
    struct Image **lien = images;

    while (*lien != NULL && *lien != image_a_retirer) {
        lien = &(*lien)->suivant;
    }

    if (*lien == image_a_retirer) {
        *lien = image_a_retirer->suivant;
        free(image_a_retirer->chemin);
        free(image_a_retirer->nom);
        free(image_a_retirer);
    }
}

/*
 * Boucle principale ncurses.
 * Elle ne remplace pas le moteur : les scans et suppressions sont délégués aux
 * fonctions métier scanner_dossier() et supprimer_image().
 */
int lancer_interface_tui(const char *chemin_dossier, struct Image **images)
{
    if (chemin_dossier == NULL || images == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * ncurses prend temporairement possession des réglages d'affichage du
     * terminal. initscr() crée la fenêtre standard et endwin() restaurera ces
     * réglages, y compris au moment de quitter l'interface.
     */
    if (initscr() == NULL) {
        fprintf(stderr, "Impossible d'initialiser ncurses.\n");
        return -1;
    }

    /*
     * Ces réglages sont nécessaires au modèle d'interaction. En cas d'échec,
     * endwin() doit quand même être appelé pour restituer l'état du terminal.
     */
    if (cbreak() == ERR || noecho() == ERR || keypad(stdscr, TRUE) == ERR) {
        (void)endwin();
        fprintf(stderr, "Impossible de configurer le terminal pour ncurses.\n");
        return -1;
    }
    (void)curs_set(0); /* Le curseur texte n'est pas utile dans cette UI. */

    size_t nombre = 0;
    for (struct Image *image = *images; image != NULL; image = image->suivant) {
        ++nombre;
    }

    size_t selection = 0;
    size_t premier_visible = 0;
    char statut[256] = "Pret. La suppression demande une confirmation.";
    int erreur_terminal = 0;
    int continuer = 1;

    while (continuer) {
        int lignes;
        int colonnes;
        getmaxyx(stdscr, lignes, colonnes);
        (void)colonnes;
        ajuster_defilement(selection, nombre, lignes - 10, &premier_visible);
        dessiner_interface(chemin_dossier, *images, nombre, selection,
                           premier_visible, statut);

        const int touche = getch();
        if (touche == ERR) {
            erreur_terminal = 1;
            continuer = 0;
            continue;
        }

        /*
         * Les touches fléchées dépendent de keypad(stdscr, TRUE); j/k offrent
         * une alternative familière aux utilisateurs de terminaux.
         */
        if (touche == KEY_UP || touche == 'k') {
            if (selection > 0) {
                --selection;
            }
        } else if (touche == KEY_DOWN || touche == 'j') {
            if (selection + 1 < nombre) {
                ++selection;
            }
        } else if (touche == 'q' || touche == 'Q') {
            continuer = 0;
        } else if (touche == 'd' || touche == 'D') {
            struct Image *image = image_a_l_index(*images, selection);

            if (image == NULL) {
                (void)snprintf(statut, sizeof statut,
                               "Aucune image selectionnee.");
            } else {
                const int confirmation = confirmer_suppression(image);

                if (confirmation < 0) {
                    const int erreur = errno;
                    (void)snprintf(statut, sizeof statut,
                                   "Dialogue de confirmation impossible : %s",
                                   strerror(erreur));
                } else if (confirmation == 0) {
                    (void)snprintf(statut, sizeof statut,
                                   "Suppression annulee.");
                } else if (supprimer_image(chemin_dossier, image) == 0) {
                    retirer_image_de_liste(images, image);
                    --nombre;
                    if (selection >= nombre && selection > 0) {
                        --selection;
                    }
                    (void)snprintf(statut, sizeof statut,
                                   "Image supprimee.");
                } else {
                    const int erreur = errno;
                    (void)snprintf(statut, sizeof statut,
                                   "Suppression refusee/echec : %s",
                                   strerror(erreur));
                }
            }
        } else if (touche == 'r' || touche == 'R') {
            struct Image *nouvelle_liste = NULL;

            if (scanner_dossier(chemin_dossier, &nouvelle_liste) == -1) {
                const int erreur = errno;
                (void)snprintf(statut, sizeof statut, "Scan impossible : %s",
                               strerror(erreur));
            } else {
                /*
                 * Le nouveau scan a réussi : seulement alors on libère
                 * l'ancienne liste et on adopte la nouvelle.
                 */
                liberer_images(*images);
                *images = nouvelle_liste;
                nombre = 0;
                for (struct Image *image = *images; image != NULL;
                     image = image->suivant) {
                    ++nombre;
                }
                selection = 0;
                premier_visible = 0;
                (void)snprintf(statut, sizeof statut,
                               "Scan termine : %zu image(s).", nombre);
            }
        }
    }

    /*
     * endwin() restitue au terminal son état avant initscr() : sans cet appel,
     * le shell pourrait rester sans écho ou avec un curseur masqué.
     */
    (void)endwin();
     if (erreur_terminal) {
         fprintf(stderr, "Erreur de lecture des touches du terminal.\n");
         return -1;
     }
     return 0;
}
