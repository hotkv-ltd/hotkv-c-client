/*
 * Smallest possible consumer of an installed libhotkv: links the library and
 * calls a function that needs no server. Used by the release workflow to
 * prove that the install tree works through find_package() and pkg-config.
 */
#include <hotkv.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    const char *key = "foo";
    unsigned slot = hotkv_key_slot(key, strlen(key));

    printf("libhotkv %s, slot of \"foo\" = %u\n", HOTKV_VERSION_STRING, slot);
    return slot == 12182u ? 0 : 1;
}
