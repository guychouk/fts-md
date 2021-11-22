#ifdef __APPLE__
#ifndef st_mtime
#define st_mtime st_mtimespec.tv_sec
#endif
#endif

#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sqlite3.h>
#include <stdbool.h>

struct str_node_s;

typedef struct {
   struct str_node_s *next;
   char *name;
} str_node_t;

bool memberInList(str_node_t *list, char *name) {
   while (list->name) {
      if(strcmp(list->name, name) == 0) {
         return true;
      } else {
         list = list->next;
      }
   }
   return false;
}


static int callback(void *data, int argc, char **argv, char **azColName) {
    int i;
    fprintf(stderr, "%s: ", (const char*)data);

    for(i = 0; i<argc; i++) {
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }

    printf("\n");
    return 0;
}

/*
 * 'slurp' reads the file identified by 'path' into a character buffer
 * pointed at by 'buf', optionally adding a terminating NUL if
 * 'add_nul' is true. On success, the size of the file is returned; on
 * failure, -1 is returned and ERRNO is set by the underlying system
 * or library call that failed.
 *
 * WARNING: 'slurp' malloc()s memory to '*buf' which must be freed by
 * the caller.
 */
long slurp(char const* path, char **buf, bool add_nul)
{
    FILE  *fp;
    size_t fsz;
    long   off_end;
    int    rc;

    /* Open the file */
    fp = fopen(path, "rb");
    if( NULL == fp ) {
        return -1L;
    }

    /* Seek to the end of the file */
    rc = fseek(fp, 0L, SEEK_END);
    if( 0 != rc ) {
        return -1L;
    }

    /* Byte offset to the end of the file (size) */
    if( 0 > (off_end = ftell(fp)) ) {
        return -1L;
    }
    fsz = (size_t)off_end;

    /* Allocate a buffer to hold the whole file */
    *buf = malloc( fsz+(int)add_nul );
    if( NULL == *buf ) {
        return -1L;
    }

    /* Rewind file pointer to start of file */
    rewind(fp);

    /* Slurp file into buffer */
    if( fsz != fread(*buf, 1, fsz, fp) ) {
        free(*buf);
        return -1L;
    }

    /* Close the file */
    if( EOF == fclose(fp) ) {
        free(*buf);
        return -1L;
    }

    if( add_nul ) {
        /* Make sure the buffer is NUL-terminated, just in case */
        buf[fsz] = '\0';
    }

    /* Return the file size */
    return (long)fsz;
}

