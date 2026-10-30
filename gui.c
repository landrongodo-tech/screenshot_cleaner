#define _POSIX_C_SOURCE 200809L

#include "gui.h"

#include "file.h"

#include <gtk/gtk.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib/gstdio.h>

/*
 * Cleaner sépare soigneusement les données du backend et les widgets GTK :
 * `images` appartient au moteur C, alors que les autres pointeurs ci-dessous
 * désignent des widgets dont la fenêtre GTK possède la durée de vie.
 */
struct AppState {
    GtkApplication *application;
    GtkWidget *fenetre;
    GtkWidget *apercu;
    GdkPixbuf *pixbuf_apercu;
    GtkWidget *details;
    GtkWidget *progression;
    GtkWidget *statut;
    GtkWidget *bouton_garder;
    GtkWidget *bouton_supprimer;
    char *chemin_configuration;
    char *dossier;
    char *date_compteur;
    char heure_rituel[6];
    guint quota;
    guint traite;
    guint delai_report_minutes;
    gint64 reporter_jusqua;
    gboolean suppression_corbeille;
    guint comportement_pause;
    struct Image *images;
    struct Image *courante;
    gboolean premier_lancement;
    gboolean choisir_dossier_initial;
    gboolean synchronisation_systemd_en_cours;
};

enum {
    COMPORTEMENT_PAUSE_FERMER,
    COMPORTEMENT_PAUSE_REPORTER,
    COMPORTEMENT_PAUSE_CHOIX
};

/*
 * Une date locale au format AAAA-MM-JJ sert de clé au compteur journalier.
 * La conversion passe par GDateTime/GLib afin d'éviter les tampons globaux
 * non réentrants de fonctions comme localtime().
 */
static char *date_locale_du_jour(void)
{
    GDateTime *maintenant = g_date_time_new_now_local();
    if (maintenant == NULL) {
        return NULL;
    }
    char *date = g_date_time_format(maintenant, "%Y-%m-%d");
    g_date_time_unref(maintenant);
    return date;
}

/*
 * Vérifie strictement HH:MM : cette validation commune protège aussi bien les
 * valeurs lues dans le fichier que les valeurs écrites par le dialogue GTK.
 */
static gboolean heure_valide(const char *heure)
{
    return heure != NULL
        && strlen(heure) == 5
        && heure[0] >= '0' && heure[0] <= '2'
        && heure[1] >= '0' && heure[1] <= '9'
        && heure[3] >= '0' && heure[3] <= '5'
        && heure[4] >= '0' && heure[4] <= '9'
        && heure[2] == ':'
        && (heure[0] != '2' || heure[1] <= '3');
}

/*
 * Utilise d'abord le répertoire Images déclaré par l'environnement de bureau
 * (qui peut être localisé), puis son sous-dossier conventionnel de captures.
 * Si XDG ne définit pas ce répertoire, on retombe sur ~/Images.
 */
static char *dossier_captures_par_defaut(void)
{
    const char *repertoire_images =
        g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    const char *home = getenv("HOME");

    /*
     * La valeur HOME appartient au compte qui lance le processus. Elle sert
     * de repli explicite si le bureau ne fournit pas de dossier Images XDG.
     * g_get_home_dir() couvre le cas rare où HOME n'est pas défini.
     */
    if (home == NULL || home[0] == '\0') {
        home = g_get_home_dir();
    }

    if (repertoire_images == NULL || repertoire_images[0] == '\0') {
        return g_build_filename(home, "Images",
                                "Captures d'écran", NULL);
    }

    return g_build_filename(repertoire_images, "Captures d'écran", NULL);
}

/*
 * GKeyFile écrit les réglages sous forme INI, lisible par l'humain et
 * extensible. g_file_set_contents remplace le fichier via un fichier temporaire
 * quand le système le permet, ce qui évite normalement un INI à moitié écrit.
 * Le compteur et le dossier y sont persistés avec les préférences.
 */
static gboolean enregistrer_configuration(
    struct AppState *etat, const char *heure, guint quota, guint traite,
    const char *date, const char *dossier, GError **erreur)
{
    GKeyFile *fichier = g_key_file_new();
    g_key_file_set_string(fichier, "rituel", "heure", heure);
    g_key_file_set_uint64(fichier, "rituel", "quota", quota);
    g_key_file_set_boolean(fichier, "suppression", "corbeille",
                           etat->suppression_corbeille);
    g_key_file_set_uint64(fichier, "pause", "comportement",
                          etat->comportement_pause);
    g_key_file_set_uint64(fichier, "pause", "delai_minutes",
                          etat->delai_report_minutes);
    g_key_file_set_int64(fichier, "pause", "reporter_jusqua",
                         etat->reporter_jusqua);
    g_key_file_set_uint64(fichier, "progression", "traite", traite);
    g_key_file_set_string(fichier, "progression", "date", date);
    g_key_file_set_string(fichier, "dossier", "chemin", dossier);

    gsize longueur = 0;
    char *contenu = g_key_file_to_data(fichier, &longueur, erreur);
    g_key_file_unref(fichier);
    if (contenu == NULL) {
        return FALSE;
    }

    char *repertoire = g_path_get_dirname(etat->chemin_configuration);
    if (g_mkdir_with_parents(repertoire, 0700) == -1) {
        g_set_error(erreur, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Impossible de créer %s : %s",
                    repertoire, g_strerror(errno));
        g_free(repertoire);
        g_free(contenu);
        return FALSE;
    }

    gboolean resultat = g_file_set_contents(
        etat->chemin_configuration, contenu, (gssize)longueur, erreur);
    if (resultat && g_chmod(etat->chemin_configuration, 0600) == -1) {
        const int erreur_systeme = errno;
        g_set_error(erreur, G_FILE_ERROR,
                    g_file_error_from_errno(erreur_systeme),
                    "Impossible de protéger %s : %s",
                    etat->chemin_configuration, g_strerror(erreur_systeme));
        resultat = FALSE;
    }

    g_free(repertoire);
    g_free(contenu);
    return resultat;
}

/*
 * Charge l'INI avec GKeyFile, qui distingue notamment une clé absente d'une
 * valeur mal formée. On ne remplace jamais silencieusement une configuration
 * existante corrompue par des valeurs par défaut.
 *
 * Résultat : 1 = configuration chargée, 0 = premier lancement, -1 = erreur.
 */
