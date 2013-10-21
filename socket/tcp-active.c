/*
 * This file is part of the Nice GLib ICE library.
 *
 * (C) 2008-2012 Collabora Ltd.
 *  Contact: Youness Alaoui
 * (C) 2008-2009 Nokia Corporation. All rights reserved.
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Nice GLib ICE library.
 *
 * The Initial Developers of the Original Code are Collabora Ltd and Nokia
 * Corporation. All Rights Reserved.
 *
 * Contributors:
 *   Youness Alaoui, Collabora Ltd.
 *   George Kiagiadakis, Collabora Ltd.
 *
 * Alternatively, the contents of this file may be used under the terms of the
 * the GNU Lesser General Public License Version 2.1 (the "LGPL"), in which
 * case the provisions of LGPL are applicable instead of those above. If you
 * wish to allow use of your version of this file only under the terms of the
 * LGPL and not to allow others to use your version of this file under the
 * MPL, indicate your decision by deleting the provisions above and replace
 * them with the notice and other provisions required by the LGPL. If you do
 * not delete the provisions above, a recipient may use your version of this
 * file under either the MPL or the LGPL.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "tcp-active.h"
#include "tcp-established.h"
#include "agent-priv.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>

#ifndef G_OS_WIN32
#include <unistd.h>
#endif

typedef struct {
  GSocketAddress *local_addr;
  GMainContext *context;
} TcpActivePriv;


static void socket_close (NiceSocket *sock);
static gint socket_recv (NiceSocket *sock, NiceAddress *from,
    guint len, gchar *buf);
static gint socket_send (NiceSocket *sock, const NiceAddress *to,
    guint len, const gchar *buf);
static gboolean socket_is_reliable (NiceSocket *sock);


NiceSocket *
nice_tcp_active_socket_new (GMainContext *ctx, NiceAddress *addr)
{
  struct sockaddr_storage name;
  NiceAddress tmp_addr;
  NiceSocket *sock;
  TcpActivePriv *priv;
  GSocketAddress *gaddr;

  if (addr == NULL) {
    /* We can't connect a tcp socket with no destination address */
    return NULL;
  }

  tmp_addr = *addr;

  /* Make sure we don't bind to any local port */
  nice_address_set_port (&tmp_addr, 0);
  nice_address_copy_to_sockaddr (&tmp_addr, (struct sockaddr *)&name);

  gaddr = g_socket_address_new_from_native (&name, sizeof (name));

  if (gaddr == NULL) {
    return NULL;
  }

  sock = g_slice_new0 (NiceSocket);

  sock->priv = priv = g_slice_new0 (TcpActivePriv);

  priv->context = g_main_context_ref (ctx);
  priv->local_addr = gaddr;

  sock->addr = tmp_addr;

  sock->type = NICE_SOCKET_TYPE_TCP_ACTIVE;
  sock->fileno = NULL;
  sock->send = socket_send;
  sock->recv = socket_recv;
  sock->is_reliable = socket_is_reliable;
  sock->close = socket_close;

  return sock;
}

static void
socket_close (NiceSocket *sock)
{
  TcpActivePriv *priv = sock->priv;

  if (priv->context)
    g_main_context_unref (priv->context);
  if (priv->local_addr)
    g_object_unref (priv->local_addr);

  g_slice_free(TcpActivePriv, sock->priv);
}

static gint
socket_recv (NiceSocket *sock, NiceAddress *from, guint len, gchar *buf)
{
  return -1;
}

static gint
socket_send (NiceSocket *sock, const NiceAddress *to,
    guint len, const gchar *buf)
{
  return -1;
}


static gboolean
socket_is_reliable (NiceSocket *sock)
{
  return TRUE;
}

NiceSocket *
nice_tcp_active_socket_connect (NiceSocket *socket, NiceAddress *addr)
{
  struct sockaddr_storage name;
  TcpActivePriv *active_priv = socket->priv;
  GSocket *gsock = NULL;
  GError *gerr = NULL;
  gboolean gret = FALSE;
  GSocketAddress *gaddr;
  NiceAddress local_addr;

  if (addr == NULL) {
    /* We can't connect a tcp socket with no destination address */
    return NULL;
  }

  nice_address_copy_to_sockaddr (addr, (struct sockaddr *)&name);

  if (name.ss_family == AF_UNSPEC || name.ss_family == AF_INET) {
    gsock = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP, NULL);

    name.ss_family = AF_INET;
#ifdef HAVE_SA_LEN
    name.ss_len = sizeof (struct sockaddr_in);
#endif
  } else if (name.ss_family == AF_INET6) {
    gsock = g_socket_new (G_SOCKET_FAMILY_IPV6, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP, NULL);
    name.ss_family = AF_INET6;
#ifdef HAVE_SA_LEN
    name.ss_len = sizeof (struct sockaddr_in6);
#endif
  }

  if (gsock == NULL) {
    return NULL;
  }

  gaddr = g_socket_address_new_from_native (&name, sizeof (name));
  if (gaddr == NULL) {
    g_object_unref (gsock);
    return NULL;
  }

  /* GSocket: All socket file descriptors are set to be close-on-exec. */
  g_socket_set_blocking (gsock, false);

  gret = g_socket_bind (gsock, active_priv->local_addr, FALSE, NULL) &&
      g_socket_connect (gsock, gaddr, NULL, &gerr);
  g_object_unref (gaddr);

  if (gret == FALSE && gerr) {
    if (g_error_matches (gerr, G_IO_ERROR, G_IO_ERROR_PENDING) == FALSE) {
      g_error_free (gerr);
      g_socket_close (gsock, NULL);
      g_object_unref (gsock);
      return NULL;
    }
    g_error_free (gerr);
  }

  gaddr = g_socket_get_local_address (gsock, NULL);
  if (gaddr == NULL ||
      !g_socket_address_to_native (gaddr, &name, sizeof (name), NULL)) {
    g_socket_close (gsock, NULL);
    g_object_unref (gsock);
    return NULL;
  }
  g_object_unref (gaddr);

  nice_address_set_from_sockaddr (&local_addr, (struct sockaddr *)&name);

  return nice_tcp_established_socket_new (gsock,
                                          &local_addr, addr, active_priv->context, NULL, NULL, NULL);
}
