{
  "cap":"link.wifi",
  "contract_version":"0.2",
  "commands":[
    {"name":"get","description":"Read a wifi parameter","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["wlanpwr","wlanpass","wlanchan","wlanssid","wifi_mode"],"multi":false},"required":true}]},
    {"name":"set","description":"Set one wifi parameter (key=value)","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true}]},
    {"name":"params","description":"Set multiple wifi parameters","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false}]},
    {"name":"start","description":"Start WiFi link","args":[]},
    {"name":"stop","description":"Stop WiFi link","args":[]},
    {"name":"status","description":"WiFi link status (placeholder)","args":[]},
    {"name":"help","description":"Describe WiFi controls","args":[]}
  ],
  "settings":[
    {"key":"wlanpwr","type":"int","control":{"kind":"range","min":100,"max":3100,"step":100},"description":"TX power (arbitrary units)"},
    {"key":"wlanpass","type":"string","control":{"kind":"select","options":["Enter value..."],"multi":false,"allow_free":true},"description":"WPA2 passphrase"},
    {"key":"wlanchan","type":"int","control":{"kind":"select","options":[36,40,44,48,149,153,157,161,165],"multi":false,"allow_free":false},"description":"Non-DFS 5GHz channel"},
    {"key":"wlanssid","type":"string","control":{"kind":"select","options":["Drone","Enter value..."],"multi":false,"allow_free":true},"description":"SSID"},
    {"key":"wifi_mode","type":"enum","control":{"kind":"select","options":["wfb_ng","ap","sta"],"multi":false,"allow_free":true},"description":"Active link type / mode"}
  ]
}
