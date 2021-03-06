"""
Telepathy-Rakia testing framework
"""

import servicetest
from servicetest import (unwrap, Event)
import constants as cs

from twisted.protocols import sip
from twisted.internet import reactor

import os
import sys
import random

import dbus
import dbus.glib

class SipProxy(sip.RegisterProxy):
    registry = sip.InMemoryRegistry("127.0.0.1")

    def __init__(self, *args, **kw):
        sip.RegisterProxy.__init__(self, *args, **kw)

    def register(self, message, host, port):
        if hasattr(self, 'registrar_handler'):
            self.event_func(servicetest.Event('sip-register',
                uri=str(message.uri), headers=message.headers, body=message.body,
                sip_message=message, host=host, port=port))
            if self.registrar_handler(message, host, port):
                sip.RegisterProxy.register(self, message, host, port)
            else:
                self.unauthorized(message, host, port)

    def handle_request(self, message, addr):
        if message.method == 'REGISTER':
            return sip.RegisterProxy.handle_REGISTER_request(self, message, addr)
        elif message.method == 'OPTIONS' and \
                'REGISTRATION PROBE' == message.headers.get('subject','')[0]:
            self.deliverResponse(self.responseFromRequest(200, message))
        else:
            headers = {}
            for key, values in message.headers.items():
                headers[key.replace('-', '_')] = values[0]
            self.event_func(servicetest.Event('sip-%s' % message.method.lower(),
                uri=str(message.uri), headers=message.headers, body=message.body,
                sip_message=message, **headers))

    def handle_response(self, message, addr):
        headers = {}
        for key, values in message.headers.items():
            headers[key.replace('-', '_')] = values[0]
        self.event_func(servicetest.Event('sip-response',
            code=message.code, headers=message.headers, body=message.body,
            sip_message=message, **headers))

def prepare_test(event_func, register_cb, params=None):
    actual_params = {
        'account': 'testacc@127.0.0.1',
        'password': 'testpwd',
        'proxy-host': '127.0.0.1',
        'port': dbus.UInt16(random.randint(9090, 9999)),
        'local-ip-address': '127.0.0.1',
        'transport': 'udp'
    }

    if params is not None:
        for k, v in params.items():
            if v is None:
                actual_params.pop(k, None)
            else:
                actual_params[k] = v

    bus = dbus.SessionBus()
    conn = servicetest.make_connection(bus, event_func,
        'sofiasip', 'sip', actual_params)

    bus.add_signal_receiver(
        lambda *args, **kw:
            event_func(
                Event('dbus-signal',
                    path=unwrap(kw['path']),
                    signal=kw['member'], args=map(unwrap, args),
                    interface=kw['interface'])),
        None,       # signal name
        None,       # interface
        None,
        path_keyword='path',
        member_keyword='member',
        interface_keyword='interface',
        byte_arrays=True
        )

    port = int(actual_params['port'])
    sip = SipProxy(host=actual_params['proxy-host'], port=port)
    sip.event_func = event_func
    sip.registrar_handler = register_cb
    reactor.listenUDP(port, sip)
    return bus, conn, sip

def default_register_cb(message, host, port):
    return True

def go(params=None, register_cb=default_register_cb, start=None):
    handler = servicetest.EventTest()
    bus, conn, sip = \
        prepare_test(handler.handle_event, register_cb, params)
    handler.data = {
        'bus': bus,
        'conn': conn,
        'conn_iface': dbus.Interface(conn, cs.CONN),
        'sip': sip}
    handler.data['test'] = handler
    handler.data['sip'].test_handler = handler
    handler.verbose = (os.environ.get('CHECK_TWISTED_VERBOSE', '') != '')
    map(handler.expect, servicetest.load_event_handlers())

    if '-v' in sys.argv:
        handler.verbose = True

    if start is None:
        handler.data['conn'].Connect()
    else:
        start(handler.data)

    reactor.run()

def exec_test(fun, params=None, register_cb=default_register_cb, timeout=None):
    queue = servicetest.IteratingEventQueue(timeout)

    queue.verbose = (os.environ.get('CHECK_TWISTED_VERBOSE', '') != '')
    if '-v' in sys.argv:
        queue.verbose = True

    bus, conn, sip = prepare_test(queue.append,
        params=params, register_cb=register_cb)

    if sys.stdout.isatty():
        def red(s):
            return '\x1b[31m%s\x1b[0m' % s

        def green(s):
            return '\x1b[32m%s\x1b[0m' % s

        patterns = {
            'handled,': green,
            'not hand': red,
            }

        class Colourer:
            def __init__(self, fh, patterns):
                self.fh = fh
                self.patterns = patterns

            def write(self, s):
                f = self.patterns.get(s[:len('handled,')], lambda x: x)
                self.fh.write(f(s))
            
            def isatty(self):
                return False

        sys.stdout = Colourer(sys.stdout, patterns)

    try:
        fun(queue, bus, conn, sip)
    finally:
        try:
            conn.Disconnect()
            # second call destroys object
            conn.Disconnect()
        except dbus.DBusException, e:
            pass