static int charger_configuration(struct AppState *etat, const char *dossier_force,
                                 GError **erreur)
{
    char *ancienne_configuration = g_build_filename(
        g_get_user_config_dir(), "cleaner", "settings.ini", NULL);
    const gboolean configuration_existe =
        g_file_test(etat->chemin_configuration, G_FILE_TEST_EXISTS);
    const gboolean ancienne_configuration_existe =
        ancienne_configuration != NULL
        && g_file_test(ancienne_configuration, G_FILE_TEST_IS_REGULAR);
    if (!configuration_existe && !ancienne_configuration_existe) {
        g_free(ancienne_configuration);
        etat->heure_rituel[0] = '\0';
        etat->quota = 10;
        etat->delai_report_minutes = 30;
        etat->comportement_pause = 2;
        etat->suppression_corbeille = FALSE;
        etat->reporter_jusqua = 0;
        etat->traite = 0;
        etat->date_compteur = date_locale_du_jour();
        char *dossier_initial = dossier_force != NULL
            ? g_strdup(dossier_force) : dossier_captures_par_defaut();
        if (dossier_initial != NULL && dossier_force == NULL) {
            etat->choisir_dossier_initial =
                !g_file_test(dossier_initial, G_FILE_TEST_IS_DIR);
        }
        etat->dossier = dossier_initial != NULL
            ? g_canonicalize_filename(dossier_initial, NULL) : NULL;
        g_free(dossier_initial);
        if (etat->dossier == NULL || etat->date_compteur == NULL) {
            g_set_error_literal(erreur, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                                "Mémoire insuffisante pendant l'initialisation.");
            return -1;
        }
        return 0;
    }

    GKeyFile *fichier = g_key_file_new();
    gboolean migration = FALSE;
    const char *source_configuration = configuration_existe
        ? etat->chemin_configuration : ancienne_configuration;
    migration = !configuration_existe;
    if (!g_key_file_load_from_file(fichier, source_configuration,
                                   G_KEY_FILE_NONE, erreur)) {
        g_key_file_unref(fichier);
        g_free(ancienne_configuration);
        return -1;
    } else {
        g_free(ancienne_configuration);
    }

    etat->delai_report_minutes = 30;
    etat->comportement_pause = 2;
    etat->suppression_corbeille = FALSE;
    etat->reporter_jusqua = 0;

    if (g_key_file_has_key(fichier, "suppression", "corbeille", NULL)) {
        etat->suppression_corbeille = g_key_file_get_boolean(
            fichier, "suppression", "corbeille", erreur);
        if (*erreur != NULL) {
            g_key_file_unref(fichier);
            return -1;
        }
    }
    if (g_key_file_has_key(fichier, "pause", "comportement", NULL)) {
        const guint64 comportement = g_key_file_get_uint64(
            fichier, "pause", "comportement", erreur);
        if (*erreur != NULL || comportement > 2) {
            if (*erreur == NULL) {
                g_set_error_literal(erreur, G_KEY_FILE_ERROR,
                                    G_KEY_FILE_ERROR_INVALID_VALUE,
                                    "Comportement de pause invalide.");
            }
            g_key_file_unref(fichier);
            return -1;
        }
        etat->comportement_pause = (guint)comportement;
    }
    if (g_key_file_has_key(fichier, "pause", "delai_minutes", NULL)) {
        const guint64 delai = g_key_file_get_uint64(
            fichier, "pause", "delai_minutes", erreur);
        if (*erreur != NULL || delai < 5 || delai > 1440) {
            if (*erreur == NULL) {
                g_set_error_literal(erreur, G_KEY_FILE_ERROR,
                                    G_KEY_FILE_ERROR_INVALID_VALUE,
                                    "Délai de report invalide.");
            }
            g_key_file_unref(fichier);
            return -1;
        }
        etat->delai_report_minutes = (guint)delai;
    }
    if (g_key_file_has_key(fichier, "pause", "reporter_jusqua", NULL)) {
        etat->reporter_jusqua = g_key_file_get_int64(
            fichier, "pause", "reporter_jusqua", erreur);
        if (*erreur != NULL || etat->reporter_jusqua < 0) {
            if (*erreur == NULL) {
                g_set_error_literal(erreur, G_KEY_FILE_ERROR,
                                    G_KEY_FILE_ERROR_INVALID_VALUE,
                                    "Échéance de report invalide.");
            }
            g_key_file_unref(fichier);
            return -1;
        }
    }

    char *heure = g_key_file_get_string(fichier, "rituel", "heure", erreur);
    if (*erreur != NULL) {
        g_key_file_unref(fichier);
        return -1;
    }
    const guint64 quota = g_key_file_get_uint64(fichier, "rituel", "quota",
                                                 erreur);
    if (*erreur != NULL) {
        g_key_file_unref(fichier);
        g_free(heure);
        return -1;
    }
    const guint64 traite = g_key_file_get_uint64(
        fichier, "progression", "traite", erreur);
    if (*erreur != NULL) {
        g_key_file_unref(fichier);
        g_free(heure);
        return -1;
    }
    char *date = g_key_file_get_string(
        fichier, "progression", "date", erreur);
    if (*erreur != NULL) {
        g_key_file_unref(fichier);
        g_free(heure);
        return -1;
    }
    char *dossier = g_key_file_get_string(fichier, "dossier", "chemin", erreur);
    g_key_file_unref(fichier);
    if (*erreur != NULL) {
        g_free(heure);
        g_free(date);
        return -1;
    }

    if (!heure_valide(heure) || quota == 0 || quota > 10000
        || traite > G_MAXUINT || dossier == NULL
        || dossier[0] == '\0') {
        g_set_error(erreur, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_INVALID_VALUE,
                    "Réglages invalides dans %s (heure, quota ou dossier).",
                    etat->chemin_configuration);
        g_free(heure);
        g_free(date);
        g_free(dossier);
        return -1;
    }

    char *dossier_initial = dossier_force != NULL
        ? g_strdup(dossier_force) : dossier_captures_par_defaut();
    if (dossier_initial == NULL) {
        g_free(heure);
        g_free(date);
        g_free(dossier);
        g_set_error_literal(erreur, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                            "Impossible de déterminer le dossier de captures.");
        return -1;
    }
    if (dossier_force == NULL) {
        /*
         * Un dossier choisi précédemment reste une préférence valide. On
         * écarte toutefois l'ancien défaut "." (devenu le répertoire courant)
         * afin qu'il ne remplace pas le nouveau défaut de captures.
         */
        char *dossier_enregistre = g_canonicalize_filename(dossier, NULL);
        char *repertoire_courant = g_get_current_dir();
        if (dossier_enregistre != NULL
            && g_file_test(dossier_enregistre, G_FILE_TEST_IS_DIR)
            && g_strcmp0(dossier_enregistre, repertoire_courant) != 0
            && g_strcmp0(dossier_enregistre, dossier_initial) != 0) {
            g_free(dossier_initial);
            dossier_initial = dossier_enregistre;
            dossier_enregistre = NULL;
        }
        g_free(dossier_enregistre);
        g_free(repertoire_courant);
        etat->choisir_dossier_initial =
            !g_file_test(dossier_initial, G_FILE_TEST_IS_DIR);
    }

    g_strlcpy(etat->heure_rituel, heure, sizeof etat->heure_rituel);
    etat->quota = CLAMP((guint)quota, 5, 50);
    etat->traite = (guint)traite;
    etat->date_compteur = date;
    etat->dossier = g_canonicalize_filename(
        dossier_initial, NULL);
    const gboolean dossier_modifie =
        dossier_force != NULL || g_strcmp0(etat->dossier, dossier) != 0;
    g_free(dossier_initial);
    g_free(dossier);
    g_free(heure);

    if (etat->dossier == NULL || etat->date_compteur == NULL) {
        g_set_error_literal(erreur, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                            "Mémoire insuffisante pendant le chargement.");
        return -1;
    }

    gboolean configuration_modifiee = dossier_modifie || migration
        || quota != etat->quota;
    char *aujourd_hui = date_locale_du_jour();
    if (aujourd_hui == NULL) {
        g_set_error_literal(erreur, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                            "Impossible de déterminer la date locale.");
        return -1;
    }
    if (g_strcmp0(etat->date_compteur, aujourd_hui) != 0) {
        g_free(etat->date_compteur);
        etat->date_compteur = aujourd_hui;
        etat->traite = 0;
        configuration_modifiee = TRUE;
    } else {
        g_free(aujourd_hui);
    }

    if (configuration_modifiee) {
        if (!enregistrer_configuration(
                etat, etat->heure_rituel, etat->quota, etat->traite,
                etat->date_compteur, etat->dossier, erreur)) {
            return -1;
        }
    }
    return 1;
}

/* Tous les dialogues d'erreur montrent un détail explicite, sans cacher errno
 * ou l'erreur GLib derrière un faux message de réussite. */
static void afficher_erreur(GtkWindow *parent, const char *titre,
                            const char *detail)
{
    GtkWidget *dialogue = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", titre);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialogue), "%s", detail);
    (void)gtk_dialog_run(GTK_DIALOG(dialogue));
    gtk_widget_destroy(dialogue);
}

