{
  "cap":"sys",
  "contract_version":"0.1",
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
