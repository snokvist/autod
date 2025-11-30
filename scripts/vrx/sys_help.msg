{
  "cap":"sys",
  "contract_version":"0.1",
  "help_endpoints":[
    "/sys/help",
    "/sys/pixelpilot/help",
    "/sys/udp_relay/help",
    "/sys/joystick2crsf/help",
    "/sys/link/help",
    "/sys/link/wfb/help",
    "/sys/pixelpilot_mini_rk/help",
    "/sys/sync/help"
  ],
  "commands":[
    {"name":"help","description":"Describe general system commands","args":[]},
    {"name":"reboot","description":"Schedule a device reboot (background)","args":[]},
    {"name":"shutdown","description":"Schedule a device shutdown (background)","args":[]}
  ],
  "notes":[
    "Reboot triggers 'reboot now' after a short delay so the API response can complete.",
    "Shutdown issues 'shutdown now' in the background; ensure power management procedures are in place before using it."
  ]
}
