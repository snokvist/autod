{
  "cap":"link",
  "contract_version":"0.1",
  "commands":[
    {"name":"mode","description":"Report the currently selected downlink mode","args":[]},
    {"name":"select","description":"Set the active link mode","args":[{"key":"mode","type":"enum","control":{"kind":"select","options":["udp_relay","pixelpilot","none"],"multi":false},"required":true}]},
    {"name":"start","description":"Start the service associated with the active link mode","args":[]},
    {"name":"stop","description":"Stop the service associated with the active link mode","args":[]},
    {"name":"status","description":"Status of the active link mode","args":[]},
    {"name":"help","description":"Describe link orchestration commands","args":[]}
  ],
  "notes":[
    "Link mode persists via fw_setenv when available; otherwise a temporary state file under /tmp/vrx is used.",
    "Mode 'none' disables orchestration while keeping individual services reachable via their own caps."
  ]
}