static gboolean executer_commande_systemd(const char *const arguments[],
                                          GError **erreur)
{
    GSubprocess *processus = g_subprocess_newv(
        arguments, G_SUBPROCESS_FLAGS_STDOUT_PIPE
                       | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        erreur);
    if (processus == NULL) {
        return FALSE;
    }

    char *sortie = NULL;
    char *diagnostic = NULL;
    const gboolean communique = g_subprocess_communicate_utf8(
        processus, NULL, NULL, &sortie, &diagnostic, erreur);
    if (!communique) {
        g_free(sortie);
        g_free(diagnostic);
        g_object_unref(processus);
        return FALSE;
    }

    if (!g_subprocess_get_successful(processus)) {
        g_set_error(erreur, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "La commande systemd a échoué : %s",
                    diagnostic != NULL && diagnostic[0] != '\0'
                        ? diagnostic
                        : "aucun détail fourni");
        g_free(sortie);
        g_free(diagnostic);
        g_object_unref(processus);
        return FALSE;
    }

    g_free(sortie);
    g_free(diagnostic);
    g_object_unref(processus);
    return TRUE;
}

static char *chemin_exec_systemd(const char *chemin)
{
    GString *resultat = g_string_new("\"");
    for (const char *caractere = chemin; *caractere != '\0'; ++caractere) {
        switch (*caractere) {
        case '\\':
            g_string_append(resultat, "\\\\");
            break;
        case '"':
            g_string_append(resultat, "\\\"");
            break;
        case '$':
            g_string_append(resultat, "$$");
            break;
        case '%':
            g_string_append(resultat, "%%");
            break;
        case '\n':
            g_string_append(resultat, "\\n");
            break;
        case '\r':
            g_string_append(resultat, "\\r");
            break;
        case '\t':
            g_string_append(resultat, "\\t");
            break;
        default:
            g_string_append_c(resultat, *caractere);
            break;
        }
    }
    return g_string_append_c(resultat, '"')->str;
}

/*
 * Le service hérite de l'environnement graphique importé dans le gestionnaire
 * systemd utilisateur. Le timer garde sa vérification par minute : l'heure
 * effective et les reports restent pilotés par config.conf.
 */
static gboolean synchroniser_timer_utilisateur(GError **erreur)
{
    const char *repertoire_configuration = g_get_user_config_dir();
    char *repertoire_unites = g_build_filename(
        repertoire_configuration, "systemd", "user", NULL);
    if (repertoire_unites == NULL
        || g_mkdir_with_parents(repertoire_unites, 0700) == -1) {
        const int erreur_systeme = errno;
        g_set_error(erreur, G_FILE_ERROR,
                    g_file_error_from_errno(erreur_systeme),
                    "Impossible de créer le répertoire des unités systemd : %s",
                    g_strerror(erreur_systeme));
        g_free(repertoire_unites);
        return FALSE;
    }

    char *chemin_service = g_build_filename(
        repertoire_unites, "cleaner.service", NULL);
    char *chemin_timer = g_build_filename(
        repertoire_unites, "cleaner.timer", NULL);
    char *chemin_binaire = g_file_read_link("/proc/self/exe", erreur);
    if (chemin_service == NULL || chemin_timer == NULL
        || chemin_binaire == NULL) {
        if (erreur != NULL && *erreur == NULL) {
            g_set_error_literal(erreur, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                                "Mémoire insuffisante pour préparer systemd.");
        }
        g_free(chemin_binaire);
        g_free(chemin_service);
        g_free(chemin_timer);
        g_free(repertoire_unites);
        return FALSE;
    }

    char *chemin_binaire_echappe = chemin_exec_systemd(chemin_binaire);
    char *contenu_service = g_strdup_printf(
        "[Unit]\n"
        "Description=Cleaner - rituel quotidien de tri des images\n"
        "After=graphical-session.target\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "ExecStart=%s --scheduled\n",
        chemin_binaire_echappe);
    const char *contenu_timer =
        "[Unit]\n"
        "Description=Vérification périodique de l'heure du rituel Cleaner\n"
        "\n"
        "[Timer]\n"
        "OnCalendar=*-*-* *:*:00\n"
        "AccuracySec=1s\n"
        "Persistent=true\n"
        "Unit=cleaner.service\n"
        "\n"
        "[Install]\n"
        "WantedBy=timers.target\n";

    gboolean resultat = contenu_service != NULL
        && g_file_set_contents(chemin_service, contenu_service, -1, erreur);
    if (contenu_service == NULL && erreur != NULL && *erreur == NULL) {
        g_set_error_literal(erreur, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                            "Mémoire insuffisante pour préparer systemd.");
        resultat = FALSE;
    }
    if (resultat && g_chmod(chemin_service, 0644) == -1) {
        const int erreur_systeme = errno;
        g_set_error(erreur, G_FILE_ERROR,
                    g_file_error_from_errno(erreur_systeme),
                    "Impossible de protéger %s : %s",
                    chemin_service, g_strerror(erreur_systeme));
        resultat = FALSE;
    }
    if (resultat) {
        resultat = g_file_set_contents(
            chemin_timer, contenu_timer, -1, erreur);
    }
    if (resultat && g_chmod(chemin_timer, 0644) == -1) {
        const int erreur_systeme = errno;
        g_set_error(erreur, G_FILE_ERROR,
                    g_file_error_from_errno(erreur_systeme),
                    "Impossible de protéger %s : %s",
                    chemin_timer, g_strerror(erreur_systeme));
        resultat = FALSE;
    }

    g_free(chemin_service);
    g_free(chemin_timer);
    g_free(chemin_binaire);
    g_free(chemin_binaire_echappe);
    g_free(contenu_service);
    g_free(repertoire_unites);
    if (!resultat) {
        return FALSE;
    }

    static const char *const importer_environnement[] = {
        "systemctl", "--user", "import-environment",
        "HOME", "XDG_CONFIG_HOME", "DISPLAY", "WAYLAND_DISPLAY",
        "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR", NULL
    };
    static const char *const recharger[] = {
        "systemctl", "--user", "daemon-reload", NULL
    };
    static const char *const activer[] = {
        "systemctl", "--user", "enable", "--now", "cleaner.timer", NULL
    };
    static const char *const redemarrer[] = {
        "systemctl", "--user", "restart", "cleaner.timer", NULL
    };

    return executer_commande_systemd(importer_environnement, erreur)
        && executer_commande_systemd(recharger, erreur)
        && executer_commande_systemd(activer, erreur)
        && executer_commande_systemd(redemarrer, erreur);
}

static void synchroniser_timer_en_arriere_plan(
    GTask *tache, gpointer source, gpointer donnees,
    GCancellable *annulation)
{
    (void)source;
    (void)donnees;
    (void)annulation;
    GError *erreur = NULL;
    if (synchroniser_timer_utilisateur(&erreur)) {
        g_task_return_boolean(tache, TRUE);
    } else {
        g_task_return_error(tache, erreur);
    }
}

static void synchronisation_timer_terminee(GObject *source,
                                           GAsyncResult *resultat,
                                           gpointer donnees)
{
    struct AppState *etat = donnees;
    GError *erreur = NULL;
    (void)source;

    etat->synchronisation_systemd_en_cours = FALSE;
    if (!g_task_propagate_boolean(G_TASK(resultat), &erreur)) {
        afficher_erreur(
            etat->fenetre != NULL ? GTK_WINDOW(etat->fenetre) : NULL,
            "Réglages enregistrés, automatisation indisponible",
            erreur->message);
        g_clear_error(&erreur);
    }
    g_application_release(G_APPLICATION(etat->application));
}

static void lancer_synchronisation_timer(struct AppState *etat)
{
    if (etat->synchronisation_systemd_en_cours) {
        return;
    }

    etat->synchronisation_systemd_en_cours = TRUE;
    g_application_hold(G_APPLICATION(etat->application));
    GTask *tache = g_task_new(NULL, NULL, synchronisation_timer_terminee, etat);
    g_task_run_in_thread(tache, synchroniser_timer_en_arriere_plan);
    g_object_unref(tache);
}

