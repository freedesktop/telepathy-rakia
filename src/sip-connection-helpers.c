/*
 * sip-connection-helpers.c - Helper routines used by RakiaConnection
 * Copyright (C) 2005 Collabora Ltd.
 * Copyright (C) 2006-2009 Nokia Corporation
 *
 * This work is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This work is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this work; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <telepathy-glib/telepathy-glib.h>

#include <rakia/util.h>
#include <rakia/handles.h>

#include "sip-connection-helpers.h"

#include <sofia-sip/sip.h>
#include <sofia-sip/sip_header.h>
#include <sofia-sip/tport_tag.h>

#include "sip-connection-private.h"

#define DEBUG_FLAG RAKIA_DEBUG_CONNECTION
#include "rakia/debug.h"

/* Default keepalive timeout in seconds,
 * a value obtained from Sofia-SIP documentation */
#define RAKIA_CONNECTION_DEFAULT_KEEPALIVE_INTERVAL 120

/* The user is not allowed to set keepalive timeout to lower than that,
 * to avoid wasting traffic and device power */
#define RAKIA_CONNECTION_MINIMUM_KEEPALIVE_INTERVAL 30

/* The user is not allowed to set keepalive timeout to lower than that
 * for REGISTER keepalives, to avoid wasting traffic and device power.
 * REGISTER is special because it may tie resources on the server side */
#define RAKIA_CONNECTION_MINIMUM_KEEPALIVE_INTERVAL_REGISTER 50

static sip_to_t *
priv_sip_to_url_make (RakiaConnection *conn,
                      su_home_t *home,
                      TpHandle contact)
{
  const url_t *url;

  url = rakia_base_connection_handle_to_uri (
      (RakiaBaseConnection *) conn, contact);
  return sip_to_create (home, (const url_string_t *) url);
}

static sip_from_t *
priv_sip_from_url_make (RakiaConnection *conn,
                        su_home_t *home)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  sip_from_t *from;
  gchar *alias = NULL;

  from = sip_from_create (home, (const url_string_t *) priv->account_url);

  if (from == NULL)
    return NULL;

  g_object_get (conn, "alias", &alias, NULL);
  if (alias != NULL)
    {
      gchar *alias_quoted;

      /* Make the alias into a quoted string, escaping all characters
       * that cannot go verbatim into a quoted string */

      alias_quoted = rakia_quote_string (alias);

      g_free (alias);

      from->a_display = su_strdup (home, alias_quoted);

      g_free (alias_quoted);
    }

  return from;
}


nua_handle_t *
rakia_conn_create_register_handle (RakiaConnection *conn,
                                 TpHandle contact)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  nua_handle_t *result = NULL;
  su_home_t temphome[1] = { SU_HOME_INIT(temphome) };
  sip_to_t *to;

  g_assert (priv->sofia_home != NULL);
  g_assert (priv->sofia_nua != NULL);

  to = priv_sip_to_url_make (conn, temphome, contact);

  if (to)
      result = nua_handle (priv->sofia_nua, NULL, SIPTAG_TO(to), TAG_END());

  su_home_deinit (temphome);

  return result;
}

nua_handle_t *
rakia_conn_create_request_handle (RakiaConnection *conn,
                                  TpHandle contact)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  nua_handle_t *result = NULL;
  su_home_t temphome[1] = { SU_HOME_INIT(temphome) };
  sip_from_t *from;
  sip_to_t *to;

  g_assert (priv->sofia_home != NULL);
  g_assert (priv->sofia_nua != NULL);

  to = priv_sip_to_url_make (conn, temphome, contact);
  from = priv_sip_from_url_make (conn, temphome);

  if (to != NULL && from != NULL)
    result = nua_handle (priv->sofia_nua, NULL,
                         NUTAG_URL(to->a_url),
                         SIPTAG_TO(to),
                         SIPTAG_FROM(from),
                         TAG_END());

  su_home_deinit (temphome);

  return result;
}

