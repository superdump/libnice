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

#include "tcp-established.h"
#include "agent-priv.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>

#ifndef G_OS_WIN32
#include <unistd.h>
#endif

#define MAX_BUFFER_SIZE 65535

typedef struct {
  NiceAddress         remote_addr;
  GQueue              send_queue;
  GMainContext       *context;
  GSource            *read_source;
  GSource            *write_source;
  gboolean            error;
  SocketRXCallback    rxcb;
  SocketTXCallback    txcb;
  gpointer            userdata;
  GDestroyNotify      destroy_notify;
  guint8              recv_buff[MAX_BUFFER_SIZE];
  guint               recv_offset;
  gboolean            connect_pending;
  guint               max_tcp_queue_size;
  gint                tx_queue_size_bytes;
} TcpEstablishedPriv;

struct to_be_sent {
  guint length;
  gchar *buf;
};

static void socket_attach (NiceSocket* sock, GMainContext* ctx);
static void socket_close (NiceSocket *sock);
static gint socket_recv (NiceSocket *sock, NiceAddress *from,
    guint len, gchar *buf);
static gint socket_send (NiceSocket *sock, const NiceAddress *to,
    guint len, const gchar *buf);
static gboolean socket_is_reliable (NiceSocket *sock);


static void add_to_be_sent (NiceSocket *sock, const gchar *buf, guint len, gboolean add_to_head);
static void free_to_be_sent (struct to_be_sent *tbs);
static gboolean socket_send_more (GSocket *gsocket, GIOCondition condition,
                                  gpointer data);
static gboolean socket_recv_more (GSocket *gsocket, GIOCondition condition,
                                  gpointer data);
static gint socket_get_tx_queue_size (NiceSocket *sock);

NiceSocket *
nice_tcp_established_socket_new (GSocket *gsock, NiceAddress *local_addr,
    const NiceAddress *remote_addr, GMainContext *ctx,
    SocketRXCallback rxcb, SocketTXCallback txcb, gpointer userdata,
    GDestroyNotify destroy_notify, gboolean connect_pending, guint max_tcp_queue_size)
{
  NiceSocket *sock;
  TcpEstablishedPriv *priv;

  g_return_val_if_fail (G_IS_SOCKET (gsock), NULL);
  g_return_val_if_fail (rxcb != NULL, NULL);
  g_return_val_if_fail (txcb != NULL, NULL);

  sock = g_slice_new0 (NiceSocket);
  sock->priv = priv = g_slice_new0 (TcpEstablishedPriv);

  priv->context = g_main_context_ref (ctx);
  priv->remote_addr = *remote_addr;
  priv->rxcb = rxcb;
  priv->txcb = txcb;
  priv->userdata = userdata;
  priv->destroy_notify = destroy_notify;
  priv->recv_offset = 0;
  priv->connect_pending = connect_pending;
  priv->max_tcp_queue_size = max_tcp_queue_size;

  sock->type = NICE_SOCKET_TYPE_TCP_ESTABLISHED;
  sock->fileno = gsock;
  sock->addr = *local_addr;
  sock->send = socket_send;
  sock->recv = socket_recv;
  sock->is_reliable = socket_is_reliable;
  sock->close = socket_close;
  sock->attach = socket_attach;
  sock->get_tx_queue_size = socket_get_tx_queue_size;

  if (max_tcp_queue_size > 0) {
    /*
     * Reduce the tx queue size so the minimum number of packets
     * are queued in the kernel
     */
    gint fd = g_socket_get_fd (gsock);
    gint sendbuff = 2048;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendbuff, sizeof (gint));
  }

  priv->read_source = g_socket_create_source(sock->fileno, G_IO_IN | G_IO_ERR, NULL);
  g_source_set_callback (priv->read_source, (GSourceFunc) socket_recv_more, sock, NULL);
  g_source_attach (priv->read_source, priv->context);
  return sock;
}

