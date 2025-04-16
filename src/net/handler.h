#ifndef SRC_NET_HANDLER_H
#define SRC_NET_HANDLER_H

#include "../database/database.h"

[[gnu::regcall, gnu::nonnull(2)]]
bool handle_request(int sock_fd, db_conn *NONNULL db);

#endif // SRC_NET_HANDLER_H
