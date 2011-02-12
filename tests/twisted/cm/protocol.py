"""
Test rakia's o.fd.T.Protocol implementation
"""

import dbus
from servicetest import (unwrap, tp_path_prefix, assertEquals, assertContains,
        call_async)
from sofiatest import exec_test
import constants as cs

def test(q, bus, conn, sip):
    cm = bus.get_object(cs.CM + '.rakia',
        tp_path_prefix + '/ConnectionManager/rakia')
    cm_iface = dbus.Interface(cm, cs.CM)
    cm_prop_iface = dbus.Interface(cm, cs.PROPERTIES_IFACE)

    protocols = unwrap(cm_prop_iface.Get(cs.CM, 'Protocols'))
    assertEquals(set(['sip']), set(protocols.keys()))

    protocol_names = unwrap(cm_iface.ListProtocols())
    assertEquals(set(['sip']), set(protocol_names))

    cm_params = cm_iface.GetParameters('sip')
    local_props = protocols['sip']
    local_params = local_props[cs.PROTOCOL + '.Parameters']
    assertEquals(cm_params, local_params)

    proto = bus.get_object(cm.bus_name, cm.object_path + '/sip')
    proto_iface = dbus.Interface(proto, cs.PROTOCOL)
    proto_prop_iface = dbus.Interface(proto, cs.PROPERTIES_IFACE)
    proto_props = unwrap(proto_prop_iface.GetAll(cs.PROTOCOL))

    for key in ['Parameters', 'Interfaces', 'ConnectionInterfaces',
      'RequestableChannelClasses', u'VCardField', u'EnglishName', u'Icon']:
        a = local_props[cs.PROTOCOL + '.' + key]
        b = proto_props[key]
        assertEquals(a, b)

    assertEquals('x-sip', proto_props['VCardField'])
    assertEquals('SIP', proto_props['EnglishName'])
    assertEquals('im-sip', proto_props['Icon'])

    assertContains(cs.CONN_IFACE_ALIASING, proto_props['ConnectionInterfaces'])
    assertContains(cs.CONN_IFACE_CONTACTS, proto_props['ConnectionInterfaces'])
    assertContains(cs.CONN_IFACE_REQUESTS, proto_props['ConnectionInterfaces'])

    assertEquals('sip:example@mit.edu',
        unwrap(proto_iface.NormalizeContact('example@MIT.Edu')))

    # Only account is mandatory
    call_async(q, proto_iface, 'IdentifyAccount', {})
    q.expect('dbus-error', method='IdentifyAccount', name=cs.INVALID_ARGUMENT)
    test_params = {'account': 'smcv@example.com'}
    acc_name = unwrap(proto_iface.IdentifyAccount(test_params))
    assertEquals('smcv@example.com', acc_name)

if __name__ == '__main__':
    exec_test(test)
