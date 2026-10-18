"""QEMU FS/HS/TD-level async-TT LAB wiring shared by the real import/WAL/editor suites.
USB_HUB_DEPTH=0 keeps the historical root-port tests unchanged. Async-TT LAB is not wire/timing qualification.
"""
import os,subprocess
DEPTH=int(os.environ.get('USB_HUB_DEPTH','0'))
assert 0<=DEPTH<=4
TT_LAB=os.environ.get('USB_TT_LAB')=='1'

def hub_model(controller):
    if controller in ('uhci','ohci'):
        assert not TT_LAB,'USB_TT_LAB needs EHCI'
        return 'usb-hub'
    assert controller in ('ehci','ehci-ohci') and os.environ.get('USB_HS_LAB')=='1', 'HS/TT LAB tests require explicit USB_HS_LAB=1'
    exe=os.environ.get('QEMU_SYSTEM_X86_64','qemu-system-x86_64')
    model='kvantos-lab-async-tt-hub' if TT_LAB else 'kvantos-lab-hs-hub'
    assert model.encode() in subprocess.check_output([exe,'-device','help']), 'requires explicit patched QEMU LAB model, not stock usb-hub'
    return model

def hub_device(controller,n,port,leaf=True):
    return f'{hub_model(controller)},id=hub{n},bus=usbtest.0,port={port}'+(f',lab-fs-ports={int(leaf)}' if TT_LAB else '')

def hub_args(controller,root=None):
    if not DEPTH:return []
    root=root or ('3' if controller=='ehci-ohci' else '1')
    return [v for n in range(DEPTH) for v in
            ('-device',hub_device(controller,n,root+".1"*n,n==DEPTH-1))]

def leaf_port(root='1'):
    return root+'.1'*DEPTH

def work(path):
    path=path/f'hub{DEPTH}' if DEPTH else path
    return path/'tt' if TT_LAB else path # keep AF_UNIX monitor/GDB paths <108 bytes