void
rakia_conn_update_proxy_and_transport (RakiaConnection *conn)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);

  if (priv->proxy_url != NULL)
    {
      su_home_t temphome[1] = { SU_HOME_INIT(temphome) };
      sip_route_t *route = NULL;
      const char *params = NULL;

      if (priv->loose_routing)
        {
          url_t *route_url;
          route_url = url_hdup (temphome, priv->proxy_url);
          g_return_if_fail (route_url != NULL);
          if (!url_has_param (route_url, "lr"))
            url_param_add (temphome, route_url, "lr");
          route = sip_route_create (temphome, route_url, NULL);
        }

      if (priv->transport != NULL && priv->proxy_url->url_type == url_sip)
        {
          if (g_ascii_strcasecmp (priv->transport, "tcp") == 0)
            params = "transport=tcp";
          else if (g_ascii_strcasecmp (priv->transport, "udp") == 0)
            params = "transport=udp";
          else
            WARNING ("unrecognized transport parameter value: %s", priv->transport);
        }

      nua_set_params (priv->sofia_nua,
                      TAG_IF(route, NUTAG_INITIAL_ROUTE(route)),
                      TAG_IF(!priv->loose_routing,
                             NUTAG_PROXY(priv->proxy_url)),
                      TAG_IF(params, NUTAG_M_PARAMS(params)),
                      TAG_NULL());

      su_home_deinit (temphome);
    }
}

const url_t *
rakia_conn_get_local_url (RakiaConnection *conn)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  url_t *url;

  url = url_make (priv->sofia_home, "sip:*:*");

  if (url == NULL)
    return NULL;

  if (priv->proxy_url != NULL)
    {
      url->url_type = priv->proxy_url->url_type;
    }
  else
    {
      g_assert (priv->account_url != NULL);
      url->url_type = priv->account_url->url_type;
    }

  if (priv->local_ip_address == NULL)
    url->url_host = "0";
  else
    url->url_host = priv->local_ip_address;

  if (priv->local_port == 0)
    url->url_port = "*";
  else
    url->url_port = su_sprintf (priv->sofia_home, "%u", priv->local_port);

  if (url->url_type == url_sip && priv->transport != NULL)
    {
      if (!g_ascii_strcasecmp(priv->transport, "udp"))
        url->url_params = "transport=udp";
      else if (!g_ascii_strcasecmp(priv->transport, "tcp"))
        url->url_params = "transport=tcp";
    }

  /* url_sanitize (url); -- we're always sane B-] */

  DEBUG("local binding expressed as <" URL_PRINT_FORMAT ">", URL_PRINT_ARGS(url));

  return url;
}

static GHashTable*
priv_nua_get_outbound_options (nua_t* nua)
{
  const char* outbound = NULL;
  GHashTable* option_table;
  gchar** options;
  gchar* token;
  gboolean value;
  int i;

  option_table = g_hash_table_new_full (g_str_hash,
                                        g_str_equal,
                                        g_free,
                                        NULL);

  nua_get_params (nua, NUTAG_OUTBOUND_REF(outbound), TAG_END());
  if (outbound == NULL)
    return option_table;

  DEBUG ("got outbound options %s", outbound);

  options = g_strsplit_set (outbound, " ", 0);

  for (i = 0; (token = options[i]) != NULL; i++)
    {
      value = TRUE;
      /* Look for the negation prefixes */
      if (g_ascii_strncasecmp (token, "no", 2) == 0)
        switch (token[2])
          {
	  case '-':
	  case '_':
	    token += 3;
	    value = FALSE;
	    break;
	  case 'n':
	  case 'N':
	    switch (token[3])
	      {
	      case '-':
	      case '_':
		token += 4;
		value = FALSE;
		break;
	      }
	    break;
          }

      g_hash_table_insert (option_table,
                           g_strdup (token),
                           GINT_TO_POINTER(value));
    }

  g_strfreev (options);

  return option_table;
}