struct ChoixDossierReglages {
    GtkWindow *parent;
    GtkWidget *affichage;
    char **chemin;
};

static void parcourir_dossier_reglages(GtkButton *bouton, gpointer donnees)
{
    struct ChoixDossierReglages *choix = donnees;
    (void)bouton;
    GtkWidget *dialogue = gtk_file_chooser_dialog_new(
        "Sélectionner le dossier d'images", choix->parent,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Annuler", GTK_RESPONSE_CANCEL,
        "_Sélectionner", GTK_RESPONSE_ACCEPT,
        NULL);

    if (*choix->chemin != NULL
        && g_file_test(*choix->chemin, G_FILE_TEST_IS_DIR)) {
        (void)gtk_file_chooser_set_current_folder(
            GTK_FILE_CHOOSER(dialogue), *choix->chemin);
    } else {
        (void)gtk_file_chooser_set_current_folder(
            GTK_FILE_CHOOSER(dialogue), g_get_home_dir());
    }

    if (gtk_dialog_run(GTK_DIALOG(dialogue)) == GTK_RESPONSE_ACCEPT) {
        char *chemin = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(dialogue));
        if (chemin != NULL) {
            g_free(*choix->chemin);
            *choix->chemin = chemin;
            char *nom_affiche = g_filename_display_name(chemin);
            gtk_label_set_text(GTK_LABEL(choix->affichage), nom_affiche);
            gtk_widget_set_tooltip_text(choix->affichage, chemin);
            g_free(nom_affiche);
        }
    }

    gtk_widget_destroy(dialogue);
}

/* Le premier lancement et le bouton Réglages partagent le même formulaire. */
static gboolean dialoguer_reglages(struct AppState *etat, gboolean premier)
{
    char *dossier_selectionne = g_strdup(etat->dossier);
    GtkWidget *dialogue = gtk_dialog_new_with_buttons(
        premier ? "Configurer Cleaner" : "Réglages du rituel",
        etat->fenetre != NULL ? GTK_WINDOW(etat->fenetre) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Annuler", GTK_RESPONSE_CANCEL,
        premier ? "Commencer" : "Enregistrer", GTK_RESPONSE_ACCEPT,
        NULL);
    GtkWidget *contenu = gtk_dialog_get_content_area(GTK_DIALOG(dialogue));
    GtkWidget *grille = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grille), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grille), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grille), 16);
    gtk_container_add(GTK_CONTAINER(contenu), grille);

    GtkWidget *heure_label = gtk_label_new("Heure quotidienne du rituel :");
    GtkWidget *heure = gtk_spin_button_new_with_range(0, 23, 1);
    GtkWidget *minute = gtk_spin_button_new_with_range(0, 59, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(heure), 0);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(minute), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(heure),
                              etat->heure_rituel[0] != '\0'
                                  ? (etat->heure_rituel[0] - '0') * 10
                                        + etat->heure_rituel[1] - '0'
                                  : 20);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(minute),
                              etat->heure_rituel[0] != '\0'
                                  ? (etat->heure_rituel[3] - '0') * 10
                                        + etat->heure_rituel[4] - '0'
                                  : 0);
    GtkWidget *separateur = gtk_label_new(":");
    GtkWidget *heure_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(heure_box), heure, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heure_box), separateur, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heure_box), minute, FALSE, FALSE, 0);

    GtkWidget *quota_label = gtk_label_new("Quota d'images par session :");
    GtkWidget *quota = gtk_spin_button_new_with_range(5, 50, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(quota), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(quota),
                              CLAMP(etat->quota != 0 ? etat->quota : 10,
                                    5, 50));
    GtkWidget *dossier_label = gtk_label_new("Dossier d'images :");
    GtkWidget *dossier_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *dossier_affichage = gtk_label_new(NULL);
    GtkWidget *dossier_bouton = gtk_button_new_with_label("Parcourir…");
    const char *chemin_initial = dossier_selectionne != NULL
        ? dossier_selectionne : "Aucun dossier sélectionné";
    char *nom_initial = dossier_selectionne != NULL
        ? g_filename_display_name(dossier_selectionne)
        : g_strdup(chemin_initial);
    gtk_label_set_text(GTK_LABEL(dossier_affichage), nom_initial);
    gtk_widget_set_tooltip_text(dossier_affichage, dossier_selectionne);
    gtk_widget_set_halign(dossier_affichage, GTK_ALIGN_START);
    gtk_widget_set_hexpand(dossier_affichage, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(dossier_affichage), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(dossier_box), dossier_affichage, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(dossier_box), dossier_bouton, FALSE, FALSE, 0);
    g_free(nom_initial);
    struct ChoixDossierReglages choix_dossier = {
        etat->fenetre != NULL ? GTK_WINDOW(etat->fenetre) : NULL,
        dossier_affichage,
        &dossier_selectionne
    };
    g_signal_connect(dossier_bouton, "clicked",
                     G_CALLBACK(parcourir_dossier_reglages), &choix_dossier);

    GtkWidget *suppression_label = gtk_label_new("Suppression :");
    GtkWidget *suppression = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(suppression),
                                   "Définitive (unlinkat)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(suppression),
                                   "Déplacer vers la Corbeille");
    gtk_combo_box_set_active(GTK_COMBO_BOX(suppression),
                             etat->suppression_corbeille ? 1 : 0);

    GtkWidget *pause_label = gtk_label_new("Bouton Reporter / Pause :");
    GtkWidget *pause = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pause),
                                   "Fermer la session");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pause),
                                   "Reporter automatiquement");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(pause),
                                   "Choisir à chaque fois");
    gtk_combo_box_set_active(GTK_COMBO_BOX(pause),
                             (gint)etat->comportement_pause);

    GtkWidget *delai_label = gtk_label_new("Délai de report (minutes) :");
    GtkWidget *delai = gtk_spin_button_new_with_range(5, 1440, 5);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(delai), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(delai),
                              etat->delai_report_minutes != 0
                                  ? etat->delai_report_minutes : 30);

    gtk_widget_set_halign(heure_label, GTK_ALIGN_START);
    gtk_widget_set_halign(quota_label, GTK_ALIGN_START);
    gtk_widget_set_halign(dossier_label, GTK_ALIGN_START);
    gtk_widget_set_halign(suppression_label, GTK_ALIGN_START);
    gtk_widget_set_halign(pause_label, GTK_ALIGN_START);
    gtk_widget_set_halign(delai_label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grille), heure_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), heure_box, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), quota_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), quota, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), dossier_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), dossier_box, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), suppression_label, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), suppression, 1, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), pause_label, 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), pause, 1, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), delai_label, 0, 5, 1, 1);
    gtk_grid_attach(GTK_GRID(grille), delai, 1, 5, 1, 1);

    if (premier) {
        GtkWidget *explication = gtk_label_new(
            "Vous pourrez modifier ces valeurs plus tard dans Réglages.");
        gtk_widget_set_halign(explication, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grille), explication, 0, 6, 2, 1);
    }

    gtk_widget_show_all(dialogue);
    const gint reponse = gtk_dialog_run(GTK_DIALOG(dialogue));
    if (reponse != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dialogue);
        g_free(dossier_selectionne);
        return FALSE;
    }

    char nouvelle_heure[6];
    (void)snprintf(nouvelle_heure, sizeof nouvelle_heure, "%02d:%02d",
                   gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(heure)),
                   gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(minute)));
    const guint nouveau_quota =
        (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(quota));
    char *nouveau_dossier = dossier_selectionne;
    const gboolean nouvelle_corbeille =
        gtk_combo_box_get_active(GTK_COMBO_BOX(suppression)) == 1;
    const guint nouveau_comportement =
        (guint)gtk_combo_box_get_active(GTK_COMBO_BOX(pause));
    const guint nouveau_delai =
        (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(delai));
    gtk_widget_destroy(dialogue);

    if (nouveau_dossier == NULL
        || !g_file_test(nouveau_dossier, G_FILE_TEST_IS_DIR)) {
        afficher_erreur(etat->fenetre != NULL ? GTK_WINDOW(etat->fenetre) : NULL,
                        "Dossier invalide",
                        "Choisissez un dossier existant pour les images.");
        g_free(nouveau_dossier);
        return FALSE;
    }

    char *aujourd_hui = date_locale_du_jour();
    if (aujourd_hui == NULL) {
        afficher_erreur(etat->fenetre != NULL ? GTK_WINDOW(etat->fenetre) : NULL,
                        "Réglages non enregistrés",
                        "Impossible de déterminer la date locale.");
        g_free(nouveau_dossier);
        return FALSE;
    }
    const guint nouveau_traite =
        !premier && g_strcmp0(etat->date_compteur, aujourd_hui) == 0
            ? etat->traite : 0;
    char *dossier_canonique = g_canonicalize_filename(nouveau_dossier, NULL);
    g_free(nouveau_dossier);
    if (dossier_canonique == NULL) {
        g_free(aujourd_hui);
        afficher_erreur(etat->fenetre != NULL ? GTK_WINDOW(etat->fenetre) : NULL,
                        "Réglages non enregistrés",
                        "Impossible de mémoriser le dossier sélectionné.");
        return FALSE;
    }

    char ancienne_heure[sizeof etat->heure_rituel];
    g_strlcpy(ancienne_heure, etat->heure_rituel, sizeof ancienne_heure);
    const guint ancien_quota = etat->quota;
    const guint ancienne_pause = etat->comportement_pause;
    const guint ancien_delai = etat->delai_report_minutes;
    const gboolean ancienne_corbeille = etat->suppression_corbeille;
    char *ancien_dossier = etat->dossier;
    const guint ancien_traite = etat->traite;
    etat->dossier = dossier_canonique;
    g_strlcpy(etat->heure_rituel, nouvelle_heure,
              sizeof etat->heure_rituel);
    etat->quota = nouveau_quota;
    etat->traite = nouveau_traite;
    etat->suppression_corbeille = nouvelle_corbeille;
    etat->comportement_pause = nouveau_comportement;
    etat->delai_report_minutes = nouveau_delai;

    GError *erreur = NULL;
    if (!enregistrer_configuration(
            etat, nouvelle_heure, nouveau_quota, nouveau_traite,
            aujourd_hui, etat->dossier, &erreur)) {
        g_strlcpy(etat->heure_rituel, ancienne_heure,
                  sizeof etat->heure_rituel);
        etat->quota = ancien_quota;
        etat->traite = ancien_traite;
        etat->suppression_corbeille = ancienne_corbeille;
        etat->comportement_pause = ancienne_pause;
        etat->delai_report_minutes = ancien_delai;
        etat->dossier = ancien_dossier;
        g_free(dossier_canonique);
        afficher_erreur(etat->fenetre != NULL ? GTK_WINDOW(etat->fenetre) : NULL,
                        "Impossible d'enregistrer les réglages",
                        erreur->message);
        g_clear_error(&erreur);
        g_free(aujourd_hui);
        return FALSE;
    }

    g_free(ancien_dossier);
    g_free(etat->date_compteur);
    etat->date_compteur = aujourd_hui;
    etat->choisir_dossier_initial = FALSE;

    lancer_synchronisation_timer(etat);
    return TRUE;
}

