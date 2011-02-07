/*
 * sip-media-stream.c - Source for TpsipMediaStream
 * Copyright (C) 2006 Collabora Ltd.
 * Copyright (C) 2006-2010 Nokia Corporation
 *   @author Kai Vehmanen <first.surname@nokia.com>
 *   @author Mikhail Zabaluev <mikhail.zabaluev@nokia.com>
 *
 * Based on telepathy-gabble implementation (gabble-media-stream).
 *   @author Ole Andre Vadla Ravnaas <ole.andre.ravnaas@collabora.co.uk>
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

#include "config.h"
#include "tpsip/media-stream.h"

#include <dbus/dbus-glib.h>
#include <stdlib.h>
#include <string.h>

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/enums.h>
#include <telepathy-glib/errors.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/svc-generic.h>
#include <telepathy-glib/svc-media-interfaces.h>
#include <telepathy-glib/util.h>

#include "config.h"

#include <tpsip/codec-param-formats.h>

#include "tpsip/media-session.h"

#include <sofia-sip/msg_parser.h>

#include "signals-marshal.h"

#define DEBUG_FLAG TPSIP_DEBUG_MEDIA
#include "tpsip/debug.h"


#define same_boolean(old, new) ((!(old)) == (!(new)))


static void stream_handler_iface_init (gpointer, gpointer);

G_DEFINE_TYPE_WITH_CODE(TpsipMediaStream,
    tpsip_media_stream,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_MEDIA_STREAM_HANDLER,
      stream_handler_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
      tp_dbus_properties_mixin_iface_init);
  )

/* signal enum */
enum
{
    SIG_READY,
    SIG_SUPPORTED_CODECS,
    SIG_STATE_CHANGED,
    SIG_DIRECTION_CHANGED,
    SIG_LOCAL_MEDIA_UPDATED,
    SIG_UNHOLD_FAILURE,

    SIG_LAST_SIGNAL
};

static guint signals[SIG_LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_MEDIA_SESSION = 1,
  PROP_DBUS_DAEMON,
  PROP_OBJECT_PATH,
  PROP_ID,
  PROP_MEDIA_TYPE,
  PROP_STATE,
  PROP_DIRECTION,
  PROP_PENDING_SEND_FLAGS,
  PROP_HOLD_STATE,
  PROP_CREATED_LOCALLY,
  PROP_NAT_TRAVERSAL,
  PROP_STUN_SERVERS,
  PROP_RELAY_INFO,
  LAST_PROPERTY
};

static GPtrArray *tpsip_media_stream_relay_info_empty = NULL;

/* private structure */
typedef struct _TpsipMediaStreamPrivate TpsipMediaStreamPrivate;

struct _TpsipMediaStreamPrivate
{
  TpDBusDaemon *dbus_daemon;
  TpsipMediaSession *session;     /* see gobj. prop. 'media-session' */
  gchar *object_path;             /* see gobj. prop. 'object-path' */
  guint id;                       /* see gobj. prop. 'id' */
  guint media_type;               /* see gobj. prop. 'media-type' */
  guint state;                    /* see gobj. prop. 'state' */
  guint direction;                /* see gobj. prop. 'direction' */
  guint pending_send_flags;       /* see gobj. prop. 'pending-send-flags' */
  gboolean hold_state;            /* see gobj. prop. 'hold-state' */
  gboolean created_locally;       /* see gobj. prop. 'created-locally' */

  gchar *stream_sdp;              /* SDP description of the stream */

  GValue native_codecs;           /* intersected codec list */
  GValue native_candidates;

  const sdp_media_t *remote_media; /* pointer to the SDP media structure
                                    *  owned by the session object */

  guint remote_candidate_counter;
  gchar *remote_candidate_id;

  gchar *native_candidate_id;

  gboolean ready_received;              /* our ready method has been called */
  gboolean playing;                     /* stream set to playing */
  gboolean sending;                     /* stream set to sending */
  gboolean pending_remote_receive;      /* TRUE if remote is to agree to receive media */
  gboolean native_cands_prepared;       /* all candidates discovered */
  gboolean native_codecs_prepared;      /* all codecs discovered */
  gboolean push_remote_cands_pending;   /* SetRemoteCandidates emission is pending */
  gboolean push_remote_codecs_pending;  /* SetRemoteCodecs emission is pending */
  gboolean codec_intersect_pending;     /* codec intersection is pending */
  gboolean requested_hold_state;        /* hold state last requested from the stream handler */
  gboolean dispose_has_run;
};

#define TPSIP_MEDIA_STREAM_GET_PRIVATE(o)     (G_TYPE_INSTANCE_GET_PRIVATE ((o), TPSIP_TYPE_MEDIA_STREAM, TpsipMediaStreamPrivate))

static void push_remote_codecs (TpsipMediaStream *stream);
static void push_remote_candidates (TpsipMediaStream *stream);
static void push_active_candidate_pair (TpsipMediaStream *stream);
static void priv_update_sending (TpsipMediaStream *stream,
                                 TpMediaStreamDirection direction);
static void priv_update_local_sdp(TpsipMediaStream *stream);
static void priv_generate_sdp (TpsipMediaStream *stream);

#if 0
#ifdef ENABLE_DEBUG
static const char *debug_tp_protocols[] = {
  "TP_MEDIA_STREAM_PROTO_UDP (0)",
  "TP_MEDIA_STREAM_PROTO_TCP (1)"
};

static const char *debug_tp_transports[] = {
  "TP_MEDIA_STREAM_TRANSPORT_TYPE_LOCAL (0)",
  "TP_MEDIA_STREAM_TRANSPORT_TYPE_DERIVED (1)",
  "TP_MEDIA_STREAM_TRANSPORT_TYPE_RELAY (2)"
};
#endif /* ENABLE_DEBUG */
#endif /* 0 */

/***********************************************************************
 * Set: Gobject interface
 ***********************************************************************/

static void
tpsip_media_stream_init (TpsipMediaStream *self)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);

  priv->playing = FALSE;
  priv->sending = FALSE;

  g_value_init (&priv->native_codecs, TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CODEC_LIST);
  g_value_take_boxed (&priv->native_codecs,
      dbus_g_type_specialized_construct (TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CODEC_LIST));

  g_value_init (&priv->native_candidates, TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE_LIST);
  g_value_take_boxed (&priv->native_candidates,
      dbus_g_type_specialized_construct (TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE_LIST));

  priv->native_cands_prepared = FALSE;
  priv->native_codecs_prepared = FALSE;

  priv->push_remote_cands_pending = FALSE;
  priv->push_remote_codecs_pending = FALSE;
}

static void
tpsip_media_stream_constructed (GObject *obj)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (
      TPSIP_MEDIA_STREAM (obj));
  GObjectClass *parent_object_class =
      G_OBJECT_CLASS (tpsip_media_stream_parent_class);

  /* call base class method */
  if (parent_object_class->constructed != NULL)
    parent_object_class->constructed (obj);

  /* XXX: overloading the remote pending send flag to check
   * if this is a locally offered stream. The code creating such streams
   * always sets the flag, because the remote end is supposed to decide
   * whether it wants to send.
   * This may look weird during a local hold. However, the pending flag
   * will be harmlessly cleared once the offer-answer is complete. */
  if ((priv->direction & TP_MEDIA_STREAM_DIRECTION_SEND) != 0
      && (priv->pending_send_flags & TP_MEDIA_STREAM_PENDING_REMOTE_SEND) != 0)
    {
      /* Block sending until the stream is remotely accepted */
      priv->pending_remote_receive = TRUE;
    }

  /* go for the bus */
  g_assert (TP_IS_DBUS_DAEMON (priv->dbus_daemon));
  tp_dbus_daemon_register_object (priv->dbus_daemon, priv->object_path, obj);
}