static void
priv_nua_outbound_vectorize_walk (gpointer key,
                                  gpointer value,
                                  gpointer user_data)
{
  gchar ***pstrv = (gchar ***)user_data;
  const gchar *option = (const gchar *)key;
  *(*pstrv)++ = (value)? g_strdup (option) : g_strdup_printf ("no-%s", option);
}

static void
priv_nua_set_outbound_options (nua_t* nua, GHashTable* option_table)
{
  gchar* outbound;
  gchar** options;
  gchar** walker;

  /* construct the option string array */
  options = g_new(gchar*, g_hash_table_size (option_table) + 1);

  /* fill the array with option tokens and terminate with NULL */
  walker = options;
  g_hash_table_foreach (option_table, priv_nua_outbound_vectorize_walk, &walker);
  *walker = NULL;

  /* concatenate all tokens into a string */
  outbound = g_strjoinv (" ", options);

  g_strfreev (options);

  g_assert (outbound != NULL);

  /* deliver the option string to the stack */
  DEBUG ("setting outbound options %s", outbound);
  nua_set_params (nua, NUTAG_OUTBOUND(outbound), TAG_NULL());

  g_free (outbound);
}

void
rakia_conn_update_nua_outbound (RakiaConnection *conn)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  GHashTable *option_table;

  g_return_if_fail (priv->sofia_nua != NULL);

  option_table = priv_nua_get_outbound_options (priv->sofia_nua);

  /* Purge existing occurrences of the affected options */
  g_hash_table_remove (option_table, "options-keepalive");

  /* Set options that affect keepalive behavior */
  switch (priv->keepalive_mechanism)
    {
    case RAKIA_CONNECTION_KEEPALIVE_NONE:
    case RAKIA_CONNECTION_KEEPALIVE_REGISTER:
      /* For REGISTER keepalives, we use NUTAG_M_FEATURES */
      g_hash_table_insert (option_table,
                           g_strdup ("options-keepalive"),
                           GINT_TO_POINTER(FALSE));
      break;
    case RAKIA_CONNECTION_KEEPALIVE_OPTIONS:
      g_hash_table_insert (option_table,
                           g_strdup ("options-keepalive"),
                           GINT_TO_POINTER(TRUE));
      break;
    case RAKIA_CONNECTION_KEEPALIVE_STUN:
      /* Not supported */
      break;
    case RAKIA_CONNECTION_KEEPALIVE_AUTO:
    default:
      break;
    }

  g_hash_table_insert (option_table,
                       g_strdup ("natify"),
                       GINT_TO_POINTER(priv->discover_binding));
  g_hash_table_insert (option_table,
                       g_strdup ("use-rport"),
                       GINT_TO_POINTER(priv->discover_binding));

  /* Hand options back to the NUA */

  priv_nua_set_outbound_options (priv->sofia_nua, option_table);

  g_hash_table_unref (option_table);
}

static void
priv_sanitize_keepalive_interval (RakiaConnectionPrivate *priv)
{
  guint minimum_interval;
  if (priv->keepalive_interval != 0)
    {
      minimum_interval =
              (priv->keepalive_mechanism == RAKIA_CONNECTION_KEEPALIVE_REGISTER)
              ? RAKIA_CONNECTION_MINIMUM_KEEPALIVE_INTERVAL_REGISTER
              : RAKIA_CONNECTION_MINIMUM_KEEPALIVE_INTERVAL;
      if (priv->keepalive_interval < minimum_interval)
        {
          WARNING ("keepalive interval is too low, pushing to %u", minimum_interval);
          priv->keepalive_interval = minimum_interval;
        }
    }
}

void
rakia_conn_update_nua_keepalive_interval (RakiaConnection *conn)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  long keepalive_interval;

  if (!priv->keepalive_interval_specified)
    return;

  if (priv->keepalive_mechanism == RAKIA_CONNECTION_KEEPALIVE_NONE)
    keepalive_interval = 0;
  else
    {
      priv_sanitize_keepalive_interval (priv);
      keepalive_interval = (long) priv->keepalive_interval;
    }
  keepalive_interval *= 1000;

  DEBUG("setting keepalive interval to %ld msec", keepalive_interval);

  nua_set_params (priv->sofia_nua,
                  NUTAG_KEEPALIVE(keepalive_interval),
                  TPTAG_KEEPALIVE(keepalive_interval),
                  TAG_NULL());
}

void
rakia_conn_update_nua_contact_features (RakiaConnection *conn)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  char *contact_features;
  guint timeout;

  if (priv->keepalive_mechanism != RAKIA_CONNECTION_KEEPALIVE_REGISTER)
    return;

  if (priv->keepalive_interval == 0)
    return;

  priv_sanitize_keepalive_interval (priv);
  timeout = priv->keepalive_interval_specified
      ? priv->keepalive_interval
      : RAKIA_CONNECTION_DEFAULT_KEEPALIVE_INTERVAL;
  contact_features = g_strdup_printf ("expires=%u", timeout);
  nua_set_params(priv->sofia_nua,
		 NUTAG_M_FEATURES(contact_features),
		 TAG_NULL());
  g_free (contact_features);
}

static void
rakia_conn_set_stun_server_address (RakiaConnection *conn, const gchar *address)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  g_return_if_fail (priv->media_manager != NULL);
  g_object_set (priv->media_manager,
                "stun-server", address,
                "stun-port", priv->stun_port,
                NULL);
}

static void
priv_stun_resolver_cb (sres_context_t *ctx, sres_query_t *query, sres_record_t **answers)
{
  RakiaConnection *conn = RAKIA_CONNECTION (ctx);
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  sres_a_record_t *ans = NULL;

  if (NULL != answers)
    {
      int i;
      GPtrArray *items = g_ptr_array_sized_new (1);

      for (i = 0; NULL != answers[i]; i++)
        {
          if ((0 == answers[i]->sr_record->r_status)
              && (sres_type_a == answers[i]->sr_record->r_type))
            {
              g_ptr_array_add (items, answers[i]->sr_a);
            }
        }

      if (items->len > 0)
        {
          ans = g_ptr_array_index (items, g_random_int_range (0, items->len));
        }

      g_ptr_array_unref (items);
    }

  if (NULL != ans)
    rakia_conn_set_stun_server_address (conn,
                                        inet_ntoa (ans->a_addr));
  else
    MESSAGE ("could not resolve STUN server address, ignoring");

  sres_free_answers (priv->sofia_resolver, answers);
}

void
rakia_conn_resolv_stun_server (RakiaConnection *conn, const gchar *stun_host)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  struct in_addr test_addr;

  if (stun_host == NULL)
    {
      rakia_conn_set_stun_server_address (conn, NULL);
      return;
    }

  /* FIXME: support IPv6 as well */
  if (inet_aton (stun_host, &test_addr))
    {
      rakia_conn_set_stun_server_address (conn, stun_host);
      return;
    }

  if (NULL == priv->sofia_resolver)
    {
      su_root_t *root = NULL;

      g_object_get (conn, "sofia-root", &root, NULL);

      priv->sofia_resolver = sres_resolver_create (root, NULL, TAG_END());

      g_return_if_fail (priv->sofia_resolver != NULL);
    }

  DEBUG("creating a new resolver query for STUN host name %s", stun_host);

  sres_query (priv->sofia_resolver,
              priv_stun_resolver_cb,
              (sres_context_t *) conn,
              sres_type_a,
              stun_host);
}

static void
priv_stun_discover_cb (sres_context_t *ctx,
                       sres_query_t *query,
                       sres_record_t **answers)
{
  RakiaConnection *conn = RAKIA_CONNECTION (ctx);
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  sres_srv_record_t *sel = NULL;
  guint n_sel_items = 0;
  guint i;

  if (answers == NULL)
    return;

  for (i = 0; NULL != answers[i]; i++)
    {
      if (answers[i]->sr_record->r_status != 0)
        continue;
      if (G_UNLIKELY (answers[i]->sr_record->r_type != sres_type_srv))
        continue;

      if (sel == NULL
          || (answers[i]->sr_srv->srv_priority < sel->srv_priority))
        {
          sel = answers[i]->sr_srv;
          n_sel_items = 1;
        }
      else if (answers[i]->sr_srv->srv_priority == sel->srv_priority)
        {
          n_sel_items++;
        }
    }

  if (n_sel_items > 1)
    {
      /* Random selection procedure as recommended in RFC 2782 */
      GArray *items = g_array_sized_new (FALSE,
                                         TRUE,
                                         sizeof (sres_srv_record_t *),
                                         n_sel_items);
      int sum = 0;
      int dice;
      sres_srv_record_t *rec;

      g_assert (sel != NULL);

      for (i = 0; NULL != answers[i]; i++)
        {
          if (answers[i]->sr_record->r_status != 0)
            continue;
          if (G_UNLIKELY (answers[i]->sr_record->r_type != sres_type_srv))
            continue;

          rec = answers[i]->sr_srv; 
          if (rec->srv_priority != sel->srv_priority)
            continue;

          if (rec->srv_weight == 0)
            g_array_prepend_val (items, rec);
          else
            g_array_append_val (items, rec);
        }

      g_assert (n_sel_items == items->len);

      for (i = 0; i < n_sel_items; i++)
        {
          rec = g_array_index (items, sres_srv_record_t *, i);
          sum = (rec->srv_weight += sum);
        }

      dice = g_random_int_range (0, sum + 1);

      for (i = 0; i < n_sel_items; i++)
        {
          rec = g_array_index (items, sres_srv_record_t *, i);
          if (rec->srv_weight >= dice)
            {
              sel = rec;
              break;
            }
        }

      g_array_unref (items);
    }

  if (sel != NULL)
    {
      DEBUG ("discovery got STUN server %s:%u",
             sel->srv_target, sel->srv_port);
      priv->stun_port = sel->srv_port;
      rakia_conn_resolv_stun_server (conn, sel->srv_target);
    }

  sres_free_answers (priv->sofia_resolver, answers);
}

void
rakia_conn_discover_stun_server (RakiaConnection *conn)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);
  const char *url_host;
  char *srv_domain;
  struct in_addr test_addr;

  g_return_if_fail (priv->account_url != NULL);

  url_host = priv->account_url->url_host;

  if (url_host == NULL)
    {
      DEBUG("unknown domain, not making STUN SRV lookup");
      return;
    }

  /* FIXME: support IPv6 as well */
  if (inet_aton (url_host, &test_addr))
    {
      DEBUG("AOR URI has IP address, not making STUN SRV lookup");
      return;
    }

  if (NULL == priv->sofia_resolver)
    {
      su_root_t *root = NULL;

      g_object_get (conn, "sofia-root", &root, NULL);

      priv->sofia_resolver = sres_resolver_create (root, NULL, TAG_END());
      g_return_if_fail (priv->sofia_resolver != NULL);
    }

  DEBUG("creating a new STUN SRV query for domain %s", url_host);

  srv_domain = g_strdup_printf ("_stun._udp.%s", url_host);

  sres_query (priv->sofia_resolver,
              priv_stun_discover_cb,
              (sres_context_t *) conn,
              sres_type_srv,
              srv_domain);

  g_free (srv_domain);
}

gchar *
rakia_handle_normalize (TpHandleRepoIface *repo,
                        const gchar *sipuri,
                        gpointer context,
                        GError **error)
{
  RakiaConnection *conn = RAKIA_CONNECTION (context);
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (conn);

  return rakia_normalize_contact (sipuri, priv->account_url, priv->transport,
      error);
}

#ifdef HAVE_LIBIPHB

