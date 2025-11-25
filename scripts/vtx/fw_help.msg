{
  "cap":"fw",
  "contract_version":"0.1",
  "commands":[
    {"name":"help","description":"Describe firmware environment controls","args":[]},
    {"name":"set_link_mode","description":"Set firmware link mode (apfpv, wfb, or empty to clear)","args":[{"key":"mode","type":"enum","control":{"kind":"select","options":["apfpv","wfb",""],"multi":false,"allow_free":true},"required":false}]},
    {"name":"set_wlanssid","description":"Set Wi-Fi SSID (8-24 alphanumerics or empty to clear)","args":[{"key":"ssid","type":"string","control":{"kind":"text"},"required":false}]},
    {"name":"set_msposd_tty","description":"Select MSP/OSD serial device","args":[{"key":"tty","type":"enum","control":{"kind":"select","options":["standalone","/dev/ttyS1","/dev/ttyS2"],"multi":false},"required":true}]},
    {"name":"set_wlanpass","description":"Set Wi-Fi passphrase (8-24 alphanumerics or empty to clear)","args":[{"key":"pass","type":"string","control":{"kind":"text"},"required":false}]}
  ]
}
