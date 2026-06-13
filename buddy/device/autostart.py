"""Per-device boot config: which app a power-on boot lands in.

Present on this device by deliberate push — NOT part of the default
bundle (install_apps.py uploads root *.py, so don't commit this file
expecting menu-first behavior elsewhere; delete it from the device or
set APP = None to restore the menu on power-on). The launcher only
honors it on a non-soft reset, so exiting the app with ESC-ESC still
lands in the menu.
"""

APP = "ble_keyboard"
