#!/usr/bin/env python3
"""Give the A8's ADT the radio entries iOS looks for (xnu 0167 reads the Wi-Fi address from it).

  adt-radios.py IN.adt OUT.adt [WIFI_MAC]      default MAC 02:a8:20:18:5a:01 (locally administered)

/chosen: mac-address-wifi0, mac-address-bluetooth0 (Wi-Fi + 1), as iBoot sets them; MobileGestalt reads both.
/arm-io/wlan and /arm-io/bluetooth nodes (name, compatible, local-mac-address): launchd's _LimitLoadToDeviceTree
"wlan" (wifid) and MobileGestalt's IODeviceTree:/arm-io/wlan look for them. No reg/interrupts: nothing drives them.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt

src, dst = sys.argv[1], sys.argv[2]
mac = bytes.fromhex((sys.argv[3] if len(sys.argv) > 3 else '02:a8:20:18:5a:01').replace(':', ''))
bt = mac[:5] + bytes([(mac[5] + 1) & 0xff])
root, _ = adt.parse(open(src, 'rb').read())

def child(n, name):
    for c in n.children:
        if c.name == name:
            return c
    return None

def setp(n, k, v):
    for i, (pk, pf, pv) in enumerate(n.props):
        if pk == k:
            n.props[i] = (k, 0, v)
            return
    n.props.append((k, 0, v))

chosen, armio = child(root, 'chosen'), child(root, 'arm-io')
setp(chosen, 'mac-address-wifi0', mac)
setp(chosen, 'mac-address-bluetooth0', bt)
for name, compat, addr in (('wlan', b'wlan-sweet,a8\0', mac), ('bluetooth', b'bluetooth-sweet,a8\0', bt)):
    n = child(armio, name)
    if n is None:
        n = adt.Node()
        armio.children.append(n)
    setp(n, 'name', name.encode() + b'\0')
    setp(n, 'compatible', compat)
    setp(n, 'device_type', name.encode() + b'\0')
    setp(n, 'local-mac-address', addr)
open(dst, 'wb').write(adt.serialize(root))
print(f'{dst}: wifi {mac.hex(":")} bt {bt.hex(":")}, /arm-io/wlan + /arm-io/bluetooth')