static void
socket_attach (NiceSocket* sock, GMainContext* ctx)
{
  TcpEstablishedPriv *priv = sock->priv;
  gboolean write_pending = FALSE;

  if (priv->context)
    g_main_context_unref (priv->context);

  if (priv->read_source) {
    g_source_destroy (priv->read_source);
    g_source_unref (priv->read_source);
  }

  if (priv->write_source) {
    write_pending = TRUE;
    g_source_destroy (priv->write_source);
    g_source_unref (priv->write_source);
  }

  priv->context = ctx;
  if (priv->context) {
    g_main_context_ref (priv->context);

    priv->read_source = g_socket_create_source(sock->fileno, G_IO_IN | G_IO_ERR, NULL);
    g_source_set_callback (priv->read_source, (GSourceFunc) socket_recv_more, sock, NULL);
    g_source_attach (priv->read_source, priv->context);
    if (write_pending) {
        priv->write_source = g_socket_create_source(sock->fileno, G_IO_OUT, NULL);
        g_source_set_callback (priv->write_source, (GSourceFunc) socket_send_more,
                               sock, NULL);
        g_source_attach (priv->write_source, priv->context);
    }
  }
}

static void
socket_close (NiceSocket *sock)
{
  TcpEstablishedPriv *priv = sock->priv;

  if (sock->fileno) {
    g_socket_close (sock->fileno, NULL);
    g_object_unref (sock->fileno);
    sock->fileno = NULL;
  }
  if (priv->read_source) {
    g_source_destroy (priv->read_source);
    g_source_unref (priv->read_source);
  }
  if (priv->write_source) {
    g_source_destroy (priv->write_source);
    g_source_unref (priv->write_source);
  }
  g_queue_foreach (&priv->send_queue, (GFunc) free_to_be_sent, NULL);
  g_queue_clear (&priv->send_queue);

  if (priv->userdata && priv->destroy_notify)
    (priv->destroy_notify)(priv->userdata);

  if (priv->context)
    g_main_context_unref (priv->context);

  g_slice_free(TcpEstablishedPriv, sock->priv);
}

