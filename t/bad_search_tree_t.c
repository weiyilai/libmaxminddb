// Required for mkstemp visibility under -D_POSIX_C_SOURCE=200112L on glibc.
#define _XOPEN_SOURCE 500

#include "maxminddb_test_helper.h"

#include <errno.h>
#include <string.h>

#ifdef _WIN32
    #include <io.h>
    #include <stdio.h>
    #define unlink _unlink
#else
    #include <unistd.h>
#endif

/*
 * Test the off-by-one fix in MMDB_read_node: node_number >= node_count
 * must return MMDB_INVALID_NODE_NUMBER_ERROR. Previously the check used
 * >, allowing node_number == node_count to read past the tree.
 */
void test_read_node_bounds(void) {
    char *db_file = test_database_path("MaxMind-DB-test-ipv4-24.mmdb");
    MMDB_s *mmdb = open_ok(db_file, MMDB_MODE_MMAP, "mmap mode");
    free(db_file);

    if (!mmdb) {
        return;
    }

    MMDB_search_node_s node;

    /* node_count - 1 is the last valid node */
    int status = MMDB_read_node(mmdb, mmdb->metadata.node_count - 1, &node);
    cmp_ok(status,
           "==",
           MMDB_SUCCESS,
           "MMDB_read_node succeeds for last valid node");

    /* node_count itself is out of bounds (the off-by-one fix) */
    status = MMDB_read_node(mmdb, mmdb->metadata.node_count, &node);
    cmp_ok(status,
           "==",
           MMDB_INVALID_NODE_NUMBER_ERROR,
           "MMDB_read_node rejects node_number == node_count");

    /* node_count + 1 is also out of bounds */
    status = MMDB_read_node(mmdb, mmdb->metadata.node_count + 1, &node);
    cmp_ok(status,
           "==",
           MMDB_INVALID_NODE_NUMBER_ERROR,
           "MMDB_read_node rejects node_number > node_count");

    MMDB_close(mmdb);
    free(mmdb);
}

static FILE *open_temp_file(char *path, size_t path_size) {
#ifdef _WIN32
    errno_t err = tmpnam_s(path, path_size);
    if (err != 0) {
        return NULL;
    }
    return fopen(path, "wb");
#else
    snprintf(path, path_size, "./bad-search-tree-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        return NULL;
    }
    return fdopen(fd, "wb");
#endif
}

static int copy_file(FILE *dest, const char *source_path) {
    FILE *source = fopen(source_path, "rb");
    if (!source) {
        return -1;
    }

    unsigned char buffer[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, bytes_read, dest) != bytes_read) {
            fclose(source);
            return -1;
        }
    }

    int result = ferror(source) ? -1 : 0;
    fclose(source);
    return result;
}

void test_separator_record_rejected(void) {
    char *db_file = test_database_path("MaxMind-DB-test-ipv4-24.mmdb");
    char temp_path[64];
    FILE *temp = open_temp_file(temp_path, sizeof(temp_path));
    if (!temp) {
        free(db_file);
        BAIL_OUT("could not create temp file: %s", strerror(errno));
    }

    if (copy_file(temp, db_file) != 0) {
        fclose(temp);
        unlink(temp_path);
        free(db_file);
        BAIL_OUT("could not copy test database: %s", strerror(errno));
    }

    // Overwrite node 0's left record with node_count + 1, which points into
    // the 16-byte separator between the search tree and data section.
    if (fseek(temp, 0, SEEK_SET) != 0) {
        fclose(temp);
        unlink(temp_path);
        free(db_file);
        BAIL_OUT("fseek failed: %s", strerror(errno));
    }
    unsigned char bad_record[3] = {0x00, 0x00, 0xA4};
    if (fwrite(bad_record, 1, sizeof(bad_record), temp) != sizeof(bad_record)) {
        fclose(temp);
        unlink(temp_path);
        free(db_file);
        BAIL_OUT("fwrite failed: %s", strerror(errno));
    }
    if (fclose(temp) != 0) {
        unlink(temp_path);
        free(db_file);
        BAIL_OUT("fclose failed: %s", strerror(errno));
    }

    MMDB_s mmdb;
    int status = MMDB_open(temp_path, MMDB_MODE_MMAP, &mmdb);
    cmp_ok(status, "==", MMDB_SUCCESS, "opened crafted bad search tree MMDB");
    if (status != MMDB_SUCCESS) {
        unlink(temp_path);
        free(db_file);
        return;
    }

    MMDB_search_node_s node;
    status = MMDB_read_node(&mmdb, 0, &node);
    cmp_ok(
        status, "==", MMDB_SUCCESS, "MMDB_read_node succeeds for crafted node");
    cmp_ok(node.left_record_type,
           "==",
           MMDB_RECORD_TYPE_INVALID,
           "MMDB_read_node marks separator record as invalid");

    int gai_error = 0;
    int mmdb_error = 0;
    MMDB_lookup_result_s result =
        MMDB_lookup_string(&mmdb, "1.1.1.1", &gai_error, &mmdb_error);
    cmp_ok(gai_error, "==", 0, "lookup string parse succeeds");
    cmp_ok(mmdb_error,
           "==",
           MMDB_CORRUPT_SEARCH_TREE_ERROR,
           "MMDB_lookup_string rejects records pointing into the separator");
    ok(!result.found_entry, "lookup does not report an entry for corrupt tree");

    MMDB_close(&mmdb);
    unlink(temp_path);
    free(db_file);
}

int main(void) {
    plan(NO_PLAN);
    test_read_node_bounds();
    test_separator_record_rejected();
    done_testing();
}
