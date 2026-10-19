#include "pkgname.h"
#include <ctype.h>
#include <string.h>

/* HARD blocklist — packages we refuse to lock unconditionally, because
 * hiding/force-stopping them would brick boot or destabilize the UI/system.
 *
 * Root-manager apps and WebUI hosts are deliberately NOT here: hiding the Magisk/
 * KernelSU/APatch app is a legitimate anti-detection feature. The WebUI gates them
 * behind an explicit confirm in its "Magisk-related" section (a deliberate, riskier
 * user choice — a hidden manager can't show a *new* su prompt until unlock), rather
 * than the daemon blocking them outright. */
static const char *BLOCKLIST[] = {"android",
                                  "com.android.systemui",
                                  "com.android.settings",
                                  "com.android.launcher3",
                                  "com.google.android.apps.nexuslauncher",
                                  "com.google.android.gms",
                                  "com.android.phone",
                                  "com.android.shell",
                                  NULL};

int pkg_valid(const char *pkg) {
    if (!pkg || !pkg[0]) return 0;
    for (const char *p = pkg; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (!(isalnum(ch) || ch == '.' || ch == '_')) return 0;
    }
    return 1;
}

int pkg_blocked(const char *pkg) {
    for (int i = 0; BLOCKLIST[i]; i++)
        if (strcmp(BLOCKLIST[i], pkg) == 0) return 1;
    return 0;
}