/* Donne le même retour visuel dans le bandeau après chaque action. */
static void afficher_statut(struct AppState *etat, const char *message)
{
    gtk_label_set_text(GTK_LABEL(etat->statut), message);
}

/*
 * Le dessin se fait dans l'allocation réelle de la zone flexible. La taille
 * intrinsèque du fichier ne participe ainsi jamais à la taille minimale de la
 * fenêtre, et les proportions de l'image sont conservées.
 */
static gboolean dessiner_apercu(GtkWidget *zone, cairo_t *contexte,
                                gpointer donnees)
{
    struct AppState *etat = donnees;
    if (etat->pixbuf_apercu == NULL) {
        return FALSE;
    }

    const gint largeur_zone = gtk_widget_get_allocated_width(zone);
    const gint hauteur_zone = gtk_widget_get_allocated_height(zone);
    const gint largeur_source = gdk_pixbuf_get_width(etat->pixbuf_apercu);
    const gint hauteur_source = gdk_pixbuf_get_height(etat->pixbuf_apercu);
    if (largeur_zone <= 0 || hauteur_zone <= 0
        || largeur_source <= 0 || hauteur_source <= 0) {
        return FALSE;
    }

    const double facteur = MIN(1.0, MIN((double)largeur_zone / largeur_source,
                                        (double)hauteur_zone / hauteur_source));
    const gint largeur = MAX(1, (gint)(largeur_source * facteur));
    const gint hauteur = MAX(1, (gint)(hauteur_source * facteur));
    GdkPixbuf *redimensionne = gdk_pixbuf_scale_simple(
        etat->pixbuf_apercu, largeur, hauteur, GDK_INTERP_BILINEAR);
    if (redimensionne == NULL) {
        return FALSE;
    }

    const double x = (largeur_zone - largeur) / 2.0;
    const double y = (hauteur_zone - hauteur) / 2.0;
    gdk_cairo_set_source_pixbuf(contexte, redimensionne, x, y);
    cairo_paint(contexte);
    g_object_unref(redimensionne);
    return FALSE;
}

static void effacer_apercu(struct AppState *etat)
{
    g_clear_object(&etat->pixbuf_apercu);
    gtk_widget_queue_draw(etat->apercu);
}

/*
 * Charge uniquement l'image courante. GdkPixbuf décode le format reconnu
 * (PNG/JPEG notamment) en limitant la mémoire utilisée. Le dessin adapte
 * ensuite cette miniature à la place disponible dans la fenêtre.
 */
static void afficher_image_courante(struct AppState *etat)
{
    char *progression = g_strdup_printf(
        "%u / %u image(s) traitée(s) dans cette session — rituel à %s",
        etat->traite, etat->quota, etat->heure_rituel);
    gtk_label_set_text(GTK_LABEL(etat->progression), progression);
    g_free(progression);

    if (etat->traite >= etat->quota) {
        effacer_apercu(etat);
        gtk_widget_hide(etat->apercu);
        gtk_widget_hide(etat->bouton_garder);
        gtk_widget_hide(etat->bouton_supprimer);
        gtk_label_set_text(GTK_LABEL(etat->details),
                           "Bravo ! Votre quota de session est atteint.");
        afficher_statut(etat, "Belle habitude : votre rituel du jour est terminé.");
        return;
    }

    gtk_widget_show(etat->bouton_garder);
    gtk_widget_show(etat->bouton_supprimer);
    if (etat->courante == NULL) {
        gtk_widget_hide(etat->bouton_garder);
        gtk_widget_hide(etat->bouton_supprimer);
        effacer_apercu(etat);
        gtk_widget_hide(etat->apercu);
        gtk_label_set_text(GTK_LABEL(etat->details),
                           "Plus d'image dans ce dossier.");
        afficher_statut(etat, "Actualisez ou choisissez un autre dossier pour continuer.");
        return;
    }

    gtk_widget_show(etat->apercu);
    GError *erreur = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
        etat->courante->chemin, 1100, 620, TRUE, &erreur);
    if (pixbuf == NULL) {
        effacer_apercu(etat);
        char *nom = g_filename_display_name(etat->courante->nom);
        char *texte = g_strdup_printf(
            "Aperçu impossible pour « %s ».\n%s\n"
            "Vous pouvez tout de même garder ou supprimer ce fichier.",
            nom, erreur->message);
        gtk_label_set_text(GTK_LABEL(etat->details), texte);
        afficher_statut(etat, "Le fichier reste traitable malgré l'erreur d'aperçu.");
        g_free(texte);
        g_free(nom);
        g_clear_error(&erreur);
    } else {
        g_clear_object(&etat->pixbuf_apercu);
        etat->pixbuf_apercu = pixbuf;
        gtk_widget_queue_draw(etat->apercu);
        char *nom = g_filename_display_name(etat->courante->nom);
        char *taille = g_format_size((guint64)etat->courante->taille);
        char *texte = g_strdup_printf("%s — %s", nom, taille);
        gtk_label_set_text(GTK_LABEL(etat->details), texte);
        g_free(texte);
        g_free(taille);
        g_free(nom);
    }

}

