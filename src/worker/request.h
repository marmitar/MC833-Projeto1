#ifndef SRC_WORKER_REQUEST_HANDLER_H
/** Network request hander. */
#define SRC_WORKER_REQUEST_HANDLER_H

#include <stdbool.h>

#include "../database/database.h"
#include "../defines.h"

[[gnu::nonnull(2), gnu::hot]]
/**
 * Handles a single client connection on sock_fd, parsing YAML requests and calling the database functions.
 *
 * Reads a series of operations from the client socket, interprets them using parser_next_op(), and executes
 * corresponding db_* calls. Sends a simple text response back to the client  for each operation, then closes the
 * socket at the end.
 *
 * @param sock_fd The accepted socket file descriptor for this client.
 * @param db      A non-null pointer to the open database connection.
 * @return true if a hard failure occurred and the server should possibly shut down, false otherwise.
 */
bool handle_request(int sock_fd, db_conn_t *NONNULL db);

#endif  // SRC_WORKER_REQUEST_HANDLER_H
