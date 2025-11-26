{
  "cap":"sys",
  "contract_version":"0.1",
  "help_endpoints":[
    {"path":"/sys/help","description":"System-level controls and discovery"},
    {"path":"/sys/link/help","description":"Link role selection and status"},
    {"path":"/sys/link/wifi/help","description":"Wi-Fi link configuration"},
    {"path":"/sys/link/wfb_ng/help","description":"WFB-NG link configuration"}
  ],
  "commands":[
    {"name":"help","description":"Describe overall system controls","args":[]},
    {"name":"shutdown","description":"Shut down the device immediately","args":[]},
    {"name":"restart","description":"Restart the device immediately","args":[]},
    {"name":"ping","description":"Send a single ICMP ping with a 1 second timeout","args":[{"key":"host","type":"string","required":true}]}
  ]
}
