{
  "cap":"link",
  "contract_version":"0.2",
  "commands":[
    {"name":"select","description":"Set wifi_mode (wfb_ng|ap|sta) which determines the active link","args":[{"key":"type","type":"enum","control":{"kind":"select","options":["wfb_ng","ap","sta"],"multi":false,"allow_free":true},"required":true}]},
    {"name":"start","description":"Start the active link determined by wifi_mode","args":[]},
    {"name":"stop","description":"Stop the active link determined by wifi_mode","args":[]},
    {"name":"status","description":"Status of the active link (placeholder)","args":[]},
    {"name":"help","description":"Describe overall link controls","args":[]}
  ]
}