static void
tpsip_media_stream_get_property (GObject    *object,
			         guint       property_id,
			         GValue     *value,
			         GParamSpec *pspec)
{
  TpsipMediaStream *stream = TPSIP_MEDIA_STREAM (object);
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  switch (property_id)
    {
    case PROP_DBUS_DAEMON:
      g_value_set_object (value, priv->dbus_daemon);
      break;
    case PROP_MEDIA_SESSION:
      g_value_set_object (value, priv->session);
      break;
    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;
    case PROP_ID:
      g_value_set_uint (value, priv->id);
      break;
    case PROP_MEDIA_TYPE:
      g_value_set_uint (value, priv->media_type);
      break;
    case PROP_STATE:
      g_value_set_uint (value, priv->state);
      break;
    case PROP_DIRECTION:
      g_value_set_uint (value, priv->direction);
      break;
    case PROP_PENDING_SEND_FLAGS:
      g_value_set_uint (value, priv->pending_send_flags);
      break;
    case PROP_HOLD_STATE:
      g_value_set_boolean (value, priv->hold_state);
      break;
    case PROP_CREATED_LOCALLY:
      g_value_set_boolean (value, priv->created_locally);
      break;
    case PROP_NAT_TRAVERSAL:
      g_value_set_static_string (value, "none");
      break;
    case PROP_STUN_SERVERS:
      g_return_if_fail (priv->session != NULL);
      g_object_get_property (G_OBJECT (priv->session), "stun-servers", value);
      break;
    case PROP_RELAY_INFO:
      g_value_set_static_boxed (value, tpsip_media_stream_relay_info_empty);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
tpsip_media_stream_set_property (GObject      *object,
			         guint         property_id,
			         const GValue *value,
			         GParamSpec   *pspec)
{
  TpsipMediaStream *stream = TPSIP_MEDIA_STREAM (object);
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  switch (property_id)
    {
    case PROP_DBUS_DAEMON:
      g_assert (priv->dbus_daemon == NULL);       /* construct-only */
      priv->dbus_daemon = g_value_dup_object (value);
      break;
    case PROP_MEDIA_SESSION:
      priv->session = g_value_get_object (value);
      break;
    case PROP_OBJECT_PATH:
      g_free (priv->object_path);
      priv->object_path = g_value_dup_string (value);
      break;
    case PROP_ID:
      priv->id = g_value_get_uint (value);
      break;
    case PROP_MEDIA_TYPE:
      priv->media_type = g_value_get_uint (value);
      break;
    case PROP_STATE:
      priv->state = g_value_get_uint (value);
      break;
    case PROP_DIRECTION:
      priv->direction = g_value_get_uint (value);
      break;
    case PROP_PENDING_SEND_FLAGS:
      priv->pending_send_flags = g_value_get_uint (value);
      break;
    case PROP_HOLD_STATE:
      priv->hold_state = g_value_get_boolean (value);
      break;
    case PROP_CREATED_LOCALLY:
      priv->created_locally = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void tpsip_media_stream_dispose (GObject *object);
static void tpsip_media_stream_finalize (GObject *object);

static void
tpsip_media_stream_class_init (TpsipMediaStreamClass *klass)
{
  static TpDBusPropertiesMixinPropImpl stream_handler_props[] = {
      { "CreatedLocally", "created-locally", NULL },
      { "NATTraversal", "nat-traversal", NULL },
      { "STUNServers", "stun-servers", NULL },
      { "RelayInfo", "relay-info", NULL },
      { NULL }
  };

  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
      { TP_IFACE_MEDIA_STREAM_HANDLER,
        tp_dbus_properties_mixin_getter_gobject_properties,
        NULL,
        stream_handler_props,
      },
      { NULL }
  };

  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GType stream_type = G_OBJECT_CLASS_TYPE (klass);
  GParamSpec *param_spec;

  g_type_class_add_private (klass, sizeof (TpsipMediaStreamPrivate));

  object_class->constructed = tpsip_media_stream_constructed;

  object_class->get_property = tpsip_media_stream_get_property;
  object_class->set_property = tpsip_media_stream_set_property;

  object_class->dispose = tpsip_media_stream_dispose;
  object_class->finalize = tpsip_media_stream_finalize;

  param_spec = g_param_spec_object ("dbus-daemon", "TpDBusDaemon",
      "Connection to D-Bus.", TP_TYPE_DBUS_DAEMON,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DBUS_DAEMON, param_spec);

  param_spec = g_param_spec_object ("media-session", "TpsipMediaSession object",
      "SIP media session object that owns this media stream object.",
      TPSIP_TYPE_MEDIA_SESSION,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_MEDIA_SESSION, param_spec);

  param_spec = g_param_spec_string ("object-path", "D-Bus object path",
      "The D-Bus object path used for this object on the bus.",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_OBJECT_PATH, param_spec);

  param_spec = g_param_spec_uint ("id", "Stream ID",
      "A stream number for the stream used in the D-Bus API.",
      0, G_MAXUINT,
      0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_ID, param_spec);

  param_spec = g_param_spec_uint ("media-type", "Stream media type",
      "A constant indicating which media type the stream carries.",
      TP_MEDIA_STREAM_TYPE_AUDIO, TP_MEDIA_STREAM_TYPE_VIDEO,
      TP_MEDIA_STREAM_TYPE_AUDIO,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_MEDIA_TYPE, param_spec);

  param_spec = g_param_spec_uint ("state", "Connection state",
      "Connection state of the media stream",
      TP_MEDIA_STREAM_STATE_DISCONNECTED, TP_MEDIA_STREAM_STATE_CONNECTED,
      TP_MEDIA_STREAM_STATE_DISCONNECTED,
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STATE, param_spec);

  /* We don't change the following two as individual properties
   * after construction, use tpsip_media_stream_set_direction() */

  param_spec = g_param_spec_uint ("direction", "Stream direction",
      "A value indicating the current direction of the stream",
      TP_MEDIA_STREAM_DIRECTION_NONE, TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL,
      TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_DIRECTION, param_spec);

  param_spec = g_param_spec_uint ("pending-send-flags", "Pending send flags",
      "Flags indicating the current pending send state of the stream",
      0,
      TP_MEDIA_STREAM_PENDING_LOCAL_SEND | TP_MEDIA_STREAM_PENDING_REMOTE_SEND,
      0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class,
                                   PROP_PENDING_SEND_FLAGS,
                                   param_spec);

  param_spec = g_param_spec_boolean ("hold-state", "Hold state",
      "Hold state of the media stream as reported by the stream engine",
      FALSE,
      G_PARAM_CONSTRUCT | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class,
                                   PROP_HOLD_STATE,
                                   param_spec);

  param_spec = g_param_spec_boolean ("created-locally", "Created locally?",
      "True if this stream was created by the local user", FALSE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_CREATED_LOCALLY,
      param_spec);

  param_spec = g_param_spec_string ("nat-traversal", "NAT traversal",
      "NAT traversal mechanism for this stream", NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_NAT_TRAVERSAL,
      param_spec);

  param_spec = g_param_spec_boxed ("stun-servers", "STUN servers",
      "Array of IP address-port pairs for available STUN servers",
      TP_ARRAY_TYPE_SOCKET_ADDRESS_IP_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STUN_SERVERS, param_spec);

  param_spec = g_param_spec_boxed ("relay-info", "Relay info",
      "Array of mappings containing relay server information",
      TP_ARRAY_TYPE_STRING_VARIANT_MAP_LIST,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_RELAY_INFO, param_spec);

  tpsip_media_stream_relay_info_empty = g_ptr_array_new ();

  /* signals not exported by DBus interface */
  signals[SIG_READY] =
    g_signal_new ("ready",
                  stream_type,
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[SIG_SUPPORTED_CODECS] =
    g_signal_new ("supported-codecs",
                  stream_type,
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIG_STATE_CHANGED] =
    g_signal_new ("state-changed",
                  stream_type,
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);

  signals[SIG_DIRECTION_CHANGED] =
    g_signal_new ("direction-changed",
                  stream_type,
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  _tpsip_marshal_VOID__UINT_UINT,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);

  signals[SIG_LOCAL_MEDIA_UPDATED] =
    g_signal_new ("local-media-updated",
                  stream_type,
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[SIG_UNHOLD_FAILURE] =
    g_signal_new ("unhold-failure",
                  stream_type,
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  klass->dbus_props_class.interfaces = prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (TpsipMediaStreamClass, dbus_props_class));
}

void
tpsip_media_stream_dispose (GObject *object)
{
  TpsipMediaStream *self = TPSIP_MEDIA_STREAM (object);
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  tp_clear_object (&priv->dbus_daemon);

  if (G_OBJECT_CLASS (tpsip_media_stream_parent_class)->dispose)
    G_OBJECT_CLASS (tpsip_media_stream_parent_class)->dispose (object);

  DEBUG ("exit");
}

void
tpsip_media_stream_finalize (GObject *object)
{
  TpsipMediaStream *self = TPSIP_MEDIA_STREAM (object);
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);

  /* free any data held directly by the object here */
  g_free (priv->object_path);
  g_free (priv->stream_sdp);

  g_value_unset (&priv->native_codecs);
  g_value_unset (&priv->native_candidates);

  g_free (priv->native_candidate_id);
  g_free (priv->remote_candidate_id);

  G_OBJECT_CLASS (tpsip_media_stream_parent_class)->finalize (object);

  DEBUG ("exit");
}

/***********************************************************************
 * Set: Media.StreamHandler interface implementation (same for 0.12/0.13???)
 ***********************************************************************/

/**
 * tpsip_media_stream_codec_choice
 *
 * Implements DBus method CodecChoice
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
tpsip_media_stream_codec_choice (TpSvcMediaStreamHandler *iface,
                               guint codec_id,
                               DBusGMethodInvocation *context)
{
  /* Inform the connection manager of the current codec choice. 
   * -> note: not implemented by tp-gabble either (2006/May) */

  DEBUG ("not implemented (ignoring)");

  tp_svc_media_stream_handler_return_from_codec_choice (context);
}

/**
 * tpsip_media_stream_error
 *
 * Implements DBus method Error
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
tpsip_media_stream_error (TpSvcMediaStreamHandler *iface,
                          guint errno,
                          const gchar *message,
                          DBusGMethodInvocation *context)
{
  DEBUG("StreamHandler.Error called: %u %s", errno, message);

  tpsip_media_stream_close (TPSIP_MEDIA_STREAM (iface));

  tp_svc_media_stream_handler_return_from_error (context);
}

/**
 * tpsip_media_stream_native_candidates_prepared
 *
 * Implements DBus method NativeCandidatesPrepared
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
tpsip_media_stream_native_candidates_prepared (TpSvcMediaStreamHandler *iface,
                                             DBusGMethodInvocation *context)
{
  /* purpose: "Informs the connection manager that all possible native candisates
   *          have been discovered for the moment." 
   */

  TpsipMediaStream *obj = TPSIP_MEDIA_STREAM (iface);
  TpsipMediaStreamPrivate *priv;

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (obj);

  DEBUG("enter");

  priv->native_cands_prepared = TRUE;

  if (priv->native_codecs_prepared)
    priv_generate_sdp (obj);

  push_active_candidate_pair (obj);

  tp_svc_media_stream_handler_return_from_native_candidates_prepared (context);
}


/**
 * tpsip_media_stream_new_active_candidate_pair
 *
 * Implements DBus method NewActiveCandidatePair
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
tpsip_media_stream_new_active_candidate_pair (TpSvcMediaStreamHandler *iface,
                                            const gchar *native_candidate_id,
                                            const gchar *remote_candidate_id,
                                            DBusGMethodInvocation *context)
{
  TpsipMediaStream *obj = TPSIP_MEDIA_STREAM (iface);
  TpsipMediaStreamPrivate *priv;

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (obj);

  DEBUG("stream engine reported new active candidate pair %s-%s",
        native_candidate_id, remote_candidate_id);

  if (priv->remote_candidate_id == NULL
      || strcmp (priv->remote_candidate_id, remote_candidate_id))
    {
      GError *err;
      err = g_error_new (TP_ERRORS,
                         TP_ERROR_INVALID_ARGUMENT,
                         "Remote candidate ID does not match the locally "
                                "stored data");
      dbus_g_method_return_error (context, err);
      g_error_free (err);
      return;
    }

  tp_svc_media_stream_handler_return_from_new_active_candidate_pair (context);
}


/**
 * tpsip_media_stream_new_native_candidate
 *
 * Implements DBus method NewNativeCandidate
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
tpsip_media_stream_new_native_candidate (TpSvcMediaStreamHandler *iface,
                                       const gchar *candidate_id,
                                       const GPtrArray *transports,
                                       DBusGMethodInvocation *context)
{
  TpsipMediaStream *obj = TPSIP_MEDIA_STREAM (iface);
  TpsipMediaStreamPrivate *priv;
  GPtrArray *candidates;
  GValue candidate = { 0, };
  GValue transport = { 0, };
  gint tr_goodness;

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (obj);

  if (priv->stream_sdp != NULL)
    {
      MESSAGE ("Stream %u: SDP already generated, ignoring native candidate '%s'", priv->id, candidate_id);
      tp_svc_media_stream_handler_return_from_new_native_candidate (context);
      return;
    }

  g_return_if_fail (transports->len >= 1);

  /* Rate the preferability of the address */
  g_value_init (&transport, TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_TRANSPORT);
  g_value_set_static_boxed (&transport, g_ptr_array_index (transports, 0));
  tr_goodness = tpsip_media_session_rate_native_transport (priv->session,
                                                           &transport);

  candidates = g_value_get_boxed (&priv->native_candidates);

  if (tr_goodness > 0)
    {
      DEBUG("native candidate '%s' is rated as preferable", candidate_id);
      g_free (priv->native_candidate_id);
      priv->native_candidate_id = g_strdup (candidate_id);

      /* Drop the candidates received previously */
      g_value_reset (&priv->native_candidates);
      candidates = dbus_g_type_specialized_construct (
                                TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE_LIST);
      g_value_take_boxed (&priv->native_candidates, candidates);
    }

  g_value_init (&candidate, TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE);
  g_value_take_boxed (&candidate,
      dbus_g_type_specialized_construct (TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE));

  dbus_g_type_struct_set (&candidate,
      0, candidate_id,
      1, transports,
      G_MAXUINT);

  g_ptr_array_add (candidates, g_value_get_boxed (&candidate));

  SESSION_DEBUG(priv->session, "put native candidate '%s' from stream-engine into cache", candidate_id);

  tp_svc_media_stream_handler_return_from_new_native_candidate (context);
}

static void
priv_set_local_codecs (TpsipMediaStream *self,
                       const GPtrArray *codecs)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);
  GValue val = { 0, };

  SESSION_DEBUG(priv->session, "putting list of %d locally supported "
                "codecs from stream-engine into cache", codecs->len);
  g_value_init (&val, TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CODEC_LIST);
  g_value_set_static_boxed (&val, codecs);
  g_value_copy (&val, &priv->native_codecs);

  priv->native_codecs_prepared = TRUE;
  if (priv->native_cands_prepared)
    priv_generate_sdp (self);
}

static void
tpsip_media_stream_codecs_updated (TpSvcMediaStreamHandler *iface,
                                   const GPtrArray *codecs,
                                   DBusGMethodInvocation *context)
{
  TpsipMediaStream *self = TPSIP_MEDIA_STREAM (iface);
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);

  if (!priv->native_codecs_prepared)
    {
      GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "CodecsUpdated may not be called before codecs have been provided "
          "with SetLocalCodecs or Ready" };

      SESSION_DEBUG(priv->session,
          "CodecsUpdated called before SetLocalCodecs or Ready");

      dbus_g_method_return_error (context, &e);
    }
  else
    {
      GValue val = { 0, };

      SESSION_DEBUG(priv->session, "putting list of %d locally supported "
          "codecs from CodecsUpdated into cache", codecs->len);
      g_value_init (&val, TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CODEC_LIST);
      g_value_set_static_boxed (&val, codecs);
      g_value_copy (&val, &priv->native_codecs);

      /* This doesn't use priv_generate_sdp because it short-circuits if
       * priv->stream_sdp is already set. We want to update it.
       */
      if (priv->native_cands_prepared)
        priv_update_local_sdp (self);

      tp_svc_media_stream_handler_return_from_codecs_updated (context);
    }
}

/**
 * tpsip_media_stream_ready
 *
 * Implements DBus method Ready
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
tpsip_media_stream_ready (TpSvcMediaStreamHandler *iface,
                          const GPtrArray *codecs,
                          DBusGMethodInvocation *context)
{
  /* purpose: "Inform the connection manager that a client is ready to handle
   *          this StreamHandler. Also provide it with info about all supported
   *          codecs."
   *
   * - note, with SIP we don't send the invite just yet (we need
   *   candidates first
   */

  TpsipMediaStream *obj = TPSIP_MEDIA_STREAM (iface);
  TpsipMediaStreamPrivate *priv;

  DEBUG ("enter");

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (obj);

  if (priv->ready_received)
    {
      MESSAGE ("Ready called more than once");
      tp_svc_media_stream_handler_return_from_ready (context);
      return;
    }

  priv->ready_received = TRUE;

  if (codecs->len != 0)
    priv_set_local_codecs (obj, codecs);

  /* Push the initial sending/playing state */
  tp_svc_media_stream_handler_emit_set_stream_playing (
        iface, priv->playing);
  tp_svc_media_stream_handler_emit_set_stream_sending (
        iface, priv->sending);

  priv->native_codecs_prepared = TRUE;
  if (priv->native_cands_prepared)
    priv_generate_sdp (obj);

  if (priv->push_remote_cands_pending)
    {
      priv->push_remote_cands_pending = FALSE;
      push_remote_candidates (obj);
    }
  if (priv->push_remote_codecs_pending)
    {
      priv->push_remote_codecs_pending = FALSE;
      push_remote_codecs (obj);
    }

  /* note: for inbound sessions, emit active candidate pair once 
           remote info is set */
  push_active_candidate_pair (obj);

  tp_svc_media_stream_handler_return_from_ready (context);
}

static void
tpsip_media_stream_set_local_codecs (TpSvcMediaStreamHandler *iface,
                                     const GPtrArray *codecs,
                                     DBusGMethodInvocation *context)
{
  priv_set_local_codecs (TPSIP_MEDIA_STREAM (iface), codecs);
  tp_svc_media_stream_handler_return_from_set_local_codecs (context);
}

/**
 * tpsip_media_stream_stream_state
 *
 * Implements DBus method StreamState
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
tpsip_media_stream_stream_state (TpSvcMediaStreamHandler *iface,
                               guint state,
                               DBusGMethodInvocation *context)
{
  /* purpose: "Informs the connection manager of the stream's current state
   *           State is as specified in *ChannelTypeStreamedMedia::GetStreams."
   *
   * - set the stream state for session
   */

  TpsipMediaStream *obj = TPSIP_MEDIA_STREAM (iface);
  TpsipMediaStreamPrivate *priv;
  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (obj);

  if (priv->state != state)
    {
      DEBUG("changing stream state from %u to %u", priv->state, state);
      priv->state = state;
      g_signal_emit (obj, signals[SIG_STATE_CHANGED], 0, state);
    }

  tp_svc_media_stream_handler_return_from_stream_state (context);
}

/**
 * tpsip_media_stream_supported_codecs
 *
 * Implements DBus method SupportedCodecs
 * on interface org.freedesktop.Telepathy.Media.StreamHandler
 */
static void
tpsip_media_stream_supported_codecs (TpSvcMediaStreamHandler *iface,
                                   const GPtrArray *codecs,
                                   DBusGMethodInvocation *context)
{
  /* purpose: "Inform the connection manager of the supported codecs for this session.
   *          This is called after the connection manager has emitted SetRemoteCodecs
   *          to notify what codecs are supported by the peer, and will thus be an
   *          intersection of all locally supported codecs (passed to Ready)
   *          and those supported by the peer."
   *
   * - emit SupportedCodecs
   */ 

  TpsipMediaStream *self = TPSIP_MEDIA_STREAM (iface);
  TpsipMediaStreamPrivate *priv;
  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);

  DEBUG("got codec intersection containing %u codecs from stream-engine",
        codecs->len);

  /* Uncomment the line below if there's need to limit the local codec list
   * with the intersection for later SDP negotiations.
   * TODO: Make sure to update the SDP for the stream as well. */
  /* g_value_set_boxed (&priv->native_codecs, codecs); */

  if (priv->codec_intersect_pending)
    {
      if (priv->push_remote_codecs_pending)
        {
          /* The remote codec list has been updated since the intersection
           * has started, plunge into a new intersection immediately */
          priv->push_remote_codecs_pending = FALSE;
          push_remote_codecs (self);
        }
      else
        {
          priv->codec_intersect_pending = FALSE;
          g_signal_emit (self, signals[SIG_SUPPORTED_CODECS], 0, codecs->len);
        }
    }
  else
    WARNING("SupportedCodecs called when no intersection is ongoing");

  tp_svc_media_stream_handler_return_from_supported_codecs (context);
}

static void
tpsip_media_stream_hold_state (TpSvcMediaStreamHandler *self,
                               gboolean held,
                               DBusGMethodInvocation *context)
{
  g_object_set (self, "hold-state", held, NULL);
  tp_svc_media_stream_handler_return_from_hold_state (context);
}

static void
tpsip_media_stream_unhold_failure (TpSvcMediaStreamHandler *self,
                                   DBusGMethodInvocation *context)
{
  /* Not doing anything to hold_state or requested_hold_state,
   * because the session is going to put all streams on hold after getting
   * the signal below */

  g_signal_emit (self, signals[SIG_UNHOLD_FAILURE], 0);
  tp_svc_media_stream_handler_return_from_unhold_failure (context);
}

/***********************************************************************
 * Helper functions follow (not based on generated templates)
 ***********************************************************************/

guint
tpsip_media_stream_get_id (TpsipMediaStream *self)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);
  return priv->id;
}

guint
tpsip_media_stream_get_media_type (TpsipMediaStream *self)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);
  return priv->media_type;
}

void
tpsip_media_stream_close (TpsipMediaStream *self)
{
  tp_svc_media_stream_handler_emit_close (self);
}

/**
 * Described the local stream configuration in SDP (RFC2327),
 * or NULL if stream not configured yet.
 */
const char *tpsip_media_stream_local_sdp (TpsipMediaStream *obj)
{
  TpsipMediaStreamPrivate *priv;
  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (obj);
  return priv->stream_sdp;
}

TpMediaStreamDirection
tpsip_media_stream_direction_from_remote_media (const sdp_media_t *media)
{
  sdp_mode_t mode = media->m_mode;
  return ((mode & sdp_recvonly)? TP_MEDIA_STREAM_DIRECTION_SEND : 0)
       | ((mode & sdp_sendonly)? TP_MEDIA_STREAM_DIRECTION_RECEIVE : 0);
}

static gboolean
tpsip_sdp_codecs_differ (const sdp_rtpmap_t *m1, const sdp_rtpmap_t *m2)
{
  while (m1 != NULL && m2 != NULL)
    {
      if (sdp_rtpmap_cmp (m1, m2) != 0)
        return TRUE;
      m1 = m1->rm_next;
      m2 = m2->rm_next;
    }
  return m1 != NULL || m2 != NULL;
}

/*
 * Returns stream direction as requested by the latest local or remote
 * direction change.
 */
static TpMediaStreamDirection
priv_get_requested_direction (TpsipMediaStreamPrivate *priv)
{
  TpMediaStreamDirection direction;

  direction = priv->direction;
  if ((priv->pending_send_flags & TP_MEDIA_STREAM_PENDING_LOCAL_SEND) != 0)
    direction |= TP_MEDIA_STREAM_DIRECTION_SEND;
  return direction;
}

/** 
 * Sets the remote candidates and codecs for this stream, as 
 * received via signaling.
 * 
 * Parses the SDP information, updates TP remote candidates and
 * codecs if the client is ready.
 * 
 * Note that the pointer to the media description structure is saved,
 * implying that the structure shall not go away for the lifetime of
 * the stream, preferably kept in the memory home attached to
 * the session object.
 *
 * @return TRUE if the remote information has been accepted,
 *         FALSE if the update is not acceptable.
 */
gboolean
tpsip_media_stream_set_remote_media (TpsipMediaStream *stream,
                                     const sdp_media_t *new_media,
                                     guint direction_up_mask,
                                     guint pending_send_mask)
{
  TpsipMediaStreamPrivate *priv;
  sdp_connection_t *sdp_conn;
  const sdp_media_t *old_media;
  gboolean transport_changed = TRUE;
  gboolean codecs_changed = TRUE;
  guint old_direction;
  guint new_direction;

  DEBUG ("enter");

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  /* Do sanity checks */

  g_return_val_if_fail (new_media != NULL, FALSE);

  if (new_media->m_rejected || new_media->m_port == 0)
    {
      DEBUG("the stream is rejected remotely");
      return FALSE;
    }

  if (new_media->m_proto != sdp_proto_rtp)
    {
      WARNING ("Stream %u: the remote protocol is not RTP/AVP", priv->id);
      return FALSE;
    }

  sdp_conn = sdp_media_connections (new_media);
  if (sdp_conn == NULL)
    {
      WARNING ("Stream %u: no valid remote connections", priv->id);
      return FALSE;
    }

  if (new_media->m_rtpmaps == NULL)
    {
      WARNING ("Stream %u: no remote codecs", priv->id);
      return FALSE;
    }

  /* Note: always update the pointer to the current media structure
   * because of memory management done in the session object */
  old_media = priv->remote_media;
  priv->remote_media = new_media;

  /* Check if there was any media update at all */

  if (sdp_media_cmp (old_media, new_media) == 0)
    {
      DEBUG("no media changes detected for the stream");
      return TRUE;
    }

  old_direction = priv_get_requested_direction (priv);
  new_direction = tpsip_media_stream_direction_from_remote_media (new_media);

  /* Make sure the peer can only enable sending or receiving direction
   * if it's allowed to */
  new_direction &= old_direction | direction_up_mask;

  if (old_media != NULL)
    {
      /* Check if the transport candidate needs to be changed */
      if (!sdp_connection_cmp (sdp_media_connections (old_media), sdp_conn))
        transport_changed = FALSE;

      /* Check if the codec list needs to be updated */
      codecs_changed = tpsip_sdp_codecs_differ (old_media->m_rtpmaps,
                                                new_media->m_rtpmaps);

      /* Disable sending at this point if it will be disabled
       * accordingly to the new direction */
      priv_update_sending (stream,
                           priv->direction & new_direction);
    }

  /* First add the new candidate, then update the codec set.
   * The offerer isn't supposed to send us anything from the new transport
   * until we accept; if it's the answer, both orderings have problems. */

  if (transport_changed)
    {
      /* Make sure we stop sending before we use the new set of codecs
       * intended for the new connection */
      if (codecs_changed) 
        tpsip_media_stream_set_sending (stream, FALSE);

      push_remote_candidates (stream);
    }

  if (codecs_changed)
    {
      if (!priv->codec_intersect_pending)
        {
          priv->codec_intersect_pending = TRUE;
          push_remote_codecs (stream);
        }
      else
        {
          priv->push_remote_codecs_pending = TRUE;
        }
    }

  /* TODO: this will go to session change commit code */

  /* note: for outbound sessions (for which remote cands become
   *       available at a later stage), emit active candidate pair 
   *       (and playing status?) once remote info set */
  push_active_candidate_pair (stream);

  /* Set the final direction and update pending send flags */
  tpsip_media_stream_set_direction (stream,
                                    new_direction,
                                    pending_send_mask);

  return TRUE;
}

/**
 * Converts a sofia-sip media type enum to Telepathy media type.
 * See <sofia-sip/sdp.h> and <telepathy-constants.h>.
 *
 * @return G_MAXUINT if the media type cannot be mapped
 */
guint
tpsip_tp_media_type (sdp_media_e sip_mtype)
{
  switch (sip_mtype)
    {
      case sdp_media_audio: return TP_MEDIA_STREAM_TYPE_AUDIO;
      case sdp_media_video: return TP_MEDIA_STREAM_TYPE_VIDEO; 
      default: return G_MAXUINT;
    }
}

/**
 * Sets the media state to playing or non-playing. When not playing,
 * received RTP packets may not be played locally.
 */
void tpsip_media_stream_set_playing (TpsipMediaStream *stream, gboolean playing)
{
  TpsipMediaStreamPrivate *priv;
  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  if (same_boolean (priv->playing, playing))
    return;

  DEBUG("set playing to %s", playing? "TRUE" : "FALSE");

  priv->playing = playing;

  if (priv->ready_received)
    tp_svc_media_stream_handler_emit_set_stream_playing (
        (TpSvcMediaStreamHandler *)stream, playing);
}

/**
 * Sets the media state to sending or non-sending. When not sending,
 * captured media are not sent over the network.
 */
void
tpsip_media_stream_set_sending (TpsipMediaStream *stream, gboolean sending)
{
  TpsipMediaStreamPrivate *priv;
  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  if (same_boolean(priv->sending, sending))
    return;

  DEBUG("set sending to %s", sending? "TRUE" : "FALSE");

  priv->sending = sending;

  if (priv->ready_received)
    tp_svc_media_stream_handler_emit_set_stream_sending (
        (TpSvcMediaStreamHandler *)stream, sending);
}

static void
priv_update_sending (TpsipMediaStream *stream,
                     TpMediaStreamDirection direction)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);
  gboolean sending = TRUE;

  /* XXX: the pending send flag check is probably an overkill
   * considering that effective sending direction and pending send should be
   * mutually exclusive */
  if ((direction & TP_MEDIA_STREAM_DIRECTION_SEND) == 0
      || priv->pending_remote_receive
      || (priv->pending_send_flags & TP_MEDIA_STREAM_PENDING_LOCAL_SEND) != 0
      || !tpsip_media_session_is_accepted (priv->session))
    {
      sending = FALSE;
    }

  tpsip_media_stream_set_sending (stream, sending);
}

