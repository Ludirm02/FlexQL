#ifndef FLEXQL_H
#define FLEXQL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FlexQL FlexQL;
typedef FlexQL flexql;

typedef int (*flexql_callback)(void* data, int columnCount, char** values, char** columnNames);

#define FLEXQL_OK 0
#define FLEXQL_ERROR 1
#define FLEXQL_NOMEM 2
#define FLEXQL_NETWORK_ERROR 3
#define FLEXQL_PROTOCOL_ERROR 4
#define FLEXQL_SQL_ERROR 5

int flexql_open(const char* host, int port, FlexQL** db);
int flexql_close(FlexQL* db);
int flexql_exec(FlexQL* db, const char* sql, flexql_callback callback, void* arg, char** errmsg);
void flexql_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif
