/* The developer build marker (docs/55). Used only inside #if DES_DEV: des_dev.cpp prints it at start and the
 * settings window's Developer tab shows it. tools/ci/check_release_no_dev.py fails a release whose files hold it. */
#pragma once
#define DES_DEV_MARKER "DES-DEV-BUILD: developer cheats compiled in"