void
tpsip_media_stream_set_direction (TpsipMediaStream *stream,
                                  TpMediaStreamDirection direction,
                                  guint pending_send_mask)
{
  TpsipMediaStreamPrivate *priv;
  guint pending_send_flags;
  TpMediaStreamDirection old_sdp_direction;

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);
  pending_send_flags = priv->pending_send_flags & pending_send_mask;

  if ((direction & ~priv->direction & TP_MEDIA_STREAM_DIRECTION_SEND) != 0)
    {
      /* We are requested to start sending, but... */
      if ((pending_send_mask
            & TP_MEDIA_STREAM_PENDING_LOCAL_SEND) != 0)
        {
          /* ... but we need to confirm this with the client.
           * Clear the sending bit and set the pending send flag. */
          direction &= ~(guint)TP_MEDIA_STREAM_DIRECTION_SEND;
          pending_send_flags |= TP_MEDIA_STREAM_PENDING_LOCAL_SEND;
        }
      if ((pending_send_mask
              & TP_MEDIA_STREAM_PENDING_REMOTE_SEND) != 0
          && (priv->pending_send_flags
              & TP_MEDIA_STREAM_PENDING_LOCAL_SEND) == 0)
        {
          g_assert ((priv_get_requested_direction (priv) & TP_MEDIA_STREAM_DIRECTION_SEND) == 0);

          /* ... but the caller wants to agree with the remote
           * end first. Block the stream handler from sending for now. */
          priv->pending_remote_receive = TRUE;
        }
    }
  if ((direction & ~priv->direction & TP_MEDIA_STREAM_DIRECTION_RECEIVE) != 0
      && (pending_send_mask
          & TP_MEDIA_STREAM_PENDING_REMOTE_SEND) != 0)
    {
      /* We're requested to start receiving, but the remote end did not
       * confirm if it will send. Set the pending send flag. */
      pending_send_flags |= TP_MEDIA_STREAM_PENDING_REMOTE_SEND;
    }

  if (priv->direction == direction
      && priv->pending_send_flags == pending_send_flags)
    return;

  old_sdp_direction = priv_get_requested_direction (priv);

  priv->direction = direction;
  priv->pending_send_flags = pending_send_flags;

  DEBUG("set direction %u, pending send flags %u", priv->direction, priv->pending_send_flags);

  g_signal_emit (stream, signals[SIG_DIRECTION_CHANGED], 0,
                 priv->direction, priv->pending_send_flags);

  if (priv->remote_media != NULL)
    priv_update_sending (stream, priv->direction);

  if (priv->native_cands_prepared
      && priv->native_codecs_prepared
      && priv_get_requested_direction (priv)
         != old_sdp_direction)
    priv_update_local_sdp (stream);
}

/*
 * Clears the pending send flag(s) present in @pending_send_mask.
 * If #TP_MEDIA_STREAM_PENDING_LOCAL_SEND is thus cleared,
 * enable the sending bit in the stream direction.
 * If @pending_send_mask has #TP_MEDIA_STREAM_PENDING_REMOTE_SEND flag set,
 * also start sending if agreed by the stream direction.
 */
void
tpsip_media_stream_apply_pending_direction (TpsipMediaStream *stream,
                                            guint pending_send_mask)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);
  guint flags;


  /* Don't apply pending send for new streams that haven't been negotiated */
  if (priv->remote_media == NULL)
    return;

  /* Remember the flags that got changes and then clear the set */
  flags = (priv->pending_send_flags & pending_send_mask);
  priv->pending_send_flags &= ~pending_send_mask;

  if (flags != 0)
    {
      if ((flags & TP_MEDIA_STREAM_PENDING_LOCAL_SEND) != 0)
        priv->direction |= TP_MEDIA_STREAM_DIRECTION_SEND;

      DEBUG("set direction %u, pending send flags %u", priv->direction, priv->pending_send_flags);

      g_signal_emit (stream, signals[SIG_DIRECTION_CHANGED], 0,
                     priv->direction, priv->pending_send_flags);
    }

  if ((pending_send_mask & TP_MEDIA_STREAM_PENDING_REMOTE_SEND) != 0)
    {
      priv->pending_remote_receive = FALSE;
      DEBUG("remote end ready to receive");
    }

  /* Always check to enable sending because the session could become accepted */
  priv_update_sending (stream, priv->direction);
}

TpMediaStreamDirection
tpsip_media_stream_get_requested_direction (TpsipMediaStream *self)
{
  return priv_get_requested_direction (TPSIP_MEDIA_STREAM_GET_PRIVATE (self));
}

/**
 * Returns true if the stream has a valid SDP description and
 * connection has been established with the stream engine.
 */
gboolean tpsip_media_stream_is_local_ready (TpsipMediaStream *self)
{
  TpsipMediaStreamPrivate *priv;
  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);
  g_assert (priv->stream_sdp == NULL || priv->ready_received);
  return (priv->stream_sdp != NULL);
}

gboolean
tpsip_media_stream_is_codec_intersect_pending (TpsipMediaStream *self)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);
  return priv->codec_intersect_pending;
}

void
tpsip_media_stream_start_telephony_event (TpsipMediaStream *self, guchar event)
{
  tp_svc_media_stream_handler_emit_start_telephony_event (
        (TpSvcMediaStreamHandler *)self, event);
}

void
tpsip_media_stream_stop_telephony_event  (TpsipMediaStream *self)
{
  tp_svc_media_stream_handler_emit_stop_telephony_event (
        (TpSvcMediaStreamHandler *)self);
}

gboolean
tpsip_media_stream_request_hold_state (TpsipMediaStream *self, gboolean hold)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);

  if ((!priv->requested_hold_state) != (!hold))
    {
      priv->requested_hold_state = hold;
      tp_svc_media_stream_handler_emit_set_stream_held (self, hold);
      return TRUE;
    }
  return FALSE;
}

static void
priv_generate_sdp (TpsipMediaStream *self)
{
  TpsipMediaStreamPrivate *priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (self);

  if (priv->stream_sdp != NULL)
    return;

  priv_update_local_sdp (self);

  g_assert (priv->stream_sdp != NULL);

  g_signal_emit (self, signals[SIG_READY], 0);
}

/**
 * Notify StreamEngine of remote codecs.
 *
 * @pre Ready signal must be receiveid (priv->ready_received)
 */
static void push_remote_codecs (TpsipMediaStream *stream)
{
  TpsipMediaStreamPrivate *priv;
  GPtrArray *codecs;
  GHashTable *opt_params;
  GType codecs_type;
  GType codec_type;
  const sdp_media_t *sdpmedia;
  const sdp_rtpmap_t *rtpmap;
  gchar *ptime = NULL;
  gchar *max_ptime = NULL;

  DEBUG ("enter");

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  sdpmedia = priv->remote_media; 
  if (sdpmedia == NULL)
    {
      DEBUG("remote media description is not received yet");
      return;
    }

  if (!priv->ready_received)
    {
      DEBUG("the stream engine is not ready, SetRemoteCodecs is pending");
      priv->push_remote_codecs_pending = TRUE;
      return;
    }

  ptime = tpsip_sdp_get_string_attribute (sdpmedia->m_attributes, "ptime");
  if (ptime == NULL)
    {
      g_object_get (priv->session,
          "remote-ptime", &ptime,
          NULL);
    }
  max_ptime = tpsip_sdp_get_string_attribute (sdpmedia->m_attributes, "maxptime");
  if (max_ptime == NULL)
    {
      g_object_get (priv->session,
          "remote-max-ptime", &max_ptime,
          NULL);
    }

  codec_type = TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_CODEC;
  codecs_type = TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CODEC_LIST;

  codecs = dbus_g_type_specialized_construct (codecs_type);
  opt_params = g_hash_table_new_full (g_str_hash,
                                      g_str_equal,
                                      g_free,
                                      g_free);

  rtpmap = sdpmedia->m_rtpmaps;
  while (rtpmap)
    {
      GValue codec = { 0, };

      g_value_init (&codec, codec_type);
      g_value_take_boxed (&codec,
                          dbus_g_type_specialized_construct (codec_type));

      if (ptime != NULL)
        g_hash_table_insert (opt_params,
            g_strdup("ptime"), g_strdup (ptime));
      if (max_ptime != NULL)
        g_hash_table_insert (opt_params,
            g_strdup("maxptime"), g_strdup (max_ptime));

      tpsip_codec_param_parse (priv->media_type, rtpmap->rm_encoding,
          rtpmap->rm_fmtp, opt_params);

      /* RFC2327: see "m=" line definition 
       *  - note, 'encoding_params' is assumed to be channel
       *    count (i.e. channels in farsight) */

      dbus_g_type_struct_set (&codec,
                              /* payload type: */
                              0, rtpmap->rm_pt,
                              /* encoding name: */
                              1, rtpmap->rm_encoding,
                              /* media type */
                              2, (guint)priv->media_type,
                              /* clock-rate */
                              3, rtpmap->rm_rate,
                              /* number of supported channels: */
                              4, rtpmap->rm_params ? atoi(rtpmap->rm_params) : 0,
                              /* optional params: */
                              5, opt_params,
                              G_MAXUINT);

      g_hash_table_remove_all (opt_params);

      g_ptr_array_add (codecs, g_value_get_boxed (&codec));

      rtpmap = rtpmap->rm_next;
    }

  g_hash_table_destroy (opt_params);
  g_free (ptime);
  g_free (max_ptime);

  SESSION_DEBUG(priv->session, "passing %d remote codecs to stream engine",
                codecs->len);

  tp_svc_media_stream_handler_emit_set_remote_codecs (
        (TpSvcMediaStreamHandler *)stream, codecs);

  g_boxed_free (codecs_type, codecs);
}