static gint
socket_recv (NiceSocket *sock, NiceAddress *from, guint len, gchar *buf)
{
  TcpEstablishedPriv *priv = sock->priv;
  int ret;
  GError *gerr = NULL;

  /* Don't try to access the socket if it had an error */
  if (priv->error)
    return -1;

  ret = g_socket_receive (sock->fileno, buf, len, NULL, &gerr);

  /* recv returns 0 when the peer performed a shutdown.. we must return -1 here
   * so that the agent destroys the g_source */
  if (ret == 0) {
    priv->error = TRUE;
    return -1;
  }

  if (ret < 0) {
    if(g_error_matches (gerr, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
      ret = 0;

    g_error_free (gerr);
    return ret;
  }

  if (from)
    *from = priv->remote_addr;
  return ret;
}

static gint
socket_send (NiceSocket *sock, const NiceAddress *to,
    guint len, const gchar *buf)
{
  TcpEstablishedPriv *priv = sock->priv;
  int ret;
  GError *gerr = NULL;
  gchar buff[MAX_BUFFER_SIZE];

  gchar to_string[NICE_ADDRESS_STRING_LEN];

  nice_address_to_string (to, to_string);

  if (nice_address_equal (to, &priv->remote_addr)) {
    nice_debug("tcp-est %p: Sending on tcp-established to %s:%u len=%d", sock, to_string, nice_address_get_port (to), len);
    
    /* Don't try to access the socket if it had an error, otherwise we risk a
       crash with SIGPIPE (Broken pipe) */
    if (priv->error)
      return -1;
    
    buff[0] = (len >> 8);
    buff[1] = (len & 0xFF);
    memcpy (&buff[2], buf, len);
    len += 2;

    /* First try to send the data, don't send it later if it can be sent now
       this way we avoid allocating memory on every send */
    if (g_socket_is_connected (sock->fileno) &&
        g_queue_is_empty (&priv->send_queue)) {
      ret = g_socket_send (sock->fileno, buff, len, NULL, &gerr);
      if (ret < 0) {
        if (g_error_matches (gerr, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
          add_to_be_sent (sock, buff, len, FALSE);
          priv->txcb (sock, buff, len, priv->tx_queue_size_bytes, priv->userdata);
          ret = len;
        }
      } else {
        guint rest = len - ret;
        if (rest > 0) {
          add_to_be_sent (sock, &buff[ret], rest, FALSE);
          ret = len;
        }
      }

      if (gerr != NULL)
        g_error_free (gerr);

      return ret;
    } else {
      nice_debug ("tcp-est %p: not connected to %s:%u, queueing. queue-size=%u, queue_is_empty=%d max-tcp-queue-size=%d", sock, to_string, 
                  nice_address_get_port (to), priv->send_queue.length, g_queue_is_empty (&priv->send_queue), priv->max_tcp_queue_size);
      add_to_be_sent (sock, buff, len, FALSE);
      if (g_socket_is_connected (sock->fileno)) {
        priv->txcb (sock, buff, len, priv->tx_queue_size_bytes, priv->userdata);
      }
      return len;
    }
  } else {
    gchar remote_string [NICE_ADDRESS_STRING_LEN];

    nice_address_to_string (&priv->remote_addr, remote_string);
    nice_debug ("tcp-est %p: not for us to send to=%s:%u (priv->remote_addr = %s:%u)", sock, to_string, nice_address_get_port (to),
                remote_string, nice_address_get_port (&priv->remote_addr));
    return 0;
  }
}

static gboolean
socket_is_reliable (NiceSocket *sock)
{
  return TRUE;
}

static void
parse_rfc4571(NiceSocket* sock, NiceAddress* from)
{
  TcpEstablishedPriv *priv = sock->priv;
  gboolean done = FALSE;

  while (!done) {
    if (priv->recv_offset > 2) {
      guint8 *data = priv->recv_buff;
      guint packet_length = data[0] << 8 | data[1];
      nice_debug ("tcp-est %p: socket_recv_more: expecting %u bytes\n", sock, packet_length);

      if (packet_length + 2 <= priv->recv_offset) {
        nice_debug ("tcp-est %p: socket_recv_more: received %d bytes, delivering", sock, packet_length);
        priv->rxcb (sock, from, (gchar *)&data[2], packet_length, priv->userdata);

        /* More data after current packet */
        memmove (&priv->recv_buff[0], &priv->recv_buff[packet_length + 2],
                priv->recv_offset - packet_length - 2);
        priv->recv_offset = priv->recv_offset - packet_length - 2;
      } else {
        done = TRUE;
      }
    } else {
      done = TRUE;
    }
  }
}

/*
 * Returns FALSE if the source should be destroyed.
 */
static gboolean
socket_recv_more (
  GSocket *gsocket,
  GIOCondition condition,
  gpointer data)
{
  gint len;
  NiceSocket* sock = (NiceSocket *)data;
  TcpEstablishedPriv *priv = NULL;
  NiceAddress from;
  
  agent_lock();

  if (g_source_is_destroyed (g_main_current_source ())) {
    nice_debug ("tcp-est %p: Source was destroyed. "
                "Avoided race condition in tcp-established.c:socket_recv_more", sock);
    agent_unlock();
    return FALSE;
  }
  
  priv = sock->priv;

  len = socket_recv (sock, &from, MAX_BUFFER_SIZE-priv->recv_offset, (gchar *)&priv->recv_buff[priv->recv_offset]);
  if (len > 0) {
    priv->recv_offset += len;
    parse_rfc4571(sock, &from);
  } else if (len < 0) {
      nice_debug("tcp-est %p: socket_recv_more: error from socket %d", sock, len);
    g_source_destroy (priv->read_source);
    g_source_unref (priv->read_source);
    priv->read_source = NULL;
    priv->error = TRUE;
    agent_unlock();
    return FALSE;
  }

  agent_unlock();
  return TRUE;
}

/*
 * Returns FALSE if the source should be destroyed.
 */
static gboolean
socket_send_more (
  GSocket *gsocket,
  GIOCondition condition,
  gpointer data)
{
  NiceSocket *sock = (NiceSocket *) data;
  TcpEstablishedPriv *priv = sock->priv;
  struct to_be_sent *tbs = NULL;
  GError *gerr = NULL;

  nice_debug("tcp-est %p: socket_send_more, condition=%u", sock, condition);

  agent_lock();

  if (g_source_is_destroyed (g_main_current_source ())) {
    nice_debug ("tcp-est %p: Source was destroyed. "
                "Avoided race condition in tcp-established.c:socket_send_more", sock);
    agent_unlock();
    return FALSE;
  }

  if (priv->connect_pending) {
    /* 
     * First event will be the connect result
     */
    if (!g_socket_check_connect_result (gsocket, &gerr)) {
        nice_debug("tcp-est %p: connect failed. g_socket_is_connected=%d", sock, g_socket_is_connected (sock->fileno));
    } else {
        nice_debug("tcp-est %p: connect completed. g_socket_is_connected=%d", sock, g_socket_is_connected (sock->fileno));
    }
    if (gerr) {
      g_error_free (gerr);
      gerr = NULL;
    }
    priv->connect_pending = FALSE;
  }

  while ((tbs = g_queue_pop_head (&priv->send_queue)) != NULL) {
    int ret;
    
    priv->tx_queue_size_bytes -= tbs->length;

    if(condition & G_IO_HUP) {
      /* connection hangs up */
      ret = -1;
      nice_debug ("tcp-est %p: socket_send_more: got G_IO_HUP signal", sock);
    } else {
      ret = g_socket_send (sock->fileno, tbs->buf, tbs->length, NULL, &gerr);
      nice_debug ("tcp-est %p: socket_send_more: tried to send %u bytes, ret=%u", sock, tbs->length, ret);
    }

    if (ret < 0) {
      if(gerr != NULL &&
          g_error_matches (gerr, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
        add_to_be_sent (sock, tbs->buf, tbs->length, TRUE);
        g_free (tbs->buf);
        g_slice_free (struct to_be_sent, tbs);
        g_error_free (gerr);
        break;
      }
      if (gerr) {
        g_error_free (gerr);
        gerr = NULL;
      }
    } else {
      guint rest = tbs->length - ret;
      if (rest > 0) {
        add_to_be_sent (sock, &tbs->buf[ret], rest, TRUE);
        g_free (tbs->buf);
        g_slice_free (struct to_be_sent, tbs);
        break;
      }
    }

    g_free (tbs->buf);
    g_slice_free (struct to_be_sent, tbs);
  }

  if (g_queue_is_empty (&priv->send_queue)) {
    nice_debug ("tcp-est %p: socket_send_more: queue empty, releasing source", sock);
    g_source_destroy (priv->write_source);
    g_source_unref (priv->write_source);
    priv->write_source = NULL;
    priv->txcb (sock, NULL, 0, 0, priv->userdata);
    agent_unlock();
    return FALSE;
  }

  agent_unlock();
  return TRUE;
}

static void
add_to_be_sent (NiceSocket *sock, const gchar *buf, guint len, gboolean add_to_head)
{
  TcpEstablishedPriv *priv = sock->priv;
  struct to_be_sent *tbs = NULL;

  if (len <= 0)
    return;

  agent_lock();

  /*
   * Check for queue overflow, we'll allow upto priv->max_tcp_queue_size+1 elements
   * on the queue
   */
  if (!add_to_head && priv->max_tcp_queue_size != 0) {
    while (g_queue_get_length (&priv->send_queue) > priv->max_tcp_queue_size) {
      
      /*
       * We want to discard the oldest queued data which is at the front of the queue.
       * However we need to be careful as the first element on the queue may be partially
       * transmitted already, we'll discard the second element on the list instead
       */
      struct to_be_sent *pkt;
      
      nice_debug ("tcp-est %p: TCP queue size breached, discarding", sock);
      pkt = g_queue_pop_nth (&priv->send_queue, 1);
      priv->tx_queue_size_bytes -= pkt->length;
      free_to_be_sent (pkt);
    }
  }

  tbs = g_slice_new0 (struct to_be_sent);
  tbs->buf = g_memdup (buf, len);
  tbs->length = len;

  if (add_to_head) {
    g_queue_push_head (&priv->send_queue, tbs);
  } else {
    g_queue_push_tail (&priv->send_queue, tbs);
  }
  priv->tx_queue_size_bytes += tbs->length;

  if (priv->write_source == NULL) {
    priv->write_source = g_socket_create_source(sock->fileno, G_IO_OUT, NULL);
    g_source_set_callback (priv->write_source, (GSourceFunc) socket_send_more,
                           sock, NULL);
    g_source_attach (priv->write_source, priv->context);
  }
  agent_unlock();
}

static void
free_to_be_sent (struct to_be_sent *tbs)
{
  g_free (tbs->buf);
  g_slice_free (struct to_be_sent, tbs);
}

static gint
socket_get_tx_queue_size (NiceSocket *sock)
{
  TcpEstablishedPriv *priv = sock->priv;

  return priv->tx_queue_size_bytes;
}
