#ifndef UI_H
#define UI_H

#include "file.h"

/*
 * Lance l'interface ncurses sur la liste déjà construite par le moteur.
 * La fonction peut mettre à jour cette liste après une suppression ou un scan
 * demandé par l'utilisateur. L'appelant reste propriétaire de la liste et doit
 * appeler liberer_images() après le retour.
 */
int lancer_interface_tui(const char *chemin_dossier, struct Image **images);

#endif
