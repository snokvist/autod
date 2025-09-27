{
  "name": "vrx_gui_help",
  "description": "Stub help content for the VRX GUI handler.",
  "endpoints": {
    "/sys/gui/help": "Return this help payload.",
    "/sys/gui/sync": "Placeholder for syncing local autod capabilities to the GUI.",
    "/sys/gui/status": "Return lightweight status including last sync time.",
    "/sys/ping": "Proxy to the system ping command for diagnostics."
  },
  "notes": [
    "The sync endpoint will query the local autod instance for capability information in a future revision.",
    "State is cached under $VRX_GUI_STATE_DIR (default /tmp/vrx-gui)."
  ]
}