/*
 * Un scan réussi remplace l'ancien flux; un échec conserve les images déjà
 * chargées. Le backend continue de décider quelles entrées sont des images.
 */
static gboolean charger_dossier(struct AppState *etat, const char *dossier)
{
    struct Image *nouvelles_images = NULL;
    char *nouveau_dossier = g_canonicalize_filename(dossier, NULL);
    if (nouveau_dossier == NULL) {
        afficher_statut(etat, "Mémoire insuffisante pour mémoriser le dossier.");
        return FALSE;
    }

    if (scanner_dossier(nouveau_dossier, &nouvelles_images) == -1) {
        const int erreur_systeme = errno;
        char *lisible = g_filename_display_name(nouveau_dossier);
        char *message = g_strdup_printf(
            "Impossible de lire le dossier « %s » : %s",
            lisible, g_strerror(erreur_systeme));
        afficher_erreur(GTK_WINDOW(etat->fenetre), "Dossier inaccessible",
                        message);
        afficher_statut(etat, "Le flux précédent est conservé.");
        g_free(message);
        g_free(lisible);
        g_free(nouveau_dossier);
        return FALSE;
    }

    GError *erreur = NULL;
    if (!enregistrer_configuration(
            etat, etat->heure_rituel, etat->quota, etat->traite,
            etat->date_compteur, nouveau_dossier, &erreur)) {
        afficher_erreur(GTK_WINDOW(etat->fenetre),
                        "Impossible d'enregistrer le dossier",
                        erreur->message);
        g_clear_error(&erreur);
        liberer_images(nouvelles_images);
        g_free(nouveau_dossier);
        return FALSE;
    }

    liberer_images(etat->images);
    etat->images = nouvelles_images;
    etat->courante = nouvelles_images;
    g_free(etat->dossier);
    etat->dossier = nouveau_dossier;
    afficher_image_courante(etat);
    afficher_statut(etat, "Nouveau flux chargé.");
    return TRUE;
}

/*
 * Chaque décision (garder ou supprimer) consomme exactement une unité du quota
 * de la session en cours.
 * Le compteur est écrit avant d'afficher l'image suivante; si l'écriture échoue,
 * l'application s'arrête pour ne pas poursuivre avec une progression ambiguë.
 */
static gboolean enregistrer_decision(struct AppState *etat)
{
    char *aujourd_hui = date_locale_du_jour();
    if (aujourd_hui == NULL) {
        afficher_erreur(GTK_WINDOW(etat->fenetre), "Progression non enregistrée",
                        "Impossible de déterminer la date locale.");
        g_application_quit(G_APPLICATION(etat->application));
        return FALSE;
    }

    const guint nouveau_traite =
        g_strcmp0(etat->date_compteur, aujourd_hui) == 0 ? etat->traite + 1 : 1;
    GError *erreur = NULL;
    if (!enregistrer_configuration(
            etat, etat->heure_rituel, etat->quota, nouveau_traite,
            aujourd_hui, etat->dossier, &erreur)) {
        afficher_erreur(GTK_WINDOW(etat->fenetre),
                        "Progression non enregistrée",
                        erreur->message);
        g_clear_error(&erreur);
        g_free(aujourd_hui);
        g_application_quit(G_APPLICATION(etat->application));
        return FALSE;
    }

    etat->traite = nouveau_traite;
    g_free(etat->date_compteur);
    etat->date_compteur = aujourd_hui;
    return TRUE;
}

/* « Garder » déplace simplement le curseur vers le maillon suivant. */
static void garder_image(GtkButton *bouton, gpointer donnees)
{
    struct AppState *etat = donnees;
    (void)bouton;
    if (etat->courante == NULL || etat->traite >= etat->quota) {
        return;
    }
    struct Image *suivante = etat->courante->suivant;
    if (!enregistrer_decision(etat)) {
        return;
    }
    etat->courante = suivante;
    afficher_image_courante(etat);
}

static gboolean mettre_image_corbeille(const struct Image *image,
                                       GError **erreur)
{
    GFile *fichier = g_file_new_for_path(image->chemin);
    GFileInfo *informations = g_file_query_info(
        fichier,
        G_FILE_ATTRIBUTE_STANDARD_TYPE ","
        G_FILE_ATTRIBUTE_UNIX_DEVICE "," G_FILE_ATTRIBUTE_UNIX_INODE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, erreur);
    if (informations == NULL) {
        g_object_unref(fichier);
        return FALSE;
    }

    const gboolean meme_fichier =
        g_file_info_get_file_type(informations) == G_FILE_TYPE_REGULAR
        && g_file_info_get_attribute_uint32(
               informations, G_FILE_ATTRIBUTE_UNIX_DEVICE)
               == (guint32)image->appareil
        && g_file_info_get_attribute_uint64(
               informations, G_FILE_ATTRIBUTE_UNIX_INODE)
               == (guint64)image->inode;
    g_object_unref(informations);
    if (!meme_fichier) {
        g_set_error_literal(erreur, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                            "Le fichier a changé depuis le scan.");
        g_object_unref(fichier);
        return FALSE;
    }

    const gboolean resultat = g_file_trash(fichier, NULL, erreur);
    g_object_unref(fichier);
    return resultat;
}

/*
 * « Supprimer » reste une opération backend : le nom et le couple device/inode
 * sont revérifiés par le backend avant unlinkat(), ou juste avant le transfert
 * vers la Corbeille XDG si ce mode a été configuré.
 */
static void supprimer_image_courante(GtkButton *bouton, gpointer donnees)
{
    struct AppState *etat = donnees;
    (void)bouton;
    if (etat->courante == NULL || etat->traite >= etat->quota) {
        return;
    }

    struct Image *image = etat->courante;
    struct Image *suivante = image->suivant;
    GError *erreur_corbeille = NULL;
    const gboolean supprime = etat->suppression_corbeille
        ? mettre_image_corbeille(image, &erreur_corbeille)
        : supprimer_image(etat->dossier, image) == 0;
    if (!supprime) {
        char *detail = etat->suppression_corbeille
            ? g_strdup_printf("Impossible de déplacer l'image vers la Corbeille : %s",
                              erreur_corbeille != NULL
                                  ? erreur_corbeille->message
                                  : "erreur inconnue")
            : g_strdup_printf("La suppression sécurisée a échoué : %s",
                              g_strerror(errno));
        afficher_erreur(GTK_WINDOW(etat->fenetre),
                        "Image non supprimée", detail);
        g_free(detail);
        g_clear_error(&erreur_corbeille);
        return;
    }

    /* Retirer le maillon de la liste avant de le libérer évite un pointeur pendant. */
    struct Image **lien = &etat->images;
    while (*lien != NULL && *lien != image) {
        lien = &(*lien)->suivant;
    }
    if (*lien == image) {
        *lien = image->suivant;
    }
    etat->courante = suivante;
    image->suivant = NULL;
    liberer_images(image);

    if (enregistrer_decision(etat)) {
        afficher_image_courante(etat);
        afficher_statut(etat, etat->suppression_corbeille
                                  ? "Image déplacée vers la Corbeille."
                                  : "Image supprimée de manière sécurisée.");
    }
}