int main(int argc, char* argv[]) {

    int     rc;
    DIR*    FD;
    struct  dirent* in_file;
    FILE    *entry_file;
    char    buffer[BUFSIZ];
    struct  stat result;
    char    *zErrMsg = 0;
    const   unsigned char *name, *state;
    const   unsigned char *bbb;
    const   unsigned char *ccc;
    const   char* data = "Callback function called";
    char    *file_cat;
    int     option;
    sqlite3 *db;

    while((option = getopt(argc, argv, ":f:")) != -1) {
       switch(option){
          case 'f':
             file_cat = optarg;
             break;
       }
    }

    /* Setup SQLite */
    rc = sqlite3_open("index.db", &db);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    /* Create table if it doesn't exist */
    char *create_table = "CREATE VIRTUAL TABLE IF NOT EXISTS zettelkasten"
                         " USING fts5(title, body, tags, mtime UNINDEXED, prefix = 3, tokenize = \"porter unicode61\");";

    rc = sqlite3_exec(db, create_table, callback, 0, &zErrMsg);

    if(rc != SQLITE_OK) {
        fprintf(stderr, "Failed to create table: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    } 

    /* Setup full text search */
    char *setup_fts = "INSERT INTO zettelkasten (zettelkasten, rank)"
                      " VALUES('rank', 'bm25(2.0, 1.0, 5.0, 0.0)');";

    rc = sqlite3_exec(db, setup_fts, callback, 0, &zErrMsg);

    if(rc != SQLITE_OK) {
        fprintf(stderr, "Failed setting up FTS: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    /* Fetch all existing entries in the DB and save them for later */
    char *existing_notes = "SELECT title FROM zettelkasten;";

    sqlite3_stmt *pstmt;
    rc = sqlite3_prepare_v2(db, existing_notes, -1, &pstmt, NULL);
    if (rc)
    {
        fprintf(stderr, "Couldn't prepare sql statement: %s\n", sqlite3_errmsg(db));
        sqlite3_finalize(pstmt);
        sqlite3_close_v2(db);
        return(1);
    }

    str_node_t *first = calloc(1, sizeof(str_node_t));
    str_node_t *current = first;

    while(sqlite3_step(pstmt) == SQLITE_ROW)
    {
        name = sqlite3_column_text(pstmt, 0);
        if (!name)
           continue;

        current->name = calloc(1, strlen(name));
        strcpy(current->name, name);

        current->next = calloc(1, sizeof(str_node_t));
        current = current->next;
    }

    sqlite3_finalize(pstmt);

    /* Iterate over Markdown files in current directory */
    if (NULL == (FD = opendir(".")))
    {
        fprintf(stderr, "Error : Failed to open input directory - %s\n", strerror(errno));
        return 1;
    }

    while ((in_file = readdir(FD)))
    {
        long mtime;
        char *note_body;
        struct stat file_attributes;

        char* point = strrchr(in_file->d_name, '.');

        if (!strcmp(in_file->d_name, "."))
            continue;
        if (!strcmp(in_file->d_name, ".."))
            continue;
        if (point == NULL || strcmp(point, ".md") != 0)
            continue;
        if (memberInList(first, in_file->d_name))
           continue;

        entry_file = fopen(in_file->d_name, "r");

        if (entry_file == NULL)
        {
            fprintf(stderr, "Error : Failed to open entry file - %s\n", strerror(errno));
            return 1;
        }

        stat(in_file->d_name, &file_attributes);
        mtime = file_attributes.st_mtime;

        if (slurp(in_file->d_name, &note_body, false) < 0L) {
           perror("File is empty!");
           return 1;
        }

        char *upsert_note = "INSERT OR IGNORE INTO zettelkasten (title, body, tags, mtime)"
           " VALUES (@title, @body, @tags, @mtime); UPDATE zettelkasten SET mtime=@mtime, body=@body;";
        sqlite3_stmt *upstmt;

        rc = sqlite3_prepare_v2(db, upsert_note, -1, &upstmt, NULL);
        if (rc == SQLITE_OK) {
           sqlite3_bind_int(upstmt, sqlite3_bind_parameter_index(upstmt, "@mtime"), mtime);
           sqlite3_bind_text(upstmt, sqlite3_bind_parameter_index(upstmt, "@tags"), &(""), -1, NULL);
           sqlite3_bind_text(upstmt, sqlite3_bind_parameter_index(upstmt, "@body"), note_body, -1, NULL);
           sqlite3_bind_text(upstmt, sqlite3_bind_parameter_index(upstmt, "@title"), &in_file->d_name, -1, NULL);
        } else {
           fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
        }

        if (sqlite3_step(upstmt) != SQLITE_DONE) {
           fprintf(stderr, "Failed to step: %s\n", sqlite3_errmsg(db));
        }

        sqlite3_finalize(upstmt);
        fclose(entry_file);
    }
   
    if (file_cat) {
       if (argc == 4) {
          char *query = argv[3];
          if (query[0] == '\0' || query[0] == ' ') {
              return 1;
          }
          char *query_for_notes = "SELECT rank, highlight(zettelkasten, 1, '\x1b[0;41m', '\x1b[0m') AS body"
             " FROM zettelkasten WHERE title = @title AND zettelkasten MATCH @query ORDER BY rank;";
          sqlite3_stmt *querystmt;

          rc = sqlite3_prepare_v2(db, query_for_notes, -1, &querystmt, NULL);
          if (rc == SQLITE_OK) {
             sqlite3_bind_text(querystmt, sqlite3_bind_parameter_index(querystmt, "@query"), query, -1, NULL);
             sqlite3_bind_text(querystmt, sqlite3_bind_parameter_index(querystmt, "@title"), file_cat, -1, NULL);
          } else {
             fprintf(stderr, "Failed to execute statement: %s\n", sqlite3_errmsg(db));
          }

          while(sqlite3_step(querystmt) == SQLITE_ROW)
          {
             printf("%s\n", sqlite3_column_text(querystmt, 1));
          }
          sqlite3_finalize(querystmt);
       }
    } else {
       if (argc == 2) {
          char *query = argv[1];
          if (query[0] == '\0' || query[0] == ' ') {
              return 1;
          }
          char *query_for_notes = "SELECT rank, highlight(zettelkasten, 0, '\x1b[0;41m', '\x1b[0m') AS body"
             " FROM zettelkasten WHERE zettelkasten MATCH @query ORDER BY rank;";
          sqlite3_stmt *querystmt;
          rc = sqlite3_prepare_v2(db, query_for_notes, -1, &querystmt, NULL);

          if (rc == SQLITE_OK) {
             sqlite3_bind_text(querystmt, sqlite3_bind_parameter_index(querystmt, "@query"), argv[1], -1, NULL);
          } else {
             fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
          }

          while(sqlite3_step(querystmt) == SQLITE_ROW)
          {
             printf("%s\n", sqlite3_column_text(querystmt, 1));
          }

          sqlite3_finalize(querystmt);
       } else {
          char *get_all_notes = "SELECT title FROM zettelkasten;";
          sqlite3_stmt *allstmt;
          rc = sqlite3_prepare_v2(db, get_all_notes, -1, &allstmt, NULL);

          if (rc != SQLITE_OK) {
             fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
          }

          while(sqlite3_step(allstmt) == SQLITE_ROW)
          {
             printf("%s\n", sqlite3_column_text(allstmt, 0));
          }

          sqlite3_finalize(allstmt);
       }
    }

    sqlite3_close(db);
    return 0;
}