static void push_remote_candidates (TpsipMediaStream *stream)
{
  TpsipMediaStreamPrivate *priv;
  GValue candidate = { 0 };
  GValue transport = { 0 };
  GValue transport_rtcp = { 0 };
  GPtrArray *candidates;
  GPtrArray *transports;
  GType candidate_type;
  GType candidates_type;
  GType transport_type;
  GType transports_type;
  const sdp_media_t *media;
  const sdp_connection_t *sdp_conn;
  gchar *candidate_id;
  guint port;

  DEBUG("enter");

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  media = priv->remote_media; 
  if (media == NULL)
    {
      DEBUG("remote media description is not received yet");
      return;
    }

  if (!priv->ready_received)
    {
      DEBUG("the stream engine is not ready, SetRemoteCandidateList is pending");
      priv->push_remote_cands_pending = TRUE;
      return;
    }

  /* use the address from SDP c-line as the only remote candidate */

  sdp_conn = sdp_media_connections (media);
  g_return_if_fail (sdp_conn != NULL);

  port = (guint) media->m_port;

  transports_type = TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_TRANSPORT_LIST;
  transports = dbus_g_type_specialized_construct (transports_type);

  transport_type = TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_TRANSPORT;
  g_value_init (&transport, transport_type);
  g_value_take_boxed (&transport,
                      dbus_g_type_specialized_construct (transport_type));
  dbus_g_type_struct_set (&transport,
                          0, 1,         /* component number */
                          1, sdp_conn->c_address,
                          2, port,
                          3, TP_MEDIA_STREAM_BASE_PROTO_UDP,
                          4, "RTP",
                          5, "AVP",
                          /* 6, 0.0f, */
                          7, TP_MEDIA_STREAM_TRANSPORT_TYPE_LOCAL,
                          /* 8, "", */
                          /* 9, "", */
                          G_MAXUINT);

  DEBUG("remote RTP address=<%s>, port=<%u>", sdp_conn->c_address, port);
  g_ptr_array_add (transports, g_value_get_boxed (&transport));

  if (!tpsip_sdp_rtcp_bandwidth_throttled (media->m_bandwidths))
    {
      gboolean session_rtcp_enabled = TRUE;
      g_object_get (priv->session,
                    "rtcp-enabled", &session_rtcp_enabled,
                    NULL);
      if (session_rtcp_enabled)
        {
          const sdp_attribute_t *rtcp_attr;
          const char *rtcp_address;
          guint rtcp_port;

          /* Get the port and optional address for RTCP accordingly to RFC 3605 */
          rtcp_address = sdp_conn->c_address;
          rtcp_attr = sdp_attribute_find (media->m_attributes, "rtcp");
          if (rtcp_attr == NULL || rtcp_attr->a_value == NULL)
            {
              rtcp_port = port + 1;
            }
          else
            {
              const char *rest;
              rtcp_port = (guint) g_ascii_strtoull (rtcp_attr->a_value,
                                                    (gchar **) &rest,
                                                    10);
              if (rtcp_port != 0
                  && (strncmp (rest, " IN IP4 ", 8) == 0
                      || strncmp (rest, " IN IP6 ", 8) == 0))
                rtcp_address = rest + 8;
            }

          g_value_init (&transport_rtcp, transport_type);
          g_value_take_boxed (&transport_rtcp,
                              dbus_g_type_specialized_construct (transport_type));
          dbus_g_type_struct_set (&transport_rtcp,
                                  0, 2,         /* component number */
                                  1, rtcp_address,
                                  2, rtcp_port,
                                  3, TP_MEDIA_STREAM_BASE_PROTO_UDP,
                                  4, "RTCP",
                                  5, "AVP",
                                  /* 6, 0.0f, */
                                  7, TP_MEDIA_STREAM_TRANSPORT_TYPE_LOCAL,
                                  /* 8, "", */
                                  /* 9, "", */
                                  G_MAXUINT);

          DEBUG("remote RTCP address=<%s>, port=<%u>", rtcp_address, rtcp_port);
          g_ptr_array_add (transports, g_value_get_boxed (&transport_rtcp));
        }
    }

  g_free (priv->remote_candidate_id);
  candidate_id = g_strdup_printf ("L%u", ++priv->remote_candidate_counter);
  priv->remote_candidate_id = candidate_id;

  candidate_type = TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE;
  g_value_init (&candidate, candidate_type);
  g_value_take_boxed (&candidate,
                      dbus_g_type_specialized_construct (candidate_type));
  dbus_g_type_struct_set (&candidate,
      0, candidate_id,
      1, transports,
      G_MAXUINT);

  candidates_type = TP_ARRAY_TYPE_MEDIA_STREAM_HANDLER_CANDIDATE_LIST;
  candidates = dbus_g_type_specialized_construct (candidates_type);
  g_ptr_array_add (candidates, g_value_get_boxed (&candidate));

  DEBUG("emitting SetRemoteCandidateList with %s", candidate_id);

  tp_svc_media_stream_handler_emit_set_remote_candidate_list (
          (TpSvcMediaStreamHandler *)stream, candidates);

  g_boxed_free (candidates_type, candidates);
  g_boxed_free (transports_type, transports);
}

static void
push_active_candidate_pair (TpsipMediaStream *stream)
{
  TpsipMediaStreamPrivate *priv;

  DEBUG("enter");

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  if (priv->ready_received
      && priv->native_candidate_id != NULL
      && priv->remote_candidate_id != NULL)
    {
      DEBUG("emitting SetActiveCandidatePair for %s-%s",
            priv->native_candidate_id, priv->remote_candidate_id);
      tp_svc_media_stream_handler_emit_set_active_candidate_pair (
                stream, priv->native_candidate_id, priv->remote_candidate_id);
    }
}

static const char* priv_media_type_to_str(guint media_type)
{
switch (media_type)
  {
  case TP_MEDIA_STREAM_TYPE_AUDIO: return "audio";
  case TP_MEDIA_STREAM_TYPE_VIDEO: return "video";
  default: g_assert_not_reached ();
    ;
  }
return "-";
}

