{
  "cap":"link.wfb_ng",
  "contract_version":"0.2",
  "commands":[
    {"name":"get","description":"Read a WFB-NG parameter","args":[{"key":"name","type":"enum","control":{"kind":"select","options":["wlanpwr","wlanchan","wifi_mode"],"multi":false},"required":true}]},
    {"name":"set","description":"Set one WFB-NG parameter (key=value)","args":[{"key":"pair","type":"string","control":{"kind":"text"},"required":true}]},
    {"name":"params","description":"Set multiple WFB-NG parameters","args":[{"key":"pairs","type":"string","control":{"kind":"text"},"required":false}]},
    {"name":"start","description":"Start WFB-NG link","args":[]},
    {"name":"stop","description":"Stop WFB-NG link","args":[]},
    {"name":"restart","description":"Restart WFB-NG link","args":[]},
    {"name":"status","description":"Request WFB-NG status via SIGUSR1 to forker","args":[]},
    {"name":"help","description":"Describe WFB-NG controls","args":[]}
  ]
}