/* Le sélecteur natif renvoie un chemin alloué par GLib, que l'appelant libère. */
static void reponse_choix_dossier(GtkNativeDialog *dialogue, gint reponse,
                                 gpointer donnees)
{
    struct AppState *etat = donnees;
    if (reponse == GTK_RESPONSE_ACCEPT) {
        char *dossier = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(dialogue));
        if (dossier != NULL) {
            (void)charger_dossier(etat, dossier);
            g_free(dossier);
        }
    }
    gtk_native_dialog_destroy(dialogue);
    g_object_unref(dialogue);
}

static void ouvrir_choix_dossier(struct AppState *etat)
{
    GtkFileChooserNative *dialogue = gtk_file_chooser_native_new(
        "Choisir un dossier d'images", GTK_WINDOW(etat->fenetre),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Choisir", "Annuler");
    if (etat->dossier != NULL
        && g_file_test(etat->dossier, G_FILE_TEST_IS_DIR)) {
        (void)gtk_file_chooser_set_current_folder(
            GTK_FILE_CHOOSER(dialogue), etat->dossier);
    }
    g_signal_connect(dialogue, "response",
                     G_CALLBACK(reponse_choix_dossier), etat);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialogue));
}

static void choisir_dossier(GtkButton *bouton, gpointer donnees)
{
    struct AppState *etat = donnees;
    (void)bouton;
    ouvrir_choix_dossier(etat);
}

static void actualiser_flux(GtkButton *bouton, gpointer donnees)
{
    struct AppState *etat = donnees;
    (void)bouton;
    (void)charger_dossier(etat, etat->dossier);
}

static void ouvrir_reglages(GtkButton *bouton, gpointer donnees)
{
    struct AppState *etat = donnees;
    (void)bouton;
    if (dialoguer_reglages(etat, FALSE)) {
        (void)charger_dossier(etat, etat->dossier);
        afficher_statut(etat, "Réglages enregistrés.");
    }
}

/*
 * Reporter met fin à cette session sans traiter l'image courante. Le timer
 * système peut relancer l'application au prochain rituel; fermer la fenêtre
 * ne marque jamais automatiquement une image comme traitée.
 */
static void reporter_session(GtkButton *bouton, gpointer donnees)
{
    struct AppState *etat = donnees;
    (void)bouton;
    gboolean reporter = etat->comportement_pause
        == COMPORTEMENT_PAUSE_REPORTER;
    if (etat->comportement_pause == COMPORTEMENT_PAUSE_CHOIX) {
        GtkWidget *dialogue = gtk_message_dialog_new(
            GTK_WINDOW(etat->fenetre), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "Que souhaitez-vous faire ?");
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialogue),
            "L'image actuelle ne sera pas marquée comme traitée.");
        gtk_dialog_add_button(GTK_DIALOG(dialogue), "Annuler",
                              GTK_RESPONSE_CANCEL);
        gtk_dialog_add_button(GTK_DIALOG(dialogue), "Fermer maintenant", 1);
        gtk_dialog_add_button(GTK_DIALOG(dialogue), "Reporter", 2);
        const gint reponse = gtk_dialog_run(GTK_DIALOG(dialogue));
        gtk_widget_destroy(dialogue);
        if (reponse == GTK_RESPONSE_CANCEL || reponse == GTK_RESPONSE_DELETE_EVENT) {
            return;
        }
        reporter = reponse == 2;
    }

    if (!reporter) {
        g_application_quit(G_APPLICATION(etat->application));
        return;
    }

    const gint64 ancien_report = etat->reporter_jusqua;
    etat->reporter_jusqua = g_get_real_time() / G_USEC_PER_SEC
        + (gint64)etat->delai_report_minutes * 60;
    GError *erreur = NULL;
    if (!enregistrer_configuration(
            etat, etat->heure_rituel, etat->quota, etat->traite,
            etat->date_compteur, etat->dossier, &erreur)) {
        etat->reporter_jusqua = ancien_report;
        afficher_erreur(GTK_WINDOW(etat->fenetre),
                        "Report non enregistré", erreur->message);
        g_clear_error(&erreur);
        return;
    }
    g_application_quit(G_APPLICATION(etat->application));
}

static void fenetre_detruite(GtkWidget *fenetre, gpointer donnees)
{
    struct AppState *etat = donnees;
    (void)fenetre;
    etat->fenetre = NULL;
}

/* Création de l'interface : un seul aperçu central, sans liste concurrente. */
static void activer_application(GtkApplication *application, gpointer donnees)
{
    struct AppState *etat = donnees;
    if (etat->fenetre != NULL) {
        gtk_window_present(GTK_WINDOW(etat->fenetre));
        char *aujourd_hui = date_locale_du_jour();
        if (aujourd_hui == NULL) {
            afficher_erreur(GTK_WINDOW(etat->fenetre),
                            "Progression quotidienne non actualisée",
                            "Impossible de déterminer la date locale.");
        } else if (g_strcmp0(etat->date_compteur, aujourd_hui) != 0) {
            GError *erreur = NULL;
            if (enregistrer_configuration(
                    etat, etat->heure_rituel, etat->quota, 0,
                    aujourd_hui, etat->dossier, &erreur)) {
                etat->traite = 0;
                g_free(etat->date_compteur);
                etat->date_compteur = aujourd_hui;
                afficher_image_courante(etat);
            } else {
                afficher_erreur(GTK_WINDOW(etat->fenetre),
                                "Progression quotidienne non actualisée",
                                erreur->message);
                g_clear_error(&erreur);
                g_free(aujourd_hui);
            }
        } else {
            g_free(aujourd_hui);
        }
        return;
    }

    if (etat->premier_lancement) {
        if (!dialoguer_reglages(etat, TRUE)) {
            g_application_quit(G_APPLICATION(application));
            return;
        }
        etat->premier_lancement = FALSE;
    }

    etat->fenetre = gtk_application_window_new(application);
    g_signal_connect(etat->fenetre, "destroy",
                     G_CALLBACK(fenetre_detruite), etat);
    gtk_window_set_title(GTK_WINDOW(etat->fenetre), "Cleaner — rituel d'images");
    gtk_window_set_resizable(GTK_WINDOW(etat->fenetre), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(etat->fenetre), 800, 600);
    gtk_window_set_position(GTK_WINDOW(etat->fenetre), GTK_WIN_POS_CENTER);

    GtkWidget *conteneur = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(conteneur), 12);
    gtk_container_add(GTK_CONTAINER(etat->fenetre), conteneur);

    GtkWidget *titre = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(titre),
                         "<span size='xx-large' weight='bold'>Cleaner</span>");
    gtk_box_pack_start(GTK_BOX(conteneur), titre, FALSE, FALSE, 0);

    etat->progression = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(conteneur), etat->progression, FALSE, FALSE, 0);

    etat->apercu = gtk_drawing_area_new();
    gtk_widget_set_halign(etat->apercu, GTK_ALIGN_FILL);
    gtk_widget_set_valign(etat->apercu, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(etat->apercu, TRUE);
    gtk_widget_set_vexpand(etat->apercu, TRUE);
    g_signal_connect(etat->apercu, "draw", G_CALLBACK(dessiner_apercu), etat);
    gtk_box_pack_start(GTK_BOX(conteneur), etat->apercu, TRUE, TRUE, 0);

    etat->details = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(etat->details), TRUE);
    gtk_box_pack_start(GTK_BOX(conteneur), etat->details, FALSE, FALSE, 0);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(actions, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(actions, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(conteneur), actions, FALSE, FALSE, 0);
    etat->bouton_garder = gtk_button_new_with_label("Garder");
    etat->bouton_supprimer = gtk_button_new_with_label("Supprimer");
    GtkWidget *bouton_pause = gtk_button_new_with_label("Reporter / Pause");
    gtk_widget_set_size_request(etat->bouton_garder, 150, 44);
    gtk_widget_set_size_request(etat->bouton_supprimer, 150, 44);
    gtk_box_pack_start(GTK_BOX(actions), etat->bouton_garder, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), etat->bouton_supprimer, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), bouton_pause, FALSE, FALSE, 0);

    GtkWidget *outils = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(outils, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(outils, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(conteneur), outils, FALSE, FALSE, 0);
    GtkWidget *bouton_dossier = gtk_button_new_with_label("Choisir un dossier");
    GtkWidget *bouton_actualiser = gtk_button_new_with_label("Actualiser");
    GtkWidget *bouton_reglages = gtk_button_new_with_label("Réglages");
    gtk_box_pack_start(GTK_BOX(outils), bouton_dossier, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outils), bouton_actualiser, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outils), bouton_reglages, FALSE, FALSE, 0);

    etat->statut = gtk_label_new("");
    gtk_widget_set_halign(etat->statut, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(conteneur), etat->statut, FALSE, FALSE, 0);

    g_signal_connect(etat->bouton_garder, "clicked",
                     G_CALLBACK(garder_image), etat);
    g_signal_connect(etat->bouton_supprimer, "clicked",
                     G_CALLBACK(supprimer_image_courante), etat);
    g_signal_connect(bouton_pause, "clicked",
                     G_CALLBACK(reporter_session), etat);
    g_signal_connect(bouton_dossier, "clicked",
                     G_CALLBACK(choisir_dossier), etat);
    g_signal_connect(bouton_actualiser, "clicked",
                     G_CALLBACK(actualiser_flux), etat);
    g_signal_connect(bouton_reglages, "clicked",
                     G_CALLBACK(ouvrir_reglages), etat);

    gtk_widget_show_all(etat->fenetre);
    afficher_image_courante(etat);
    if (etat->choisir_dossier_initial) {
        etat->choisir_dossier_initial = FALSE;
        afficher_statut(etat,
                       "Le dossier de captures par défaut est introuvable. "
                       "Choisissez un dossier à nettoyer.");
        ouvrir_choix_dossier(etat);
    } else if (!charger_dossier(etat, etat->dossier)) {
        afficher_statut(etat,
                        "Dossier inaccessible. Choisissez un autre dossier "
                        "ou réessayez plus tard.");
    }
}

