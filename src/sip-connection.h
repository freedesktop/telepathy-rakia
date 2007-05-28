/*
 * sip-connection.h - Header for SIPConnection
 * Copyright (C) 2005 Collabora Ltd.
 * Copyright (C) 2005,2006 Nokia Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __SIP_CONNECTION_H__
#define __SIP_CONNECTION_H__

#include <glib-object.h>
#include <dbus/dbus-glib.h>

#include <telepathy-glib/base-connection.h>
#include "sip-sofia-decls.h"

G_BEGIN_DECLS

typedef struct _SIPConnection SIPConnection;
typedef struct _SIPConnectionClass SIPConnectionClass;
typedef struct _SIPConnectionPrivate SIPConnectionPrivate;


typedef enum /*< lowercase_name=sip_connection_keepalive_mechanism >*/
{
  SIP_CONNECTION_KEEPALIVE_AUTO = 0,	/** Keepalive management is up to the implementation */
  SIP_CONNECTION_KEEPALIVE_NONE,	/** Disable keepalive management */
  SIP_CONNECTION_KEEPALIVE_REGISTER,	/** Maintain registration with REGISTER requests */
  SIP_CONNECTION_KEEPALIVE_OPTIONS,	/** Maintain registration with OPTIONS requests */
  SIP_CONNECTION_KEEPALIVE_STUN,	/** Maintain registration with STUN as described in IETF draft-sip-outbound */
} SIPConnectionKeepaliveMechanism;


struct _SIPConnectionClass {
    TpBaseConnectionClass parent_class;
};

struct _SIPConnection {
    TpBaseConnection parent;
};

GType sip_connection_get_type(void);

#define SIP_DEFAULT_STUN_PORT 3478

/* TYPE MACROS */
#define SIP_TYPE_CONNECTION \
  (sip_connection_get_type())
#define SIP_CONNECTION(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), SIP_TYPE_CONNECTION, SIPConnection))
#define SIP_CONNECTION_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), SIP_TYPE_CONNECTION, SIPConnectionClass))
#define SIP_IS_CONNECTION(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), SIP_TYPE_CONNECTION))
#define SIP_IS_CONNECTION_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), SIP_TYPE_CONNECTION))
#define SIP_CONNECTION_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), SIP_TYPE_CONNECTION, SipConnectionClass))

G_END_DECLS

#endif /* #ifndef __SIP_CONNECTION_H__*/
