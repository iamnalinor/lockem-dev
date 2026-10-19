#include "pkgname.h"
#include <ctype.h>
#include <string.h>

/* packages we refuse to lock — hiding/force-stopping these can destabilize the
 * system or break the root-grant / recovery chain lockem itself relies on.
 * The root-manager apps are blocked on purpose: hiding the one that shows the su
 * prompt can wedge new su grants. To hide the manager, use the manager's own
 * repackage/hide feature instead. (The WebUI host — KsuWebUI/MMRL — is NOT blocked:
 * hiding it only costs the config UI until unlock, a deliberate user choice.) */
static const char *BLOCKLIST[] = {"android",
                                  "com.android.systemui",
                                  "com.android.settings",
                                  "com.android.launcher3",
                                  "com.google.android.apps.nexuslauncher",
                                  "com.google.android.gms",
                                  "com.android.phone",
                                  "com.android.shell",
                                  "com.topjohnwu.magisk", /* Magisk manager */
                                  "me.weishu.kernelsu",   /* KernelSU manager */
                                  "me.bmax.apatch",       /* APatch manager */
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