static void
priv_append_rtpmaps (const GPtrArray *codecs, GString *mline, GString *alines)
{
  GValue codec = { 0, };
  gchar *co_name = NULL;
  guint co_id;
  guint co_type;
  guint co_clockrate;
  guint co_channels;
  GHashTable *co_params = NULL;
  guint i;

  g_value_init (&codec, TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_CODEC);

  for (i = 0; i < codecs->len; i++)
    {
      g_value_set_static_boxed (&codec, g_ptr_array_index (codecs, i));

      dbus_g_type_struct_get (&codec,
                              0, &co_id,
                              1, &co_name,
                              2, &co_type,
                              3, &co_clockrate,
                              4, &co_channels,
                              5, &co_params,
                              G_MAXUINT);

      /* g_return_if_fail (co_type == priv->media_type); */

      /* Add rtpmap entry to the a= lines */
      g_string_append_printf (alines,
                              "a=rtpmap:%u %s/%u",
                              co_id,
                              co_name,
                              co_clockrate);
      if (co_channels > 1)
        g_string_append_printf (alines, "/%u", co_channels);
      g_string_append (alines, "\r\n");

      /* Marshal parameters into the fmtp attribute */
      if (g_hash_table_size (co_params) != 0)
        {
          GString *fmtp_value;
          g_string_append_printf (alines, "a=fmtp:%u ", co_id);
          fmtp_value = g_string_new (NULL);
          tpsip_codec_param_format (co_type, co_name,
              co_params, fmtp_value);
          g_string_append (alines, fmtp_value->str);
          g_string_free (fmtp_value, TRUE);
          g_string_append (alines, "\r\n");
        }

      /* Add PT id to the m= line */
      g_string_append_printf (mline, " %u", co_id);

      g_free (co_name);
      co_name = NULL;
      g_hash_table_destroy (co_params);
      co_params = NULL;
    }
}

/**
* Refreshes the local SDP based on Farsight stream, and current
* object, state.
*/
static void
priv_update_local_sdp(TpsipMediaStream *stream)
{
  TpsipMediaStreamPrivate *priv;
  GString *mline;
  GString *alines;
  gchar *cline;
  GValue transport = { 0 };
  const GPtrArray *candidates;
  gchar *tr_addr = NULL;
  /* gchar *tr_user = NULL; */
  /* gchar *tr_pass = NULL; */
  gchar *tr_subtype = NULL;
  gchar *tr_profile = NULL;
  guint tr_port;
  guint tr_component;
  /* guint tr_type; */
  /* gdouble tr_pref; */
  guint rtcp_port = 0;
  gchar *rtcp_address = NULL;
  const gchar *dirline;
  int i;

  priv = TPSIP_MEDIA_STREAM_GET_PRIVATE (stream);

  candidates = g_value_get_boxed (&priv->native_candidates);

  g_value_init (&transport, TP_STRUCT_TYPE_MEDIA_STREAM_HANDLER_TRANSPORT);

  /* Find the preferred candidate, if defined,
   * else the last acceptable candidate */

  for (i = candidates->len - 1; i >= 0; --i)
    {
      GValueArray *candidate;
      const gchar *candidate_id;
      const GPtrArray *ca_tports;
      guint tr_proto = TP_MEDIA_STREAM_BASE_PROTO_UDP;
      guint j;

      candidate = g_ptr_array_index (candidates, i);
      candidate_id =
                g_value_get_string (g_value_array_get_nth (candidate, 0));
      ca_tports = g_value_get_boxed (g_value_array_get_nth (candidate, 1));

      if (ca_tports->len == 0)
        {
          WARNING ("candidate '%s' lists no transports, skipping", candidate_id);
          continue;
        }

      for (j = 0; j < ca_tports->len; j++)
        {
          g_value_set_static_boxed (&transport,
                                    g_ptr_array_index (ca_tports, j));
          dbus_g_type_struct_get (&transport,
                                  0, &tr_component,
                                  G_MAXUINT);
          switch (tr_component)
            {
            case 1:     /* RTP */
              dbus_g_type_struct_get (&transport,
                                      1, &tr_addr,
                                      2, &tr_port,
                                      3, &tr_proto,
                                      4, &tr_subtype,
                                      5, &tr_profile,
                                      /* 6, &tr_pref, */
                                      /* 7, &tr_type, */
                                      /* 8, &tr_user, */
                                      /* 9, &tr_pass, */
                                      G_MAXUINT);
              break;
            case 2:     /* RTCP */
              dbus_g_type_struct_get (&transport,
                                      1, &rtcp_address,
                                      2, &rtcp_port,
                                      G_MAXUINT);
              break;
            }
        }

      if (priv->native_candidate_id != NULL)
        {
          if (!strcmp (candidate_id, priv->native_candidate_id))
            break;
        }
      else if (tr_proto == TP_MEDIA_STREAM_BASE_PROTO_UDP)
        {
          g_free (priv->native_candidate_id);
          priv->native_candidate_id = g_strdup (candidate_id);
          break;
        }
    }
  g_return_if_fail (i >= 0);
  g_return_if_fail (tr_addr != NULL);
  g_return_if_fail (tr_subtype != NULL);
  g_return_if_fail (tr_profile != NULL);

  mline = g_string_new ("m=");
  g_string_append_printf (mline,
                          "%s %u %s/%s",
                          priv_media_type_to_str (priv->media_type),
                          tr_port,
                          tr_subtype,
                          tr_profile);

  cline = g_strdup_printf ("c=IN %s %s\r\n",
                           (strchr (tr_addr, ':') == NULL)? "IP4" : "IP6",
                           tr_addr);

  switch (priv_get_requested_direction (priv))
    {
    case TP_MEDIA_STREAM_DIRECTION_BIDIRECTIONAL:
      dirline = "";
      break;
    case TP_MEDIA_STREAM_DIRECTION_SEND:
      dirline = "a=sendonly\r\n";
      break;
    case TP_MEDIA_STREAM_DIRECTION_RECEIVE:
      dirline = "a=recvonly\r\n";
      break;
    case TP_MEDIA_STREAM_DIRECTION_NONE:
      dirline = "a=inactive\r\n";
      break;
    default:
      g_assert_not_reached();
    }

  alines = g_string_new (dirline);

  if (rtcp_address != NULL)
    {
      /* Add RTCP attribute as per RFC 3605 */
      if (strcmp (rtcp_address, tr_addr) != 0)
        {
          g_string_append_printf (alines,
                                  "a=rtcp:%u IN %s %s\r\n",
                                  rtcp_port,
                                  (strchr (rtcp_address, ':') == NULL)
                                        ? "IP4" : "IP6",
                                  rtcp_address);
        }
      else if (rtcp_port != tr_port + 1)
        {
          g_string_append_printf (alines,
                                  "a=rtcp:%u\r\n",
                                  rtcp_port);
        }
    }

  priv_append_rtpmaps (g_value_get_boxed (&priv->native_codecs),
                       mline, alines);

  g_free(priv->stream_sdp);
  priv->stream_sdp = g_strconcat(mline->str, "\r\n",
                                 cline,
                                 alines->str,
                                 NULL);

  g_free (tr_addr);
  g_free (tr_profile);
  g_free (tr_subtype);
  /* g_free (tr_user); */
  /* g_free (tr_pass); */
  g_free (rtcp_address);

  g_string_free (mline, TRUE);
  g_free (cline);
  g_string_free (alines, TRUE);

  g_signal_emit (stream, signals[SIG_LOCAL_MEDIA_UPDATED], 0);
}

static void
stream_handler_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcMediaStreamHandlerClass *klass = (TpSvcMediaStreamHandlerClass *)g_iface;

#define IMPLEMENT(x) tp_svc_media_stream_handler_implement_##x (\
    klass, (tp_svc_media_stream_handler_##x##_impl) tpsip_media_stream_##x)
  IMPLEMENT(codec_choice);
  IMPLEMENT(error);
  IMPLEMENT(native_candidates_prepared);
  IMPLEMENT(new_active_candidate_pair);
  IMPLEMENT(new_native_candidate);
  IMPLEMENT(ready);
  IMPLEMENT(set_local_codecs);
  IMPLEMENT(codecs_updated);
  IMPLEMENT(stream_state);
  IMPLEMENT(supported_codecs);
  IMPLEMENT(hold_state);
  IMPLEMENT(unhold_failure);
#undef IMPLEMENT
}