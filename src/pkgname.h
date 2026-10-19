#ifndef LOCKEM_PKGNAME_H
#define LOCKEM_PKGNAME_H

/* Package-name gates, factored out of actions.c so they can be unit-tested
   without pulling in the system()/popen() action code. */

/* 1 if pkg is a well-formed package name: non-empty, only [A-Za-z0-9._].
   This is both a sanity gate and a hard shell-injection guard, since package
   names flow into system()/popen() command strings. */
int pkg_valid(const char *pkg);

/* 1 if pkg is on the critical-package blocklist (hiding/force-stopping it could
   destabilize the system), so the daemon must refuse to lock it. */
int pkg_blocked(const char *pkg);

#endif
