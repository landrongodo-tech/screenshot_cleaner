#ifndef GUI_H
#define GUI_H

/*
 * Lance l'application GTK3 pour le dossier indiqué. En mode programmé,
 * l'interface ne s'ouvre que si l'heure configurée correspond à l'heure locale.
 * Le chemin est copié par l'application avant le retour de cette fonction.
 */
int lancer_interface_graphique(const char *chemin_dossier,
                               int lancement_programme);

#endif
