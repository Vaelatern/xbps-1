/*-
 * Copyright (c) 2008-2011 Juan Romero Pardines.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <xbps_api.h>
#include "defs.h"
#include "../xbps-repo/defs.h"

static int
pkg_remove_and_purge(const char *pkgname, const char *version, bool purge)
{
	int rv = 0;

	printf("Removing package %s-%s ...\n", pkgname, version);

	if ((rv = xbps_remove_pkg(pkgname, version, false)) != 0) {
		xbps_error_printf("xbps-bin: unable to remove %s-%s (%s).\n",
		    pkgname, version, strerror(errno));
		return rv;
	}
	if (purge) {
		printf(" Purging ... ");
		(void)fflush(stdout);
		if ((rv = xbps_purge_pkg(pkgname, false)) != 0) {
			xbps_error_printf("xbps-bin: unable to purge %s-%s "
			    "(%s).\n", pkgname, version,
			    strerror(errno));
			return rv;
		}
		printf("done.\n");
	}

	return rv;
}

int
autoremove_pkgs(bool yes, bool purge)
{
	prop_array_t orphans = NULL;
	prop_object_t obj = NULL;
	prop_object_iterator_t iter = NULL;
	const char *pkgver, *pkgname, *version;
	int rv = 0;

	/*
	 * Removes orphan pkgs. These packages were installed
	 * as dependency and any installed package does not depend
	 * on it currently.
	 */
	orphans = xbps_find_pkg_orphans(NULL);
	if (orphans == NULL)
		return errno;

	if (prop_array_count(orphans) == 0) {
		printf("There are not orphaned packages currently.\n");
		goto out;
	}

	iter = prop_array_iterator(orphans);
	if (iter == NULL) {
		rv = errno;
		goto out;
	}

	printf("The following packages were installed automatically\n"
	    "(as dependencies) and aren't needed anymore:\n\n");
	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgver", &pkgver);
		print_package_line(pkgver, false);
	}
	prop_object_iterator_reset(iter);
	printf("\n\n");

	if (!yes && !noyes("Do you want to continue?")) {
		printf("Cancelled!\n");
		goto out;
	}

	while ((obj = prop_object_iterator_next(iter)) != NULL) {
		prop_dictionary_get_cstring_nocopy(obj, "pkgname", &pkgname);
		prop_dictionary_get_cstring_nocopy(obj, "version", &version);
		if ((rv = pkg_remove_and_purge(pkgname, version, purge)) != 0)
			goto out;
	}

out:
	if (iter)
		prop_object_iterator_release(iter);
	if (orphans)
		prop_object_release(orphans);

	return rv;
}

int
remove_installed_pkgs(int argc, char **argv, bool yes, bool purge,
		      bool force_rm_with_deps, bool recursive_rm)
{
	prop_array_t sorted, unsorted, orphans, reqby, orphans_user = NULL;
	prop_dictionary_t dict;
	size_t x;
	const char *version, *pkgver, *pkgname;
	int i, rv = 0;
	bool found = false, reqby_force = false;

	unsorted = prop_array_create();
	if (unsorted == NULL)
		return -1;

	sorted = prop_array_create();
	if (sorted == NULL)
		return -1;
	/*
	 * If recursively removing packages, find out which packages
	 * would be orphans if the supplied packages were already removed.
	 */
	if (recursive_rm) {
		orphans_user = prop_array_create();
		if (orphans_user == NULL) {
			xbps_error_printf("NULL orphans_user array\n");
			return ENOMEM;
		}
		for (x = 0, i = 1; i < argc; i++, x++)
			prop_array_set_cstring_nocopy(orphans_user, x, argv[i]);

		orphans = xbps_find_pkg_orphans(orphans_user);
		prop_object_release(orphans_user);
		if (orphans == NULL) {
			xbps_error_printf("NULL orphans array\n");
			return EINVAL;
		}
		for (x = 0; x < prop_array_count(orphans); x++)
			prop_array_add(unsorted, prop_array_get(orphans, x));

		prop_object_release(orphans);
	}
	/*
	 * First check if package is required by other packages.
	 */
	for (i = 1; i < argc; i++) {
		dict = xbps_find_pkg_dict_installed(argv[i], false);
		if (dict == NULL) {
			printf("Package %s is not installed.\n", argv[i]);
			continue;
		}
		/*
		 * Check that current package is not required by
		 * other installed packages.
		 */
		prop_array_add(sorted, dict);
		found = true;
		prop_dictionary_get_cstring_nocopy(dict, "pkgver", &pkgver);
		reqby = prop_dictionary_get(dict, "requiredby");
		if (reqby == NULL || prop_array_count(reqby) == 0) {
			prop_object_release(dict);
			continue;
		}

		xbps_printf("WARNING: %s IS REQUIRED BY %u PACKAGE%s:\n\n",
		    pkgver, prop_array_count(reqby),
		    prop_array_count(reqby) > 1 ? "S" : "");
		for (x = 0; x < prop_array_count(reqby); x++) {
			prop_array_get_cstring_nocopy(reqby, x, &pkgver);
			print_package_line(pkgver, false);
		}
		printf("\n\n");
		print_package_line(NULL, true);
		reqby_force = true;
		prop_object_release(dict);
	}
	if (!found) {
		prop_object_release(unsorted);
		return 0;
	}
	if (reqby_force && !force_rm_with_deps) {
		prop_object_release(unsorted);
		prop_object_release(sorted);
		return EINVAL;
	}
	for (x = 0; x < prop_array_count(unsorted); x++) {
		dict = prop_array_get(unsorted, x);
		prop_array_add(sorted, dict);
	}
	prop_object_release(unsorted);
	/*
	 * Show the list of going-to-be removed packages.
	 */
	printf("The following packages will be removed:\n\n");
	for (x = 0; x < prop_array_count(sorted); x++) {
		dict = prop_array_get(sorted, x);
		prop_dictionary_get_cstring_nocopy(dict, "pkgver", &pkgver);
		print_package_line(pkgver, false);
	}
	printf("\n\n");
	printf("%u package%s will be removed%s.\n\n",
	    prop_array_count(sorted),
	    prop_array_count(sorted) == 1 ? "" : "s",
	    purge ? " and purged" : "");

	if (!yes && !noyes("Do you want to continue?")) {
		printf("Cancelling!\n");
		prop_object_release(sorted);
		return 0;
	}

	for (x = 0; x < prop_array_count(sorted); x++) {
		dict = prop_array_get(sorted, x);
		prop_dictionary_get_cstring_nocopy(dict, "pkgname", &pkgname);
		prop_dictionary_get_cstring_nocopy(dict, "version", &version);
		if ((rv = pkg_remove_and_purge(pkgname, version, purge)) != 0) {
			prop_object_release(sorted);
			return rv;
		}
	}
	prop_object_release(sorted);

	return 0;
}