static gushort
recommended_intervals[] = {
    IPHB_GS_WAIT_10_HOURS,
    IPHB_GS_WAIT_2_HOURS,
    IPHB_GS_WAIT_1_HOUR,
    IPHB_GS_WAIT_30_MINS,
    IPHB_GS_WAIT_10_MINS * 2, /* It aligns with the 1 hour slot. */
    IPHB_GS_WAIT_10_MINS,
    IPHB_GS_WAIT_5_MINS,
    IPHB_GS_WAIT_2_5_MINS,
    IPHB_GS_WAIT_30_SEC};

static gushort
get_system_sync_interval (guint max_interval)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (recommended_intervals); i++)
    {
      if (recommended_intervals[i] <= max_interval)
        return recommended_intervals[i];
    }

  return (gushort) MIN (max_interval, G_MAXUSHORT);
}

static void
heartbeat_schedule_wait (RakiaConnection *self)
{
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (self);
  gushort interval;

  /* Passing the same minimum and maximum interval to iphb_wait() means
   * that the iphb daemon will wake us up when its internal time is a
   * multiple of the interval.
   * By using recommended intervals across the platform we can get
   * multiple processes waken up at the same time. */
  interval = get_system_sync_interval (priv->keepalive_interval);
  DEBUG ("requested %hd as maximum interval; using the recommended %hu "
      "interval", priv->keepalive_interval, interval);

  if (iphb_wait (priv->heartbeat, interval, interval, 0) < 0)
    {
      WARNING ("iphb_wait failed");
      rakia_conn_heartbeat_shutdown (self);
    }
}

static int
heartbeat_wakeup (su_root_magic_t *foo,
                  su_wait_t *wait,
                  void *user_data)
{
  RakiaConnection *self = (RakiaConnection *) user_data;
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (self);

  DEBUG("tick");

  g_assert (priv->heartbeat != NULL);

  if ((wait->revents & (SU_WAIT_IN | SU_WAIT_HUP | SU_WAIT_ERR)) != SU_WAIT_IN)
    {
      WARNING ("heartbeat descriptor invalidated prematurely with event mask %hd", wait->revents);
      rakia_conn_heartbeat_shutdown (self);
      return 0;
    }

  heartbeat_schedule_wait (self);

  return 0;
}

#endif /* HAVE_LIBIPHB */

void
rakia_conn_heartbeat_init (RakiaConnection *self)
{
#ifdef HAVE_LIBIPHB
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (self);
  int wait_id;
  int reference_interval = 0;
  su_root_t *root = NULL;

  g_assert (priv->heartbeat == NULL);

  priv->heartbeat = iphb_open (&reference_interval);

  if (priv->heartbeat == NULL)
    {
      WARNING ("opening IP heartbeat failed: %s", strerror (errno));
      return;
    }

  DEBUG("heartbeat opened with reference interval %d", reference_interval);

  su_wait_init (priv->heartbeat_wait);
  if (su_wait_create (priv->heartbeat_wait,
                      iphb_get_fd (priv->heartbeat),
                      SU_WAIT_IN) != 0)
    rakia_log (DEBUG_FLAG, G_LOG_LEVEL_CRITICAL,
        "could not create a wait object");

  g_object_get (self, "sofia-root", &root, NULL);

  wait_id = su_root_register (root,
      priv->heartbeat_wait, heartbeat_wakeup, self, 0);

  g_return_if_fail (wait_id > 0);
  priv->heartbeat_wait_id = wait_id;

  heartbeat_schedule_wait (self);

#endif /* HAVE_LIBIPHB */
}

void
rakia_conn_heartbeat_shutdown (RakiaConnection *self)
{
#ifdef HAVE_LIBIPHB
  RakiaConnectionPrivate *priv = RAKIA_CONNECTION_GET_PRIVATE (self);
  su_root_t *root = NULL;

  if (priv->heartbeat_wait_id == 0)
    return;

  g_object_get (self, "sofia-root", &root, NULL);

  su_root_deregister (root, priv->heartbeat_wait_id);
  priv->heartbeat_wait_id = 0;

  su_wait_destroy (priv->heartbeat_wait);

  iphb_close (priv->heartbeat);
  priv->heartbeat = NULL;
#endif /* HAVE_LIBIPHB */
}