/*
 * Entrée publique du module graphique. Le chemin de configuration suit la
 * convention XDG (~/.config/cleaner/config.conf); aucune donnée n'est écrite
 * dans le dossier d'images ni dans le répertoire d'installation système.
 */
int lancer_interface_graphique(const char *chemin_dossier,
                               int lancement_programme)
{
    struct AppState etat = {0};
    etat.chemin_configuration = g_build_filename(
        g_get_user_config_dir(), "cleaner", "config.conf", NULL);
    if (etat.chemin_configuration == NULL) {
        fprintf(stderr, "Impossible de préparer le chemin de configuration.\n");
        return EXIT_FAILURE;
    }

    GError *erreur = NULL;
    const int configuration = charger_configuration(
        &etat, chemin_dossier, &erreur);
    if (configuration == -1) {
        fprintf(stderr, "Configuration Cleaner : %s\n",
                erreur != NULL ? erreur->message : "erreur inconnue");
        g_clear_error(&erreur);
        g_free(etat.chemin_configuration);
        g_free(etat.dossier);
        g_free(etat.date_compteur);
        return EXIT_FAILURE;
    }
    etat.premier_lancement = configuration == 0;
    gboolean reprise_report = FALSE;

    /*
     * systemd appelle ce contrôle chaque minute. Sans configuration, il ne
     * faut pas ouvrir l'assistant au démarrage du timer : le premier lancement
     * interactif initialise les préférences, puis le timer les suit ici.
     */
    if (lancement_programme) {
        if (configuration == 0) {
            g_free(etat.chemin_configuration);
            g_free(etat.dossier);
            g_free(etat.date_compteur);
            return EXIT_SUCCESS;
        }

        GDateTime *maintenant = g_date_time_new_now_local();
        if (maintenant == NULL) {
            fprintf(stderr, "Impossible de déterminer l'heure locale.\n");
            g_free(etat.chemin_configuration);
            g_free(etat.dossier);
            g_free(etat.date_compteur);
            return EXIT_FAILURE;
        }
        char *heure_actuelle = g_date_time_format(maintenant, "%H:%M");
        const gint64 maintenant_epoch =
            g_date_time_to_unix(maintenant);
        g_date_time_unref(maintenant);
        if (heure_actuelle == NULL) {
            fprintf(stderr, "Impossible de formater l'heure locale.\n");
            g_free(etat.chemin_configuration);
            g_free(etat.dossier);
            g_free(etat.date_compteur);
            return EXIT_FAILURE;
        }
        gboolean rituel_du =
            g_strcmp0(heure_actuelle, etat.heure_rituel) == 0;
        g_free(heure_actuelle);
        if (etat.reporter_jusqua != 0) {
            if (maintenant_epoch < etat.reporter_jusqua) {
                rituel_du = FALSE;
            } else {
                const gint64 ancien_report = etat.reporter_jusqua;
                etat.reporter_jusqua = 0;
                GError *erreur_report = NULL;
                if (!enregistrer_configuration(
                        &etat, etat.heure_rituel, etat.quota, etat.traite,
                        etat.date_compteur, etat.dossier, &erreur_report)) {
                    etat.reporter_jusqua = ancien_report;
                    fprintf(stderr, "Impossible d'actualiser le report : %s\n",
                            erreur_report->message);
                    g_clear_error(&erreur_report);
                    g_free(etat.chemin_configuration);
                    g_free(etat.dossier);
                    g_free(etat.date_compteur);
                    return EXIT_FAILURE;
                }
                reprise_report = TRUE;
                rituel_du = TRUE;
            }
        }
        if (!rituel_du) {
            g_free(etat.chemin_configuration);
            g_free(etat.dossier);
            g_free(etat.date_compteur);
            return EXIT_SUCCESS;
        }
    }

    /*
     * Un lancement manuel ou le rituel quotidien ouvre une nouvelle session ;
     * un report, lui, reprend la progression de la session interrompue.
     */
    if ((!lancement_programme || !reprise_report) && etat.traite != 0) {
        etat.traite = 0;
        GError *erreur_session = NULL;
        if (!enregistrer_configuration(
                &etat, etat.heure_rituel, etat.quota, 0,
                etat.date_compteur, etat.dossier, &erreur_session)) {
            fprintf(stderr, "Impossible d'initialiser la session : %s\n",
                    erreur_session->message);
            g_clear_error(&erreur_session);
            g_free(etat.chemin_configuration);
            g_free(etat.dossier);
            g_free(etat.date_compteur);
            return EXIT_FAILURE;
        }
    }

    etat.application = gtk_application_new("org.example.cleaner", 0);
    if (etat.application == NULL) {
        fprintf(stderr, "Impossible de créer l'application GTK.\n");
        liberer_images(etat.images);
        g_free(etat.chemin_configuration);
        g_free(etat.dossier);
        g_free(etat.date_compteur);
        return EXIT_FAILURE;
    }

    g_signal_connect(etat.application, "activate",
                     G_CALLBACK(activer_application), &etat);

    /*
     * GTK reçoit seulement le nom du programme, pas les arguments métier déjà
     * traités ici. Le dialogue de premier lancement s'affiche dans activate.
     */
    char *arguments_gtk[] = {(char *)"cleaner", NULL};
    const int boucle = g_application_run(G_APPLICATION(etat.application),
                                         1, arguments_gtk);

    if (etat.fenetre != NULL) {
        gtk_widget_destroy(etat.fenetre);
    }
    liberer_images(etat.images);
    g_clear_object(&etat.pixbuf_apercu);
    g_object_unref(etat.application);
    g_free(etat.chemin_configuration);
    g_free(etat.dossier);
    g_free(etat.date_compteur);
    return boucle;
}
