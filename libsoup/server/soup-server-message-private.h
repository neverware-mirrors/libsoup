/*
 * Copyright (C) 2020 Igalia S.L.
 */

#ifndef __SOUP_SERVER_MESSAGE_PRIVATE_H__
#define __SOUP_SERVER_MESSAGE_PRIVATE_H__ 1

#include "soup-server-message.h"
#include "soup-auth-domain.h"
#include "soup-message-io-data.h"
#include "soup-socket.h"

SoupServerMessage *soup_server_message_new                 (SoupSocket               *sock);
void               soup_server_message_set_uri             (SoupServerMessage        *msg,
                                                            SoupURI                  *uri);
void               soup_server_message_set_method          (SoupServerMessage        *msg,
                                                            const char               *method);
SoupSocket        *soup_server_message_get_soup_socket     (SoupServerMessage        *msg);
void               soup_server_message_set_auth            (SoupServerMessage        *msg,
                                                            SoupAuthDomain           *domain,
                                                            char                     *user);
gboolean           soup_server_message_is_keepalive        (SoupServerMessage        *msg);
GIOStream         *soup_server_message_io_steal            (SoupServerMessage        *msg);
void               soup_server_message_io_pause            (SoupServerMessage        *msg);
void               soup_server_message_io_unpause          (SoupServerMessage        *msg);
gboolean           soup_server_message_is_io_paused        (SoupServerMessage        *msg);
void               soup_server_message_io_finished         (SoupServerMessage        *msg);
void               soup_server_message_cleanup_response    (SoupServerMessage        *msg);
void               soup_server_message_wrote_informational (SoupServerMessage        *msg);
void               soup_server_message_wrote_headers       (SoupServerMessage        *msg);
void               soup_server_message_wrote_chunk         (SoupServerMessage        *msg);
void               soup_server_message_wrote_body_data     (SoupServerMessage        *msg,
                                                            gsize                     chunk_size);
void               soup_server_message_wrote_body          (SoupServerMessage        *msg);
void               soup_server_message_got_headers         (SoupServerMessage        *msg);
void               soup_server_message_got_chunk           (SoupServerMessage        *msg,
                                                            GBytes                   *chunk);
void               soup_server_message_got_body            (SoupServerMessage        *msg);
void               soup_server_message_finished            (SoupServerMessage        *msg);
void               soup_server_message_read_request        (SoupServerMessage        *msg,
                                                            SoupMessageIOCompletionFn completion_cb,
                                                            gpointer                  user_data);

typedef struct _SoupServerMessageIOData SoupServerMessageIOData;
void                     soup_server_message_io_data_free  (SoupServerMessageIOData *io);
void                     soup_server_message_set_io_data   (SoupServerMessage        *msg,
                                                            SoupServerMessageIOData  *io);
SoupServerMessageIOData *soup_server_message_get_io_data   (SoupServerMessage        *msg);

#endif /* __SOUP_SERVER_MESSAGE_PRIVATE_H__ */
